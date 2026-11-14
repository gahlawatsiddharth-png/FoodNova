// FoodNova ID camera - AI-Thinker ESP32-CAM (Arduino framework, C++)
// Board: "AI Thinker ESP32-CAM". The student holds their ID card (with its QR code) up to the camera.
// The photo is sent to the FoodNova server, where YOLOv8 + OpenCV read the ID. The server then links the next
// food drop on the bin to that student and sends them a notification.
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>

const char* WIFI_SSID  = "SCHOOL_WIFI";
const char* WIFI_PASS  = "WIFI_PASSWORD";
const char* SERVER     = "http://192.168.1.50:8000";  // IP of the PC running server.py
const char* DEVICE_KEY = "foodnova-device-key";       // must match DEVICE_KEY in server.py
const char* DEVICE_ID  = "bin1";                      // use the same id in the weighing-station firmware

#define TRIGGER_PIN 13     // push button (or IR sensor output) to GND when a card is held up
#define FLASH_PIN 4        // on-board white LED, used as a flash and as a status light
const bool AUTO_SCAN = false;           // true = keep trying every 2 s instead of waiting for the trigger

// AI-Thinker ESP32-CAM pin map
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

bool startCamera() {
  camera_config_t c;
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000; c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = psramFound() ? FRAMESIZE_SVGA : FRAMESIZE_VGA;   // bigger frames make small QR codes easier to read
  c.jpeg_quality = 10; c.fb_count = psramFound() ? 2 : 1;
  c.grab_mode = CAMERA_GRAB_LATEST;
  return esp_camera_init(&c) == ESP_OK;
}

void blink(int n) { for (int i = 0; i < n; i++) { digitalWrite(FLASH_PIN, HIGH); delay(120); digitalWrite(FLASH_PIN, LOW); delay(160); } }

// Returns true when the server found a student that matches the ID card.
bool scanOnce() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return false;
  bool matched = false;
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(String(SERVER) + "/api/scan");
    http.addHeader("Content-Type", "image/jpeg");
    http.addHeader("X-Device-Key", DEVICE_KEY);
    http.addHeader("X-Device-Id", DEVICE_ID);
    int code = http.POST(fb->buf, fb->len);
    if (code == 200) { String r = http.getString(); r.replace(" ", ""); matched = r.indexOf("\"matched\":true") >= 0; }
    http.end();
  }
  esp_camera_fb_return(fb);                    // always give the frame buffer back
  return matched;
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIGGER_PIN, INPUT_PULLUP); pinMode(FLASH_PIN, OUTPUT);
  if (!startCamera()) { while (true) blink(5); }          // 5 blinks forever = camera problem
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);
  blink(2);                                               // ready
}

void loop() {
  if (AUTO_SCAN || digitalRead(TRIGGER_PIN) == LOW) {
    digitalWrite(FLASH_PIN, HIGH); delay(150);            // light the card
    bool ok = false;
    for (int tries = 0; tries < 3 && !ok; tries++) { ok = scanOnce(); if (!ok) delay(400); }
    digitalWrite(FLASH_PIN, LOW);
    blink(ok ? 1 : 3);                                    // 1 blink = student found, 3 = try again
    delay(AUTO_SCAN ? 2000 : 1500);
  }
  delay(50);
}
