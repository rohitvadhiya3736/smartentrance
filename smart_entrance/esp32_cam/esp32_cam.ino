// ============================================================
//  SMART HOME ENTRANCE SYSTEM
//  ESP32-CAM + Face Recognition + Servo + Buzzer + Telegram
//  Board: AI Thinker ESP32-CAM
// ============================================================

#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// ─── USER CONFIG ─────────────────────────────────────────────
const char* WIFI_SSID     = "Esp32";
const char* WIFI_PASSWORD = "12345678";
const char* SERVER_URL    = "http://172.31.71.241:5000/detect-face";

const int CAPTURE_INTERVAL_MS  = 2000;
const int DOOR_UNLOCK_DURATION = 5000;  // 5 seconds

const int SERVO_LOCKED   = 0;    // 0°  = locked
const int SERVO_UNLOCKED = 90;   // 90° = unlocked
// ─────────────────────────────────────────────────────────────

// ─── CAMERA PINS ─────────────────────────────────────────────
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// ─── OUTPUT PINS ─────────────────────────────────────────────
#define SERVO_PIN   12
#define GREEN_LED   13
#define RED_LED     14
#define BUZZER_PIN  15
#define FLASH_LED    4
// ─────────────────────────────────────────────────────────────

Servo doorServo;
bool cameraReady  = false;
bool doorUnlocked = false;
unsigned long doorOpenTime = 0;

// ─────────────────────────────────────────────────────────────
void setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count     = 2;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("[CAM] Init failed!");
    cameraReady = false;
    return;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  } else {
    s->set_brightness(s, 1);
    s->set_contrast(s, 1);
    s->set_lenc(s, 1);
  }
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_gain_ctrl(s, 1);

  Serial.println("[CAM] Camera OK");
  cameraReady = true;
}

void connectWiFi() {
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int att = 0;
  while (WiFi.status() != WL_CONNECTED && att < 20) {
    delay(500); Serial.print("."); att++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] Failed - restarting");
    ESP.restart();
  }
}

// ─── Door Control ─────────────────────────────────────────────
void unlockDoor() {
  doorServo.write(SERVO_UNLOCKED);
  doorUnlocked = true;
  doorOpenTime = millis();
  Serial.println("[DOOR] UNLOCKED 🔓");
}

void lockDoor() {
  doorServo.write(SERVO_LOCKED);
  doorUnlocked = false;
  Serial.println("[DOOR] LOCKED 🔒");
}

void checkDoorTimeout() {
  if (doorUnlocked && millis() - doorOpenTime >= DOOR_UNLOCK_DURATION) {
    lockDoor();
    Serial.println("[DOOR] Auto-locked");
  }
}

// ─── Buzzer Patterns ──────────────────────────────────────────
void buzzerGranted() {
  // No beep for known person — silent access
}

void buzzerDenied() {
  // 3 fast + 1 long beep
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(80);
    digitalWrite(BUZZER_PIN, LOW);  delay(80);
  }
  delay(100);
  digitalWrite(BUZZER_PIN, HIGH); delay(500);
  digitalWrite(BUZZER_PIN, LOW);
}

void buzzerWaiting() {
  // Soft tick while waiting for owner
  digitalWrite(BUZZER_PIN, HIGH); delay(30);
  digitalWrite(BUZZER_PIN, LOW);
}

void buzzerStartup() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(50 + i * 50);
    digitalWrite(BUZZER_PIN, LOW);  delay(50);
  }
}

// ─── Access Responses ─────────────────────────────────────────
void accessGranted(String name, float conf) {
  Serial.println("╔══════════════════════════════╗");
  Serial.println("║      ✅ ACCESS GRANTED        ║");
  Serial.println("╠══════════════════════════════╣");
  Serial.println("║ Person    : " + name);
  Serial.println("║ Confidence: " + String(conf) + "%");
  Serial.println("╚══════════════════════════════╝");

  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED,   LOW);
  buzzerGranted();
  unlockDoor();
  delay(2000);
  digitalWrite(GREEN_LED, LOW);
}

void accessDenied() {
  Serial.println("╔══════════════════════════════╗");
  Serial.println("║      ❌ ACCESS DENIED         ║");
  Serial.println("║      UNKNOWN PERSON           ║");
  Serial.println("╚══════════════════════════════╝");

  for (int i = 0; i < 4; i++) {
    digitalWrite(RED_LED, HIGH); delay(150);
    digitalWrite(RED_LED, LOW);  delay(150);
  }
  buzzerDenied();
  lockDoor();
}

void waitingForOwner() {
  Serial.println("[PENDING] Waiting for owner...");
  // Alternate green/red while waiting
  for (int i = 0; i < 8; i++) {
    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(RED_LED,   LOW);
    buzzerWaiting();
    delay(500);
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED,   HIGH);
    delay(500);
  }
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED,   LOW);
}

// ─────────────────────────────────────────────────────────────
void captureAndSend() {
  if (!cameraReady) return;

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERR] Capture failed");
    esp_camera_deinit();
    setupCamera();
    return;
  }

  Serial.printf("[CAM] Captured %d bytes\n", fb->len);

  if (WiFi.status() != WL_CONNECTED) {
    esp_camera_fb_return(fb);
    connectWiFi();
    return;
  }

  // Both LEDs OFF while processing
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED,   LOW);

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(40000);  // 40s for Telegram wait

  int httpCode = http.POST(fb->buf, fb->len);
  esp_camera_fb_return(fb);

  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED,   LOW);

  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("[HTTP] " + payload);

    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, payload)) {
      String status   = doc["status"]     | "no_face";
      String person   = doc["person"]     | "Unknown";
      bool   is_known = doc["is_known"]   | false;
      float  conf     = doc["confidence"] | 0.0;

      if (status == "face_detected") {
        if (is_known) {
          accessGranted(person, conf);
        } else {
          accessDenied();
        }
      } else {
        Serial.println("[SCAN] No face - scanning...");
      }
    }
  } else if (httpCode == -1) {
    Serial.println("[HTTP] Connection failed");
  } else {
    Serial.printf("[HTTP] Error: %d\n", httpCode);
  }

  http.end();
}

// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(GREEN_LED,  OUTPUT);
  pinMode(RED_LED,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(FLASH_LED,  OUTPUT);

  digitalWrite(GREEN_LED,  LOW);
  digitalWrite(RED_LED,    LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(FLASH_LED,  LOW);

  doorServo.attach(SERVO_PIN);
  doorServo.write(SERVO_LOCKED);
  delay(500);

  Serial.println("\n╔══════════════════════════════════╗");
  Serial.println("║   SMART HOME ENTRANCE SYSTEM      ║");
  Serial.println("║   Face Recognition + Telegram     ║");
  Serial.println("║   Servo Door Lock + Buzzer        ║");
  Serial.println("╚══════════════════════════════════╝");

  setupCamera();
  connectWiFi();

  buzzerStartup();
  digitalWrite(GREEN_LED, HIGH); delay(300); digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED,   HIGH); delay(300); digitalWrite(RED_LED,   LOW);

  Serial.println("[READY] System Active! Door LOCKED.");
}

// ─────────────────────────────────────────────────────────────
void loop() {
  checkDoorTimeout();
  captureAndSend();
  delay(CAPTURE_INTERVAL_MS);
}
