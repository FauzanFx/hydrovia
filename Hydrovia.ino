// --- LIBRARIES ---
#include <WiFi.h>
#include <PubSubClient.h>
#include <NewPing.h>

// --- PIN DEFINITIONS ---
const int FLOW_SENSOR_1_PIN = 14;
const int FLOW_SENSOR_2_PIN = 19;
const int TRIG_PIN = 23;
const int ECHO_PIN = 22;
const int RELAY_PIN = 32; // Pin Relay

// --- WIFI & MQTT CONFIGURATION ---
const char* WIFI_SSID = "yatochief";
const char* WIFI_PASS = "TALUNkaton26";
const char* MQTT_SERVER = "192.168.137.1";
const int MQTT_PORT = 1883;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- FLOW CALCULATION ---
float PPL1 = 212.0; 
float PPL2 = 360.0; 

float flowRate1 = 0;
float totalVolume1 = 0;
float flowRate2 = 0;
float totalVolume2 = 0;

// --- GLOBAL VARIABLES ---
NewPing sonar(TRIG_PIN, ECHO_PIN, 100); 

bool systemLocked = false;
bool valveOpen = false;
unsigned long lastMsgTime = 0;
const unsigned long publishInterval = 2000; 

bool reset_pending = false;

volatile uint32_t pulseCount1 = 0;
volatile uint32_t pulseCount2 = 0;

// --- INTERRUPT SERVICE ROUTINES (ISRs) ---
void IRAM_ATTR pulseCounter1() { pulseCount1++; }
void IRAM_ATTR pulseCounter2() { pulseCount2++; }

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  Serial.println("HYDROVIA SYSTEM (ACTIVE LOW) Booting...");

  // Setup Relay
  pinMode(RELAY_PIN, OUTPUT);
  
  // --- PERUBAHAN 1: Default MATI adalah HIGH ---
  digitalWrite(RELAY_PIN, HIGH); 

  pinMode(FLOW_SENSOR_1_PIN, INPUT_PULLUP);
  pinMode(FLOW_SENSOR_2_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_1_PIN), pulseCounter1, FALLING);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_2_PIN), pulseCounter2, FALLING);

  setupWiFi();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

// --- LOOP ---
void loop() {
  if (WiFi.status() != WL_CONNECTED) setupWiFi();
  if (!mqttClient.connected()) reconnectMQTT();
  mqttClient.loop();

  unsigned long now = millis();
  if (now - lastMsgTime > publishInterval) {
    lastMsgTime = now;
    processDataAndPublish();

    // --- LOGIKA KONTROL RELAY (DIBALIK) ---
    if (systemLocked) {
      // Jika terkunci, paksa MATI (HIGH)
      digitalWrite(RELAY_PIN, LOW); 
      valveOpen = false;
    } else {
      // Jika valveOpen = true (Ingin Nyala) -> Kirim LOW
      // Jika valveOpen = false (Ingin Mati) -> Kirim HIGH
      digitalWrite(RELAY_PIN, valveOpen ? HIGH : LOW);
    }
  }
}

// --- PROCESS & PUBLISH ---
void processDataAndPublish() {
  int distance_cm = sonar.ping_median(5);
  distance_cm = sonar.convert_cm(distance_cm);
  if (distance_cm == 0) distance_cm = -1; 

  noInterrupts();
  uint32_t pulses1 = pulseCount1;
  uint32_t pulses2 = pulseCount2;
  pulseCount1 = 0;
  pulseCount2 = 0;
  interrupts();

  float interval_s = (float)publishInterval / 1000.0;
  if (interval_s > 0) {
    flowRate1 = ((float)pulses1 / PPL1) * (60.0 / interval_s);
    flowRate2 = ((float)pulses2 / PPL2) * (60.0 / interval_s);
  }

  if (flowRate1 < 0.1) flowRate1 = 0;
  if (flowRate2 < 0.1) flowRate2 = 0;

  float vol_inc1 = (float)pulses1 / PPL1;
  float vol_inc2 = (float)pulses2 / PPL2;

  if (valveOpen) {
    totalVolume1 += vol_inc1;
    totalVolume2 += vol_inc2;
  }

  char jsonBuffer[512];
  snprintf(jsonBuffer, sizeof(jsonBuffer),
           "{\"level_cm\":%d, \"rate1_lpm\":%.2f, \"volume_inc1_l\":%.4f, \"total_volume1_l\":%.4f, \"rate2_lpm\":%.2f, \"volume_inc2_l\":%.4f, \"total_volume2_l\":%.4f, \"valve_open\":%s}",
           distance_cm,
           flowRate1, vol_inc1, totalVolume1,
           flowRate2, vol_inc2, totalVolume2,
           valveOpen ? "true" : "false");

  Serial.print("Publishing: ");
  Serial.println(jsonBuffer);
  mqttClient.publish("hydrovia/alerts/sensordata", jsonBuffer);

  if (reset_pending) {
    totalVolume1 = 0;
    totalVolume2 = 0;
    reset_pending = false;
    Serial.println("Volume counters RESET via Command.");
  }
}

// --- WIFI & MQTT HELPERS ---
void setupWiFi() {
  delay(10);
  Serial.print("Connecting to "); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi Connected.");
  Serial.println(WiFi.localIP());
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';
  
  String t = String(topic);
  
  // === BAGIAN KONTROL VALVE ===
  if (t == "hydrovia/control/valve") {
    if (String(message) == "OPEN") {
      valveOpen = true;
      // JANGAN di-reset di sini kalau mau melanjutkan hitungan. 
      // Kalau mau setiap BUKA baru hitungan mulai dari 0, uncomment baris bawah:
      // reset_pending = true; 
    } else if (String(message) == "CLOSE") {
      valveOpen = false;
      // reset_pending = true;  <--- BARIS INI DIHAPUS. Jangan reset saat tutup.
    }
  }
  
  // === BAGIAN KUNCI MANUAL ===
  if (t == "hydrovia/control/lock") {
    if (String(message) == "true") systemLocked = true;
    else if (String(message) == "false") systemLocked = false;
  }

  // === BAGIAN RESET MANUAL (KHUSUS TOMBOL RESET) ===
  if (t == "hydrovia/control/reset") {
      reset_pending = true; // Hanya reset jika topik ini dipanggil
  }
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    String clientId = "hydrovia-esp32-" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      mqttClient.subscribe("hydrovia/control/valve");
      mqttClient.subscribe("hydrovia/control/lock");
      mqttClient.subscribe("hydrovia/control/reset");
    } else {
      delay(5000);
    }
  }
}