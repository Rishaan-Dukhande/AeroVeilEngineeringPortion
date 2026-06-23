#include <WiFi.h>
#include <Adafruit_MQTT.h>
#include <Adafruit_MQTT_Client.h>
#include <Adafruit_BME280.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <SensirionI2cSps30.h>
#include <SensirionI2cScd4x.h>

#include "secrets.h"


// ------- MQTT Setup -------
WiFiClient client;
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_PORT, AIO_USERNAME, AIO_KEY);

Adafruit_MQTT_Publish feed_aqi         = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/air-quality-sensor.aqi");
Adafruit_MQTT_Publish feed_temp        = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/air-quality-sensor.temperature");
Adafruit_MQTT_Publish feed_humidity    = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/air-quality-sensor.humidity");
Adafruit_MQTT_Publish feed_category   = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/air-quality-sensor.category");
Adafruit_MQTT_Publish feed_co2         = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/air-quality-sensor.co2");
Adafruit_MQTT_Publish feed_scd41_temp  = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/air-quality-sensor.scd41-temperature");
Adafruit_MQTT_Publish feed_scd41_humid = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/air-quality-sensor.scd41-humidity");

// ------- Sensors & Display -------
Adafruit_BME280   bme;
SensirionI2cSps30 sps30;                   // fixed: lowercase c
SensirionI2cScd4x scd41;
Adafruit_SH1107   display(64, 128, &Wire);

bool bmeFound   = false;
bool sps30Found = false;
bool scd41Found = false;
bool oledFound  = true;

int redPin   = 25;
int greenPin = 26;
int bluePin  = 27;

unsigned long lastPublish = 0;
const unsigned long publishInterval = 30000;

// ---- PM2.5 rolling average ----
// The SPS30 has a documented +/-5 ug/m3 noise floor at low concentrations,
// which is why readings jitter between values like 0.1 and 0.2 ug/m3 even
// in stable air. Averaging the last 5 readings smooths that noise out
// while still responding to real events (smoke, dust, etc).
const int PM25_HISTORY_SIZE = 5;
float pm25History[PM25_HISTORY_SIZE] = {0, 0, 0, 0, 0};
int pm25HistoryIndex = 0;
bool pm25HistoryFilled = false;

float addPM25Reading(float newReading) {
  pm25History[pm25HistoryIndex] = newReading;
  pm25HistoryIndex = (pm25HistoryIndex + 1) % PM25_HISTORY_SIZE;
  if (pm25HistoryIndex == 0) pm25HistoryFilled = true;

  int count = pm25HistoryFilled ? PM25_HISTORY_SIZE : pm25HistoryIndex;
  if (count == 0) return newReading; // first reading ever, nothing to average yet

  float sum = 0;
  for (int i = 0; i < count; i++) sum += pm25History[i];
  return sum / count;
}

// ================================================================
// WiFi
// ================================================================
void connectWiFi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" WiFi connected");
}

// ================================================================
// MQTT
// ================================================================
void connectMQTT() {
  Serial.print("Connecting to Adafruit IO MQTT...");
  while (mqtt.connect() != 0) {
    Serial.println("Retrying MQTT connection in 5 seconds...");
    delay(5000);
  }
  Serial.println(" MQTT connected");
}

// ================================================================
// AQI Calculator
// ================================================================
int calculate_aqi(float pm25_val, String &category) {
  float aqiDecimal; // the precise, unrounded result - lets us SEE the math working
  int aqi;

  if (pm25_val <= 12.0) {
    aqiDecimal = (pm25_val - 0.0) / (12.0 - 0.0) * (50 - 0) + 0;
    category = "Good";
  } else if (pm25_val <= 35.4) {
    aqiDecimal = (pm25_val - 12.0) / (35.4 - 12.0) * (100 - 51) + 51;
    category = "Moderate";
  } else if (pm25_val <= 55.4) {
    aqiDecimal = (pm25_val - 35.5) / (55.4 - 35.5) * (150 - 101) + 101;
    category = "Unhealthy for Sensitive Groups";
  } else if (pm25_val <= 150.4) {
    aqiDecimal = (pm25_val - 55.5) / (150.4 - 55.5) * (200 - 151) + 151;
    category = "Unhealthy";
  } else if (pm25_val <= 250.4) {
    aqiDecimal = (pm25_val - 150.5) / (250.4 - 150.5) * (300 - 201) + 201;
    category = "Very Unhealthy";
  } else {
    aqiDecimal = (pm25_val - 250.5) / (500.0 - 250.5) * (500 - 301) + 301;
    category = "Hazardous";
  }

  Serial.printf("  AQI math -> PM2.5:%.2f ug/m3  =>  AQI (decimal):%.2f  =>  AQI (rounded):%d\n",
                pm25_val, aqiDecimal, (int)aqiDecimal);

  aqi = (int)aqiDecimal;
  return constrain(aqi, 0, 500);
}

// ================================================================
// RGB LED
// ================================================================
void updateLED(String category) {
  if (category == "Good") {
    analogWrite(redPin, 0);   analogWrite(greenPin, 255); analogWrite(bluePin, 0);
  } else if (category == "Moderate") {
    analogWrite(redPin, 183); analogWrite(greenPin, 255); analogWrite(bluePin, 0);
  } else if (category == "Unhealthy for Sensitive Groups") {
    analogWrite(redPin, 183); analogWrite(greenPin, 200); analogWrite(bluePin, 0);
  } else if (category == "Unhealthy") {
    analogWrite(redPin, 183); analogWrite(greenPin, 0);   analogWrite(bluePin, 0);
  } else if (category == "Very Unhealthy") {
    analogWrite(redPin, 183); analogWrite(greenPin, 0);   analogWrite(bluePin, 200);
  } else if (category == "Hazardous") {
    analogWrite(redPin, 91);  analogWrite(greenPin, 0);   analogWrite(bluePin, 0);
  }
}

// ================================================================
// OLED
// ================================================================
void updateOLED(int aqi, float bme_temp, float bme_humid,
                float co2, float pm25_raw) {
  if (!oledFound) return;

  display.clearDisplay();

  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("AQI: ");
  display.println(aqi);

  display.setTextSize(1);

  display.setCursor(0, 28);
  display.print("Temp: ");
  display.print(bme_temp, 1);
  display.println("F");

  display.setCursor(0, 38);
  display.print("Hum:  ");
  display.print(bme_humid, 1);
  display.println("%");

  display.setCursor(0, 48);
  if (scd41Found) {
    display.print("CO2:  ");
    display.print((int)co2);
    display.println("ppm");
  } else {
    display.println("CO2:  waiting...");
  }

  display.setCursor(0, 58);
  if (sps30Found) {
    display.print("PM25: ");
    display.print(pm25_raw, 1);
    display.println("ug");
  } else {
    display.println("PM25: waiting...");
  }

  display.display();
}

// ================================================================
// Setup
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22);
  delay(500);

  connectWiFi();
  connectMQTT();

  // BME280
  if (!bme.begin(0x76) && !bme.begin(0x77)) {
    Serial.println("WARNING: BME280 not found!");
  } else {
    bmeFound = true;
    Serial.println("OK: BME280 initialized");
  }

  // SPS30
  sps30.begin(Wire, SPS30_I2C_ADDR_69);
  int16_t spsErr = sps30.startMeasurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_UINT16);
  if (spsErr) {
    Serial.println("SPS30: Not connected yet (waiting for JST cable)");
    sps30Found = false;
  } else {
    sps30Found = true;
    Serial.println("OK: SPS30 fan spinning up...");
    delay(2000);
    Serial.println("OK: SPS30 ready");
  }

  // SCD41
  scd41.begin(Wire, SCD41_I2C_ADDR_62);
  uint16_t scdErr = scd41.stopPeriodicMeasurement();
  delay(500);
  scdErr = scd41.startPeriodicMeasurement();
  if (scdErr) {
    Serial.println("SCD41: Not connected yet (wire when received)");
    scd41Found = false;
  } else {
    scd41Found = true;
    Serial.println("OK: SCD41 initialized — warming up (5 seconds)...");
    delay(5000);
    Serial.println("OK: SCD41 ready");
  }

  // OLED
  if (!display.begin(0x3C, true)) {
    Serial.println("WARNING: OLED not found at 0x3C");
    while (1) delay(10);
  }
  Serial.println("OK: OLED initialized");
  display.setContrast(255);
  display.clearDisplay();
  display.setRotation(1);
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("Air Monitor Ready");
  display.print("SPS30: ");
  display.println(sps30Found ? "OK" : "waiting");
  display.print("SCD41: ");
  display.println(scd41Found ? "OK" : "waiting");
  display.display();

  // RGB LED
  pinMode(redPin,   OUTPUT);
  pinMode(greenPin, OUTPUT);
  pinMode(bluePin,  OUTPUT);

  Serial.println("Setup complete. Publishing every 2 minutes.");
  Serial.println("--------------------------------------------");
}

// ================================================================
// Loop
// ================================================================
void loop() {
  mqtt.processPackets(10);
  mqtt.ping();

  unsigned long now = millis();
  if (now - lastPublish >= publishInterval) {
    lastPublish = now;

    // BME280
    float bme_temp  = bmeFound ? (bme.readTemperature() * 1.8 + 32) : 0;
    float bme_humid = bmeFound ? bme.readHumidity() : 0;

    // SPS30
    float pm25Value = 0;
    if (sps30Found) {
      uint16_t dataReadyFlag = 0;
      uint16_t mc1p0, mc2p5, mc4p0, mc10p0;
      uint16_t nc0p5, nc1p0, nc2p5, nc4p0, nc10p0;
      uint16_t typicalSize;

      sps30.readDataReadyFlag(dataReadyFlag);

      if (dataReadyFlag) {
        int16_t readErr = sps30.readMeasurementValuesUint16(
          mc1p0, mc2p5, mc4p0, mc10p0,
          nc0p5, nc1p0, nc2p5, nc4p0, nc10p0,
          typicalSize
        );

        if (readErr) {
          Serial.print("SPS30 read error: ");
          Serial.println(readErr);
        } else {
          // uint16 values are scaled x10, divide to get ug/m3
          float pm25Raw = mc2p5 / 10.0;
          pm25Value = addPM25Reading(pm25Raw); // smoothed average of last 5 readings

          Serial.printf("SPS30 -> PM1.0:%.3f  PM2.5(raw):%.3f  PM2.5(avg):%.3f  PM4.0:%.3f  PM10:%.3f ug/m3\n",
                        mc1p0/10.0, pm25Raw, pm25Value, mc4p0/10.0, mc10p0/10.0);
        }
      } else {
        Serial.println("SPS30: data not ready yet");
      }
    }

    // SCD41
    float co2Value  = 0;
    float scd_temp  = 0;
    float scd_humid = 0;
    if (scd41Found) {
      uint16_t co2_raw;
      float scd_t, scd_h;
      bool dataReady = false;

      uint16_t readyErr = scd41.getDataReadyStatus(dataReady);
      if (readyErr) {
        Serial.print("SCD41 ready-check error: ");
        Serial.println(readyErr);
      } else if (!dataReady) {
        Serial.println("SCD41: measurement not ready yet");
      } else {
        uint16_t readErr = scd41.readMeasurement(co2_raw, scd_t, scd_h);
        if (readErr) {
          Serial.print("SCD41 read error: ");
          Serial.println(readErr);
        } else {
          co2Value  = co2_raw;
          scd_temp  = (scd_t * 1.8) + 32;
          scd_humid = scd_h;
          Serial.printf("SCD41 -> CO2:%d ppm | Temp:%.1fF | Hum:%.1f%%\n",
                        co2_raw, scd_temp, scd_humid);
          if (bmeFound) {
            Serial.printf("  Temp check -> BME280:%.1fF  SCD41:%.1fF  Diff:%.1fF\n",
                          bme_temp, scd_temp, abs(bme_temp - scd_temp));
          }
        }
      }
    }

    // AQI
    String category;
    int aqi = calculate_aqi(pm25Value, category);

    Serial.printf("AQI:%d | BME Temp:%.1fF | BME Hum:%.1f%% | Cat:%s\n",
                  aqi, bme_temp, bme_humid, category.c_str());

    // Publish existing feeds
    char buf[10];
    sprintf(buf, "%d", aqi);
    feed_aqi.publish(buf);

    dtostrf(bme_temp, 4, 2, buf);
    feed_temp.publish(buf);

    dtostrf(bme_humid, 4, 2, buf);
    feed_humidity.publish(buf);

    feed_category.publish(category.c_str());

    // Publish SCD41 feeds only when connected and valid
    if (scd41Found && co2Value > 0) {
      dtostrf(co2Value, 6, 1, buf);
      feed_co2.publish(buf);

      dtostrf(scd_temp, 4, 2, buf);
      feed_scd41_temp.publish(buf);

      dtostrf(scd_humid, 4, 2, buf);
      feed_scd41_humid.publish(buf);
    }

    // Update outputs
    updateOLED(aqi, bme_temp, bme_humid, co2Value, pm25Value);
    updateLED(category);
  }
}