#include <Wire.h>
#include <Adafruit_ADS1X15.h>

Adafruit_ADS1115 ads;

#define I2C_SDA 22
#define I2C_SCL 23

const float CALIBRATION = 50.0; 
const float VOLTS_PER_BIT = 0.0000625; // Para GAIN_TWO (+/- 2.048V)

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!ads.begin(0x48)) {
    Serial.println("Error al inicializar ADS1115");
    while (1);
  }

  ads.setGain(GAIN_TWO);
  ads.setDataRate(RATE_ADS1115_860SPS);
}

// Función genérica para calcular Irms de un canal diferencial específico
double readIrmsChannel(uint8_t channel, int numSamples) {
  double sumSq = 0.0;

  for (int i = 0; i < numSamples; i++) {
    int16_t rawDiff = 0;
    
    if (channel == 0) {
      rawDiff = ads.readADC_Differential_0_1(); // Lee A0 - A1 (Fase)
    } else if (channel == 1) {
      rawDiff = ads.readADC_Differential_2_3(); // Lee A2 - A3 (Neutro)
    }

    float voltage = rawDiff * VOLTS_PER_BIT;
    float current = voltage * CALIBRATION;
    sumSq += (current * current);
  }

  double irms = sqrt(sumSq / numSamples);
  if (irms < 0.02) irms = 0.0; // Umbral de ruido basal (20 mA)
  
  return irms;
}

void loop() {
  // Muestreo de 250 muestras por canal (~300 ms por canal a 860 SPS)
  double iFase = readIrmsChannel(0, 250);
  double iNeutro = readIrmsChannel(1, 250);
  
  double iDiferencial = fabs(iFase - iNeutro);

  Serial.print("I_Fase: ");
  Serial.print(iFase, 3);
  Serial.print(" A | I_Neutro: ");
  Serial.print(iNeutro, 3);
  Serial.print(" A | I_Fuga (aprox): ");
  Serial.print(iDiferencial, 3);
  Serial.println(" A");

  delay(100);
}