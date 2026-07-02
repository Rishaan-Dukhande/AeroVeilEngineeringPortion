// ================================================================
// AeroVeil — Relay Controller
// Second ESP32, sits near fan/anywhere in house
// Subscribes to Adafruit IO relay-command feed via MQTT
// Controls Digital Loggers IoT Power Relay on GPIO 26
// Manual override via BOOT button (GPIO 0)
// ================================================================

#include <WiFi.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"
#include "secrets.h"

// ---- Adafruit IO ----
#define AIO_SERVER  "io.adafruit.com"
#define AIO_PORT    1883

// ---- Pins ----
const int RELAY_PIN    = 26;  // signal wire to Digital Loggers + terminal
const int OVERRIDE_BTN = 0;   // built-in BOOT button on ESP32 DevKit

// ---- MQTT ----
WiFiClient client;
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_PORT, AIO_USERNAME, AIO_KEY);

// Feed we SUBSCRIBE to — main AeroVeil board publishes here
Adafruit_MQTT_Subscribe feed_relay_cmd =
  Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME "/feeds/air-quality-sensor.relay-command");

// Feed we PUBLISH to — reports current relay state back to dashboard
Adafruit_MQTT_Publish feed_relay_state =
  Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/air-quality-sensor.relay-state");

// ---- State tracking ----
bool relayOn         = false;  // current relay state
bool manualOverride  = false;  // true = ignore AQI commands, respect manual toggle only
bool lastBtnState    = HIGH;   // for button debounce
unsigned long lastBtnPress = 0;
const unsigned long DEBOUNCE_MS = 200;

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
  Serial.println(" connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// ================================================================
// MQTT
// ================================================================
void connectMQTT() {
  Serial.print("Connecting to Adafruit IO...");
  while (mqtt.connect() != 0) {
    Serial.println(" retrying in 5s...");
    delay(5000);
  }
  Serial.println(" connected!");
}

// ================================================================
// Relay control
// ================================================================
void setRelay(bool on, const char* reason) {
  relayOn = on;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);

  // Report state back to Adafruit IO dashboard
  feed_relay_state.publish(on ? "ON" : "OFF");

  Serial.printf("RELAY: %s  (reason: %s)\n", on ? "ON" : "OFF", reason);
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== AeroVeil Relay Controller starting ===");

  // Relay pin
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // start with relay OFF
  Serial.println("Relay pin initialized LOW (fan off)");

  // Manual override button
  // GPIO 0 is the built-in BOOT button — no extra wiring needed
  pinMode(OVERRIDE_BTN, INPUT_PULLUP);

  // Connect
  connectWiFi();
  connectMQTT();

  // Subscribe to the relay command feed
  mqtt.subscribe(&feed_relay_cmd);
  Serial.println("Subscribed to relay-command feed");
  Serial.println("=== Setup complete — waiting for AQI commands ===");
}

// ================================================================
// LOOP
// ================================================================
void loop() {

  // Keep MQTT alive
  if (!mqtt.connected()) {
    Serial.println("MQTT disconnected — reconnecting...");
    connectMQTT();
  }
  mqtt.processPackets(200);
  mqtt.ping();

  // ---- Check for incoming MQTT message ----
  Adafruit_MQTT_Subscribe *subscription;
  while ((subscription = mqtt.readSubscription(100))) {
    if (subscription == &feed_relay_cmd) {
      String cmd = String((char *)feed_relay_cmd.lastread);
      cmd.trim();
      Serial.printf("Received command: %s\n", cmd.c_str());

      if (!manualOverride) {
        // Normal mode — follow AQI commands from main board
        if (cmd == "ON") {
          setRelay(true, "AQI command");
        } else if (cmd == "OFF") {
          setRelay(false, "AQI command");
        } else {
          Serial.printf("Unknown command: %s — ignored\n", cmd.c_str());
        }
      } else {
        // Manual override active — ignore AQI commands
        Serial.println("Manual override active — AQI command ignored");
      }
    }
  }

  // ---- Manual override button check ----
  // Press the BOOT button to toggle manual override on/off
  // First press: activates manual override AND toggles relay
  // Second press: deactivates manual override, relay returns to AQI control
  bool btnState = digitalRead(OVERRIDE_BTN);
  if (btnState == LOW && lastBtnState == HIGH) {
    unsigned long now = millis();
    if (now - lastBtnPress > DEBOUNCE_MS) {
      lastBtnPress = now;

      if (!manualOverride) {
        // Entering manual override — toggle relay
        manualOverride = true;
        setRelay(!relayOn, "manual override activated");
        Serial.println("Manual override ON — AQI commands suspended");
      } else {
        // Exiting manual override — return control to AQI
        manualOverride = false;
        Serial.println("Manual override OFF — AQI commands resumed");
        // Note: relay stays in current state until next AQI command arrives
      }
    }
  }
  lastBtnState = btnState;
}