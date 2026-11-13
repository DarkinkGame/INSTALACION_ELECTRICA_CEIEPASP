#include <Wire.h>
#include <Adafruit_ADS1X15.h>

Adafruit_ADS1115 ads;

#define I2C_SDA 5
#define I2C_SCL 23
#define PIN_ZMPT 1          // GPIO1 (A1) en la ESP32-C6 Pulsar

// Factores de calibración
const float CALIBRATION_SCT = 50.0;    // 50A / 1V RMS
const float VOLTS_PER_BIT_ADS = 0.0000625; // GAIN_TWO (+/- 2.048V)

// Ajustar este valor comparando con un multímetro en AC
float CALIBRATION_VOLT = 466.78; 

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  if (!ads.begin(0x48)) {
    Serial.println("Error al inicializar ADS1115");
    while (1);
  }

  ads.setGain(GAIN_TWO);
  ads.setDataRate(RATE_ADS1115_860SPS);
}

// Lectura RMS del ZMPT101B en el ESP32-C6
double readVoltageRMS(int numSamples) {
  // 1. Determinar el offset DC del ZMPT (~2048 para 1.65V)
  long sumRaw = 0;
  for (int i = 0; i < 300; i++) {
    sumRaw += analogRead(PIN_ZMPT);
    delayMicroseconds(50);
  }
  float offset = sumRaw / 300.0;

  // 2. Muestreo de la onda senoidal de voltaje
  double sumSq = 0.0;
  for (int i = 0; i < numSamples; i++) {
    float centered = analogRead(PIN_ZMPT) - offset;
    float vInst = (centered / 4095.0) * 3.3;
    float vAC = vInst * CALIBRATION_VOLT;
    sumSq += (vAC * vAC);
    delayMicroseconds(100);
  }

  double vRMS = sqrt(sumSq / numSamples);
  if (vRMS < 10.0) vRMS = 0.0; // Umbral de corte sin red
  return vRMS;
}

// Lectura RMS diferencial en ADS1115 para SCT
double readIrmsADS(uint8_t channel, int numSamples) {
  double sumSq = 0.0;
  for (int i = 0; i < numSamples; i++) {
    int16_t rawDiff = (channel == 0) ? ads.readADC_Differential_0_1() : ads.readADC_Differential_2_3();
    float voltage = rawDiff * VOLTS_PER_BIT_ADS;
    float current = voltage * CALIBRATION_SCT;
    sumSq += (current * current);
  }
  double irms = sqrt(sumSq / numSamples);
  if (irms < 0.02) irms = 0.0;
  return irms;
}

void loop() {
  double vRed = readVoltageRMS(1000);
  double iFase = readIrmsADS(0, 250);
  double iNeutro = readIrmsADS(1, 250);
  
  double potenciaAparente = vRed * iFase;

  Serial.println("--- Mediciones AC ---");
  Serial.print("Voltaje Red:     "); Serial.print(vRed, 1); Serial.println(" V");
  Serial.print("Corriente Fase:  "); Serial.print(iFase, 3); Serial.println(" A");
  Serial.print("Corriente Neutro:"); Serial.print(iNeutro, 3); Serial.println(" A");
  Serial.print("Potencia Aparent:"); Serial.print(potenciaAparente, 1); Serial.println(" VA");
  Serial.println();

  delay(100);
}