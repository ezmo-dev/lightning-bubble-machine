/*
 Lightning Bubble Machine
 ESP32 DevKit (WROOM-32) + x2 5V Relays module with optocoupler (ON/OFF)
 *
 How it works:
   1. Payment received via BTCPay WebSocket
   2. Pulse GPIO_ON (500ms) -> machine starts
   3. Wait BUBBLE_DURATION seconds
   4. Pulse GPIO_OFF (500ms) -> machine stops
 *
 Wiring:
    GPIO 21 -> IN1 relay (ON button on remote)
    GPIO 19 -> IN2 relay (OFF button on remote)
    5V (VIN) -> VCC relay
    GND -> GND relay
 *
License: MIT
  https://github.com/ezmo-dev/lightning-bubble-machine
 */

#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

// --- Configuration ---

const char* WIFI_SSID     = "YOUR_WIFI";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";

const char* WSS_HOST = "YOUR_BTCPAY_DOMAIN";
const char* WSS_PATH = "/apps/YOUR_POS_ID/pos/bitcoinswitch";

const int GPIO_ON  = 21;   // Relay 1 - ON button
const int GPIO_OFF = 19;   // Relay 2 - OFF button

const int PULSE_DURATION   = 500;   // Pulse length in ms
const int BUBBLE_DURATION  = 10;    // Bubble time in seconds

// --- Internal (do not modify) ---

#define LED_PIN 2

const unsigned long HEARTBEAT_INTERVAL = 30000;
unsigned long lastHeartbeat = 0;

const unsigned long UPTIME_LOG_INTERVAL = 300000;
unsigned long lastUptimeLog = 0;
unsigned long startTime = 0;

WebSocketsClient webSocket;
bool wsConnected = false;

bool machineRunning = false;
unsigned long machineStopTime = 0;

// --- Setup ---

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("Lightning Bubble Machine v1.0");
    Serial.println();

    pinMode(LED_PIN, OUTPUT);
    pinMode(GPIO_ON, OUTPUT);
    pinMode(GPIO_OFF, OUTPUT);

    digitalWrite(GPIO_ON, LOW);
    digitalWrite(GPIO_OFF, LOW);
    digitalWrite(LED_PIN, LOW);

    Serial.println("GPIO ON:  " + String(GPIO_ON));
    Serial.println("GPIO OFF: " + String(GPIO_OFF));
    Serial.println("Bubble duration: " + String(BUBBLE_DURATION) + "s");

    setupWifi();

    Serial.println("Connecting to " + String(WSS_HOST) + String(WSS_PATH));
    webSocket.beginSSL(WSS_HOST, 443, WSS_PATH);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);
    webSocket.enableHeartbeat(25000, 10000, 2);

    startTime = millis();
    Serial.println("Ready, waiting for payment...");
}

// --- Loop ---

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected, reconnecting...");
        wsConnected = false;
        setupWifi();
    }

    webSocket.loop();

    if (wsConnected && (millis() - lastHeartbeat > HEARTBEAT_INTERVAL)) {
        webSocket.sendTXT("ping");
        lastHeartbeat = millis();
    }

    if (millis() - lastUptimeLog > UPTIME_LOG_INTERVAL) {
        logUptime();
        lastUptimeLog = millis();
    }

    if (machineRunning && millis() >= machineStopTime) {
        stopBubbleMachine();
    }
}

// --- WiFi ---

void setupWifi() {
    Serial.print("Connecting to " + String(WIFI_SSID));

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(" OK");
        Serial.println("IP: " + WiFi.localIP().toString());
        digitalWrite(LED_PIN, HIGH);
    } else {
        Serial.println(" FAILED");
        digitalWrite(LED_PIN, LOW);
        delay(5000);
        ESP.restart();
    }
}

// --- WebSocket ---

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.println("WS disconnected");
            wsConnected = false;
            digitalWrite(LED_PIN, LOW);
            break;

        case WStype_CONNECTED:
            Serial.println("WS connected");
            wsConnected = true;
            lastHeartbeat = millis();
            digitalWrite(LED_PIN, HIGH);
            break;

        case WStype_TEXT:
            handleWebSocketMessage(payload, length);
            break;

        case WStype_ERROR:
            Serial.println("WS error");
            break;

        case WStype_PING:
        case WStype_PONG:
            break;
    }
}

void handleWebSocketMessage(uint8_t * payload, size_t length) {
    String message = String((char*)payload);
    Serial.println("Message received: " + message);

    if (message == "pong" || message == "ping") {
        return;
    }

    // JSON format (BTCPay payment notification)
    if (message.startsWith("{")) {
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, message);

        if (!error) {
            if (doc.containsKey("paid") && doc["paid"] == true) {
                Serial.println("Payment confirmed (JSON)");
                startBubbleMachine();
                return;
            }
        }
    }

    // BitcoinSwitch format: "pin-duration" (e.g. "21-5000")
    int dashIndex = message.indexOf('-');
    if (dashIndex > 0) {
        int pin = message.substring(0, dashIndex).toInt();
        int duration = message.substring(dashIndex + 1).toInt();

        Serial.println("Trigger: pin " + String(pin) + ", duration " + String(duration) + "ms");

        if (pin == GPIO_ON || pin > 0) {
            startBubbleMachine();
        }
        return;
    }

    // Any other non-empty message triggers the machine
    if (message.length() > 0 && message != "connected") {
        Serial.println("Signal received, starting machine");
        startBubbleMachine();
    }
}

// --- Bubble machine control ---

void startBubbleMachine() {
    if (machineRunning) {
        Serial.println("Machine already running, ignoring");
        return;
    }

    Serial.println("Payment received!");

    // Pulse ON button
    Serial.println("Pressing ON (GPIO " + String(GPIO_ON) + ")");
    digitalWrite(GPIO_ON, HIGH);
    delay(PULSE_DURATION);
    digitalWrite(GPIO_ON, LOW);
    Serial.println("Machine started");

    // Schedule stop
    machineRunning = true;
    machineStopTime = millis() + (BUBBLE_DURATION * 1000UL);
    Serial.println("Will stop in " + String(BUBBLE_DURATION) + "s");

    // Blink LED to confirm
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, LOW);
        delay(100);
        digitalWrite(LED_PIN, HIGH);
        delay(100);
    }
}

void stopBubbleMachine() {
    Serial.println("Time's up, stopping machine...");

    // Pulse OFF button
    Serial.println("Pressing OFF (GPIO " + String(GPIO_OFF) + ")");
    digitalWrite(GPIO_OFF, HIGH);
    delay(PULSE_DURATION);
    digitalWrite(GPIO_OFF, LOW);

    machineRunning = false;
    Serial.println("Machine stopped");
    Serial.println("Waiting for next payment...");
}

// --- Utilities ---

void logUptime() {
    unsigned long uptime = (millis() - startTime) / 60000;
    Serial.println("Uptime: " + String(uptime) + "min | WiFi: " +
                   (WiFi.status() == WL_CONNECTED ? "OK" : "NOK") +
                   " | WS: " + (wsConnected ? "OK" : "NOK") +
                   " | Machine: " + (machineRunning ? "ON" : "OFF") +
                   " | Heap: " + String(ESP.getFreeHeap()));
}
