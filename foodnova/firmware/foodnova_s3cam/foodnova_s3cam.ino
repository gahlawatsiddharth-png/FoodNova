#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_http_server.h"
#include <lwip/sockets.h>

// ================= 1. WI-FI & PYTHON SERVER SETTINGS =================
const char *WIFI_SSID     = "YOUR_WIFI_SSID";          // Optional: Put your Wi-Fi / Hotspot name here
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";      // Optional: Put your Wi-Fi password here
const char *SERVER_URL    = "http://192.168.4.2:8000"; // Works on ESP32-S3-CAM Hotspot! (Or change to your Laptop IP)
const char *DEVICE_KEY    = "foodnova-device-key";

const char *AP_SSID       = "ESP32-S3-CAM";
const char *AP_PASSWORD   = "12345678";

// ================= 2. CAMERA PINS (FROM YOUR WORKING SKETCH) =================
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM   4
#define SIOC_GPIO_NUM   5

#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM     8
#define Y3_GPIO_NUM     9
#define Y2_GPIO_NUM    11

#define VSYNC_GPIO_NUM  6
#define HREF_GPIO_NUM   7
#define PCLK_GPIO_NUM  13

// Peripherals on free ESP32-S3 pins
#define HX_DT          47   // HX711 DT pin (for when you wire load cell later)
#define HX_SCK         48   // HX711 SCK pin (for when you wire load cell later)
#define BOOT_BTN_PIN    0   // Built-in BOOT button on ESP32-S3-CAM (press to trigger plate drop)

#define PART_BOUNDARY "esp32camera_boundary"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";

static httpd_handle_t index_httpd = nullptr;
static httpd_handle_t stream_httpd = nullptr;

bool hx711Ready = false;
float calibFactor = 420.0;
long zeroOffset = 0;

bool systemArmed = false;
String activeStudent = "";
uint32_t lastPollMs = 0;
volatile float currentFps = 0.0;

// ================= 3. BUILT-IN NATIVE HX711 DRIVER (NO LIBRARY NEEDED) =================
bool hx711WaitReady(uint32_t timeoutMs = 200) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (digitalRead(HX_DT) == LOW) return true;
    delay(1);
  }
  return false;
}

long hx711ReadSingle() {
  if (!hx711WaitReady(150)) return 0;
  unsigned long value = 0;
  uint8_t data[3] = {0};

  noInterrupts();
  for (uint8_t j = 0; j < 3; j++) {
    for (uint8_t i = 0; i < 8; i++) {
      digitalWrite(HX_SCK, HIGH);
      delayMicroseconds(1);
      data[j] |= (digitalRead(HX_DT) << (7 - i));
      digitalWrite(HX_SCK, LOW);
      delayMicroseconds(1);
    }
  }
  digitalWrite(HX_SCK, HIGH);
  delayMicroseconds(1);
  digitalWrite(HX_SCK, LOW);
  interrupts();

  if (data[0] & 0x80) {
    value = ((unsigned long)0xFF << 24) | ((unsigned long)data[0] << 16) | ((unsigned long)data[1] << 8) | (unsigned long)data[2];
  } else {
    value = ((unsigned long)data[0] << 16) | ((unsigned long)data[1] << 8) | (unsigned long)data[2];
  }
  return (long)value;
}

long hx711ReadAverage(uint8_t samples = 5) {
  long sum = 0;
  for (uint8_t i = 0; i < samples; i++) {
    sum += hx711ReadSingle();
  }
  return sum / samples;
}

// ================= 4. WEB PAGE (RAW UNFILTERED VIDEO) =================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-S3 Camera</title>
  <style>
    body { background: #111; color: #eee; text-align: center; font-family: Arial, sans-serif; margin: 0; padding: 18px; }
    h2 { margin: 4px 0 10px; }
    .wrap { position: relative; display: inline-block; width: 100%; max-width: 800px; }
    img { display: block; width: 100%; height: auto; margin: auto; border-radius: 10px; background: #000; }
    .badge { position: absolute; top: 12px; left: 12px; background: rgba(0,0,0,0.7); color: #4ade80; padding: 5px 11px; border-radius: 6px; font-weight: bold; font-size: 0.85rem; }
    .bar { display: flex; gap: 10px; justify-content: center; flex-wrap: wrap; margin-top: 14px; }
    select, .btn { background: #222; color: #eee; border: 1px solid #444; padding: 9px 14px; border-radius: 8px; font-size: 0.9rem; cursor: pointer; text-decoration: none; }
  </style>
</head>
<body>
  <h2>ESP32-S3 Camera (Natural Sensor Stream)</h2>
  <div class="wrap">
    <div class="badge" id="fps">Live Stream</div>
    <img id="camera" alt="Live camera stream">
  </div>
  <div class="bar">
    <select id="res" onchange="tune()">
      <option value="8" selected>640 x 480 (VGA - Max FPS)</option>
      <option value="9">800 x 600 (SVGA)</option>
      <option value="10">1024 x 768 (XGA)</option>
      <option value="11">1280 x 720 (HD)</option>
    </select>
    <select id="qual" onchange="tune()">
      <option value="8">Quality 8 (Sharpest)</option>
      <option value="10" selected>Quality 10 (Balanced)</option>
      <option value="12">Quality 12 (Fastest FPS)</option>
    </select>
    <a class="btn" href="/capture" target="_blank">Snapshot (/capture)</a>
  </div>
  <script>
    document.getElementById("camera").src = "http://" + location.hostname + ":81/stream";
    async function tune() {
      const r = document.getElementById("res").value;
      const q = document.getElementById("qual").value;
      await fetch(`/control?res=${r}&qual=${q}`);
    }
    setInterval(async () => {
      try {
        const r = await fetch("/status");
        const j = await r.json();
        document.getElementById("fps").textContent = j.fps + " FPS";
      } catch (e) {}
    }, 1200);
  </script>
</body>
</html>
)rawliteral";

// ================= 5. HTTP HANDLERS =================
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t status_handler(httpd_req_t *req) {
  char out[64];
  snprintf(out, sizeof(out), "{\"fps\":%.1f}", currentFps);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, out, strlen(out));
}

static esp_err_t control_handler(httpd_req_t *req) {
  char buf[64];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
      char val[16];
      if (httpd_query_key_value(buf, "res", val, sizeof(val)) == ESP_OK) {
        int res = atoi(val);
        if (res >= 5 && res <= 12) s->set_framesize(s, (framesize_t)res);
      }
      if (httpd_query_key_value(buf, "qual", val, sizeof(val)) == ESP_OK) {
        int q = atoi(val);
        if (q >= 4 && q <= 30) s->set_quality(s, q);
      }
    }
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "OK", 2);
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t res = httpd_resp_send(req, reinterpret_cast<const char *>(fb->buf), fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t result = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (result != ESP_OK) return result;

  int fd = httpd_req_to_sockfd(req);
  int nodelay = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  char header[128];
  uint32_t fpsTimer = millis();
  uint16_t frameCount = 0;

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      delay(1);
      continue;
    }

    if (fb->format != PIXFORMAT_JPEG || fb->len == 0) {
      esp_camera_fb_return(fb);
      return ESP_FAIL;
    }

    int headerLength = snprintf(
        header, sizeof(header),
        "%sContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
        STREAM_BOUNDARY, fb->len
    );

    result = httpd_resp_send_chunk(req, header, headerLength);
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(fb->buf), fb->len);
    }

    esp_camera_fb_return(fb);
    if (result != ESP_OK) return result;

    frameCount++;
    uint32_t now = millis();
    if (now - fpsTimer >= 1000) {
      currentFps = (frameCount * 1000.0f) / (now - fpsTimer);
      frameCount = 0;
      fpsTimer = now;
    }

    delay(1);
  }
}

static bool startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.stack_size = 8192;
  config.max_uri_handlers = 8;

  httpd_uri_t index_uri   = {.uri = "/",        .method = HTTP_GET, .handler = index_handler,   .user_ctx = nullptr};
  httpd_uri_t status_uri  = {.uri = "/status",  .method = HTTP_GET, .handler = status_handler,  .user_ctx = nullptr};
  httpd_uri_t control_uri = {.uri = "/control", .method = HTTP_GET, .handler = control_handler, .user_ctx = nullptr};
  httpd_uri_t capture_uri = {.uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = nullptr};
  httpd_uri_t stream_uri  = {.uri = "/stream",  .method = HTTP_GET, .handler = stream_handler,  .user_ctx = nullptr};

  if (httpd_start(&index_httpd, &config) != ESP_OK) return false;
  httpd_register_uri_handler(index_httpd, &index_uri);
  httpd_register_uri_handler(index_httpd, &status_uri);
  httpd_register_uri_handler(index_httpd, &control_uri);
  httpd_register_uri_handler(index_httpd, &capture_uri);

  config.server_port = 81;
  config.ctrl_port = 32769;
  if (httpd_start(&stream_httpd, &config) != ESP_OK) return false;
  httpd_register_uri_handler(stream_httpd, &stream_uri);

  return true;
}

// ================= 6. NATURAL CAMERA INIT (EXACT MATCH TO YOUR ORIGINAL SKETCH) =================
static bool initCamera() {
  bool hasPsram = psramFound();

  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;

  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;

  c.pin_xclk     = XCLK_GPIO_NUM;
  c.pin_pclk     = PCLK_GPIO_NUM;
  c.pin_vsync    = VSYNC_GPIO_NUM;
  c.pin_href     = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn     = PWDN_GPIO_NUM;
  c.pin_reset    = RESET_GPIO_NUM;

  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;

  if (hasPsram) {
    c.frame_size   = FRAMESIZE_VGA;
    c.jpeg_quality = 10;
    c.fb_count     = 2;
    c.fb_location  = CAMERA_FB_IN_PSRAM;
    c.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    c.frame_size   = FRAMESIZE_VGA;
    c.jpeg_quality = 12;
    c.fb_count     = 1;
    c.fb_location  = CAMERA_FB_IN_DRAM;
    c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK && !hasPsram) {
    c.frame_size = FRAMESIZE_CIF;
    err = esp_camera_init(&c);
  }
  if (err != ESP_OK) {
    Serial.printf("Camera initialization failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    esp_camera_deinit();
    return false;
  }

  Serial.printf("Sensor PID: 0x%04X\n", s->id.PID);

  // Exact neutral sensor settings from finalvideostream.ino — zero artificial color changes
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);

  s->set_exposure_ctrl(s, 1);
  s->set_gain_ctrl(s, 1);
  s->set_ae_level(s, 0);

  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_special_effect(s, 0);

  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);

  delay(500);
  for (int i = 0; i < 5; ++i) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(50);
  }

  return true;
}

// ================= 7. FOODNOVA AI & SCALE TRIGGER =================
String myActiveIP() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return WiFi.softAPIP().toString();
}

void sendWasteAndTriggerAI(int grams) {
  if (grams <= 0) grams = 150;
  Serial.printf("[SCALE] Plate weighed: %d g -> Sending to FoodNova AI...\n", grams);

  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/waste");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);

  String camIp = myActiveIP();
  String body = "{\"grams\":" + String(grams) + ",\"cam_ip\":\"" + camIp + "\"}";
  int code = http.POST(body);
  String resp = http.getString();
  Serial.printf("[SERVER %d] %s\n", code, resp.c_str());
  http.end();

  if (hx711Ready && hx711WaitReady(200)) {
    zeroOffset = hx711ReadAverage(8);
  }
  systemArmed = false;
  activeStudent = "";
}

// ================= 8. SETUP & LOOP =================
void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  pinMode(HX_DT, INPUT);
  pinMode(HX_SCK, OUTPUT);
  digitalWrite(HX_SCK, LOW);

  Serial.println("\nESP32-S3 camera starting");
  Serial.printf("PSRAM: %u bytes; free heap: %u bytes\n",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());

  if (!initCamera()) {
    Serial.println("Camera failed. Check wiring, power and PSRAM.");
    while (true) delay(1000);
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD, 1, 0, 4);

  if (String(WIFI_SSID) != "YOUR_WIFI_SSID" && strlen(WIFI_SSID) > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
      delay(400);
      Serial.print(".");
    }
    Serial.println();
  }

  WiFi.setSleep(false);

  if (!startCameraServer()) {
    Serial.println("HTTP server failed");
    while (true) delay(1000);
  }

  if (hx711WaitReady(250)) {
    long testVal = hx711ReadSingle();
    if (testVal != 0 && testVal != -1) {
      hx711Ready = true;
      zeroOffset = hx711ReadAverage(8);
    }
  }

  Serial.printf("SSID: %s | Password: %s\n", AP_SSID, AP_PASSWORD);
  Serial.printf("Open http://%s/\n", myActiveIP().c_str());
}

void loop() {
  // Press BOOT button on board to trigger Food Drop + AI Snapshot
  if (digitalRead(BOOT_BTN_PIN) == LOW) {
    delay(60);
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
      sendWasteAndTriggerAI(165);
      while (digitalRead(BOOT_BTN_PIN) == LOW) delay(20);
    }
  }

  // Or type 'w' or weight in Serial Monitor
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("w")) sendWasteAndTriggerAI(180);
    else if (cmd.toInt() > 0) sendWasteAndTriggerAI(cmd.toInt());
  }

  // Poll server if connected to router
  if (!systemArmed && millis() - lastPollMs >= 2500) {
    lastPollMs = millis();
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.setTimeout(600);
      http.begin(String(SERVER_URL) + "/api/station/status");
      http.addHeader("X-Device-Key", DEVICE_KEY);
      if (http.GET() == 200) {
        String payload = http.getString();
        if (payload.indexOf("\"active\": true") >= 0 || payload.indexOf("\"active\":true") >= 0) {
          systemArmed = true;
        }
      }
      http.end();
    }
  }

  // Read HX711 if connected
  if (systemArmed && hx711Ready && hx711WaitReady(80)) {
    float w = (hx711ReadAverage(3) - zeroOffset) / calibFactor;
    if (w > 15.0) {
      delay(2500);
      int finalGrams = (int)((hx711ReadAverage(6) - zeroOffset) / calibFactor);
      if (finalGrams > 10) sendWasteAndTriggerAI(finalGrams);
    }
  }

  delay(10);
}