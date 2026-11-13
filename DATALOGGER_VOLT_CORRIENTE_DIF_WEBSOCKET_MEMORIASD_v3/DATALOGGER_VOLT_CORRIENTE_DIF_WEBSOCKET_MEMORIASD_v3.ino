#include <ArduinoJson.h>
#include <ArduinoJson.hpp>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <time.h>
#include <sys/time.h>
SemaphoreHandle_t sdMutex;
// --- Credenciales SoftAP ---
const char* AP_SSID = "Monitor-Energia";
const char* AP_PASS = "12345678";

// --- Hardware & Pines ---
#define I2C_SDA      5
#define I2C_SCL      23
#define PIN_ZMPT     1
#define SD_CS_PIN    19

// --- Constantes de Calibración ---
const float CALIBRATION_SCT   = 50.0;
const float VOLTS_PER_BIT_ADS = 0.0000625;
const float CALIBRATION_VOLT  = 450;
const float EMA_ALPHA         = 0.15;

// --- Configuración de Archivos ---
// Lapso de rotación en segundos (ej. 1800 = 30 min, 3600 = 1 hora)
const unsigned int TIEMPO_GUARDADO_MIN = 30;
const unsigned long FILE_ROTATION_SEC = TIEMPO_GUARDADO_MIN*60; 

struct Medicion {
  char timestamp_str[24];
  float voltaje;
  float i_fase;
  float i_neutro;
  float i_fuga;
  float potencia_aparente;
};

Adafruit_ADS1115 ads;
QueueHandle_t medicionesQueue;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

float vRMS_filtrado = 0.0;
String currentFileName = "";
unsigned long fileStartTime = 0;
bool timeSynchronized = false;

// Obtener string de fecha/hora formateada
String getFormattedTime() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

// Generar nombre de archivo rotativo
String generateNewFileName() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "/log_%Y-%m-%d_%H-%M.csv", &timeinfo);
  return String(buffer);
}
void sendFileListViaWS(AsyncWebSocketClient *client) {
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (SD.exists("/index.txt")) {
      File idx = SD.open("/index.txt", FILE_READ);
      if (idx) {
        JsonDocument doc;
        JsonArray arr = doc["fileList"].to<JsonArray>();
        
        while (idx.available()) {
          String line = idx.readStringUntil('\n');
          line.trim();
          if (line.length() > 0) {
            arr.add(line);
          }
        }
        idx.close();

        String out;
        serializeJson(doc, out);
        client->text(out);
      }
    } else {
      // Si aún no hay índice, enviar lista vacía
      client->text("{\"fileList\":[]}");
    }
    xSemaphoreGive(sdMutex);
  }
}

// Inicializar nuevo archivo CSV y registrarlo en el índice
void initNewLogFile() {
  currentFileName = generateNewFileName();
  fileStartTime = millis();
  
  // 1. Crear el nuevo archivo de datos
  File f = SD.open(currentFileName.c_str(), FILE_WRITE);
  if (f) {
    f.println("Fecha_Hora,Voltaje_V,I_Fase_A,I_Neutro_A,I_Fuga_A,Potencia_VA");
    f.close();
    Serial.println("Nuevo archivo creado: " + currentFileName);
  }

  // 2. Anotar el nombre en index.txt (sin la diagonal inicial para la URL)
  String cleanName = currentFileName.startsWith("/") ? currentFileName.substring(1) : currentFileName;
  File idx = SD.open("/index.txt", FILE_APPEND);
  if (idx) {
    idx.println(cleanName);
    idx.close();
    Serial.println("Registrado en index.txt: " + cleanName);
  }

  // 3. Notificar de inmediato al celular vía WebSocket si está abierto
  if (ws.count() > 0) {
    JsonDocument doc;
    doc["newFile"] = cleanName;
    String out;
    serializeJson(doc, out);
    ws.textAll(out);
  }
}
// HTML con sincronización horaria automática y explorador de archivos
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Monitor Eléctrico</title>
  <style>
    body { font-family: -apple-system, sans-serif; background: #121418; color: #e0e6ed; margin: 0; padding: 12px; }
    .container { max-width: 600px; margin: 0 auto; }
    .card { background: #1c1f26; border-radius: 8px; padding: 12px; margin-bottom: 10px; border: 1px solid #2d333b; text-align: center; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
    .val { font-size: 1.7rem; font-weight: bold; }
    .v-val { color: #00e676; } .if-val { color: #29b6f6; } .in-val { color: #ab47bc; } .fug-val { color: #ef5350; } .p-val { color: #ffa726; }
    .file-item { display: flex; justify-content: space-between; align-items: center; padding: 8px 4px; border-bottom: 1px solid #2d333b; font-size: 0.9rem; }
    .btn { background: #008080; color: white; padding: 6px 12px; border-radius: 4px; text-decoration: none; font-size: 0.85rem; font-weight: bold; }
    #time-status { font-size: 0.8rem; color: #8b949e; margin-top: 4px; }
  </style>
</head>
<body>
  <div class="container">
    <h2 style="text-align:center; margin: 8px 0;">Monitor Eléctrico Autónomo</h2>
    <div id="time-status" style="text-align:center;">Sincronizando hora...</div>

    <div class="card" style="margin-top:10px;">
      <div>Voltaje RMS</div>
      <div id="volt" class="val v-val">-- V</div>
    </div>
    <div class="grid">
      <div class="card">
        <div>I Fase RMS</div>
        <div id="ifase" class="val if-val">-- A</div>
      </div>
      <div class="card">
        <div>I Neutro RMS</div>
        <div id="ineutro" class="val in-val">-- A</div>
      </div>
    </div>
    <div class="grid">
      <div class="card">
        <div>I Fuga</div>
        <div id="ifuga" class="val fug-val">-- A</div>
      </div>
      <div class="card">
        <div>Potencia</div>
        <div id="power" class="val p-val">-- W</div>
      </div>
    </div>

    <div class="card">
      <h3 style="margin: 4px 0 10px 0;">Archivos en Memoria MicroSD</h3>
      <div id="file-list">Cargando registros...</div>
    </div>
  </div>

<script>
    const gateway = `ws://${window.location.hostname}/ws`;
    let websocket;
    let knownFiles = [];

    function renderFileList() {
      const container = document.getElementById('file-list');
      if (knownFiles.length === 0) {
        container.innerHTML = '<p style="color:#8b949e;">No hay archivos registrados aún.</p>';
        return;
      }
      let html = '';
      // Mostramos los más recientes primero (orden invertido)
      for (let i = knownFiles.length - 1; i >= 0; i--) {
        const name = knownFiles[i];
        html += `<div class="file-item">
                  <span><b>${name}</b></span>
                  <a class="btn" href="/download?file=${encodeURIComponent(name)}" download>Descargar</a>
                 </div>`;
      }
      container.innerHTML = html;
    }

    function initWebSocket() {
      websocket = new WebSocket(gateway);
      websocket.onopen = function() {
        const localEpoch = Math.floor(Date.now() / 1000);
        websocket.send(JSON.stringify({ setTime: localEpoch }));
      };
      websocket.onmessage = function(event) {
        const data = JSON.parse(event.data);
        
        // Recibir lista inicial de archivos
        if (data.fileList !== undefined) {
          knownFiles = data.fileList;
          renderFileList();
        }
        // Recibir notificación de archivo nuevo rotado
        else if (data.newFile !== undefined) {
          if (!knownFiles.includes(data.newFile)) {
            knownFiles.push(data.newFile);
            renderFileList();
          }
        }
        // Hora sincronizada
        else if (data.timeSync) {
          document.getElementById('time-status').innerText = "Hora sincronizada: " + data.clock;
        }
        // Mediciones en tiempo real
        else if (data.v !== undefined) {
          document.getElementById('volt').innerText = Number(data.v).toFixed(1) + " V";
          document.getElementById('ifase').innerText = Number(data.if).toFixed(3) + " A";
          document.getElementById('ineutro').innerText = Number(data.in).toFixed(3) + " A";
          document.getElementById('ifuga').innerText = Number(data.ifug).toFixed(3) + " A";
          document.getElementById('power').innerText = Number(data.p).toFixed(1) + " VA";
        }
      };
      websocket.onclose = function() { setTimeout(initWebSocket, 2000); };
    }

    window.onload = initWebSocket;
  </script>
</body>
</html>
)rawliteral";

// --- Muestreo de magnitudes ---
double readVoltageRMS(int /*unused*/) {
  const unsigned long WINDOW_US = 100000;
  long sumRaw = 0;
  for (int i = 0; i < 400; i++) {
    sumRaw += analogRead(PIN_ZMPT);
    delayMicroseconds(25);
  }
  float offset = sumRaw / 400.0;

  double sumSq = 0.0;
  long sampleCount = 0;
  unsigned long startMicros = micros();

  while (micros() - startMicros < WINDOW_US) {
    float centered = analogRead(PIN_ZMPT) - offset;
    float vInst = (centered / 4095.0) * 3.3 * CALIBRATION_VOLT;
    sumSq += (vInst * vInst);
    sampleCount++;
    delayMicroseconds(10);
  }

  if (sampleCount == 0) return 0.0;
  double vRMS_inst = sqrt(sumSq / sampleCount);
  if (vRMS_inst < 15.0) vRMS_inst = 0.0;

  if (vRMS_filtrado == 0.0) {
    vRMS_filtrado = vRMS_inst;
  } else {
    vRMS_filtrado = (EMA_ALPHA * vRMS_inst) + ((1.0 - EMA_ALPHA) * vRMS_filtrado);
  }
  return vRMS_filtrado;
}

double readIrmsADS(uint8_t channel, int numSamples) {
  double sumSq = 0.0;
  for (int i = 0; i < numSamples; i++) {
    int16_t rawDiff = (channel == 0) ? ads.readADC_Differential_0_1() : ads.readADC_Differential_2_3();
    float voltage = rawDiff * VOLTS_PER_BIT_ADS;
    float current = voltage * CALIBRATION_SCT;
    sumSq += (current * current);
  }
  double irms = sqrt(sumSq / numSamples);
  return (irms < 0.02) ? 0.0 : irms;
}

// --- TAREA 1: Muestreo continuo ---
void TaskSampling(void *pvParameters) {
  for (;;) {
    double vRed = readVoltageRMS(0);
    double iFase = readIrmsADS(0, 200);
    double iNeutro = readIrmsADS(1, 200);
    double pAparente = vRed * iFase;
    double iFuga = fabs(iFase - iNeutro);

    Medicion med;
    String tStr = getFormattedTime();
    strncpy(med.timestamp_str, tStr.c_str(), sizeof(med.timestamp_str));
    med.voltaje = round(vRed * 10.0) / 10.0;
    med.i_fase = round(iFase * 1000.0) / 1000.0;
    med.i_neutro = round(iNeutro * 1000.0) / 1000.0;
    med.i_fuga = round(iFuga * 1000.0) / 1000.0;
    med.potencia_aparente = round(pAparente * 10.0) / 10.0;

    xQueueSend(medicionesQueue, &med, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// --- TAREA 2: MicroSD con Rotación y WebSockets ---
void TaskLoggingAndComms(void *pvParameters) {
  Medicion med;
  unsigned long lastSDWrite = 0;
  String dataBuffer = "";

  for (;;) {
    if (xQueueReceive(medicionesQueue, &med, portMAX_DELAY) == pdPASS) {
      
      // 1. WebSocket & Serial
      JsonDocument doc;
      doc["v"] = med.voltaje;
      doc["if"] = med.i_fase;
      doc["in"] = med.i_neutro;
      doc["ifug"] = med.i_fuga;
      doc["p"] = med.potencia_aparente;
      
      String jsonStr;
      serializeJson(doc, jsonStr);
      Serial.print("[TELEMETRIA] ");
      Serial.println(jsonStr);

      if (ws.count() > 0) {
        ws.textAll(jsonStr);
      }

      // 2. Acumulación CSV con Fecha y Hora
      dataBuffer += String(med.timestamp_str) + "," +
                    String(med.voltaje, 1) + "," +
                    String(med.i_fase, 3) + "," +
                    String(med.i_neutro, 3) + "," +
                    String(med.i_fuga, 3) + "," +
                    String(med.potencia_aparente, 1) + "\n";

      // 3. Revisar rotación de archivo
      if ((millis() - fileStartTime) >= (FILE_ROTATION_SEC * 1000UL)) {
        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          if (dataBuffer.length() > 0 && currentFileName != "") {
            File f = SD.open(currentFileName.c_str(), FILE_APPEND);
            if (f) { f.print(dataBuffer); f.close(); }
            dataBuffer = "";
          }
          initNewLogFile();
          xSemaphoreGive(sdMutex);
        }
      }

      // 4. Escritura periódica a SD cada 5 segundos
      if (millis() - lastSDWrite >= 5000) {
        if (currentFileName != "") {
          if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            File file = SD.open(currentFileName.c_str(), FILE_APPEND);
            if (file) {
              file.print(dataBuffer);
              file.close();
              dataBuffer = "";
              lastSDWrite = millis();
            }
            xSemaphoreGive(sdMutex);
          }
        }
      }
    }
  }
}

// Manejo de eventos WebSocket (recibe sincronización de hora)
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("Cliente WebSocket #%u conectado. Enviando lista de archivos...\n", client->id());
    sendFileListViaWS(client);
  }
  else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      JsonDocument doc;
      deserializeJson(doc, data, len);
      if (doc.containsKey("setTime")) {
        time_t epoch = doc["setTime"];
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        tzset();
        timeSynchronized = true;

        Serial.printf("Reloj sincronizado con el navegador: %ld\n", epoch);

        if (currentFileName.startsWith("/log_1970")) {
          if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            initNewLogFile();
            xSemaphoreGive(sdMutex);
          }
          // Reenviar lista actualizada
          sendFileListViaWS(client);
        }

        JsonDocument res;
        res["timeSync"] = true;
        res["clock"] = getFormattedTime();
        String out;
        serializeJson(res, out);
        client->text(out);
      }
    }
  }
}

void setupServerRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", INDEX_HTML);
  });

  // Endpoint robusto para listar archivos CSV
  server.on("/list-files", HTTP_GET, [](AsyncWebServerRequest *request){
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    Serial.println("[SD] Solicitud de listado de archivos...");

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      File root = SD.open("/");
      if (!root) {
        Serial.println("[SD ERROR] No se pudo abrir la raíz '/'");
      } else {
        File file = root.openNextFile();
        while (file) {
          if (!file.isDirectory()) {
            String fname = String(file.name());
            
            // Limpiar diagonales iniciales si existen
            while (fname.startsWith("/")) {
              fname = fname.substring(1);
            }

            // Filtrar archivos de sistema de Windows/Mac y aceptar solo CSV
            if (!fname.startsWith(".") && !fname.startsWith("_") && (fname.endsWith(".csv") || fname.endsWith(".CSV"))) {
              JsonObject obj = arr.add<JsonObject>();
              obj["name"] = fname;
              
              size_t bytes = file.size();
              if (bytes < 1024) {
                obj["size"] = String(bytes) + " B";
              } else {
                obj["size"] = String(bytes / 1024.0, 1) + " KB";
              }
              Serial.printf("[SD] Encontrado: %s (%u bytes)\n", fname.c_str(), bytes);
            }
          }
          file.close();
          file = root.openNextFile();
        }
        root.close();
      }
      xSemaphoreGive(sdMutex);
    } else {
      Serial.println("[SD ERROR] Timeout esperando sdMutex en /list-files");
    }

    String response;
    serializeJson(doc, response);
    Serial.print("[SD JSON Enviado] ");
    Serial.println(response);

    AsyncWebServerResponse *res = request->beginResponse(200, "application/json", response);
    res->addHeader("Access-Control-Allow-Origin", "*");
    res->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    request->send(res);
  });

  // Descarga protegida con el Mutex
  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String fName = "/" + request->getParam("file")->value();
      
      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1500)) == pdTRUE) {
        bool exists = SD.exists(fName.c_str());
        xSemaphoreGive(sdMutex);
        
        if (exists) {
          request->send(SD, fName.c_str(), "text/csv", true);
          return;
        }
      }
    }
    request->send(404, "text/plain", "Archivo no encontrado.");
  });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();
}

void setup() {
  Serial.begin(115200);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
    // Configurar zona horaria para México Central (UTC-6 todo el año)
  setenv("TZ", "CST6", 1);
  tzset();

  Wire.begin(I2C_SDA, I2C_SCL);
  if (ads.begin(0x48)) {
    ads.setGain(GAIN_TWO);
    ads.setDataRate(RATE_ADS1115_860SPS);
    Serial.println("ADS1115 listo.");
  }
  sdMutex = xSemaphoreCreateMutex();

  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("Error al montar MicroSD.");
  } else {
    Serial.println("MicroSD inicializada.");
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      initNewLogFile();
      xSemaphoreGive(sdMutex);
    }
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("SoftAP activo en: ");
  Serial.println(WiFi.softAPIP());

  setupServerRoutes();

  medicionesQueue = xQueueCreate(20, sizeof(Medicion));

  xTaskCreatePinnedToCore(TaskSampling, "SamplingTask", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TaskLoggingAndComms, "CommsTask", 4096, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelete(NULL);
}