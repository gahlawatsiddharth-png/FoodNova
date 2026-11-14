// FoodNova disposal station - ESP32 firmware (Arduino framework, C++)
// Libraries (Library Manager): HX711 by bogde, LiquidCrystal_I2C, ESP32Servo
#include <WiFi.h>
#include <HTTPClient.h>
#include <HX711.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

const char* WIFI_SSID  = "SCHOOL_WIFI";
const char* WIFI_PASS  = "WIFI_PASSWORD";
const char* SERVER     = "http://192.168.1.50:8000";  // IP of the PC running server.py
const char* DEVICE_KEY = "foodnova-device-key";       // must match DEVICE_KEY in server.py
const char* DEVICE_ID  = "bin1";                      // same id as in the ID-camera firmware

// Pins: change to match your wiring
#define HX_DT 4
#define HX_SCK 5
#define IR_PIN 6          // IR proximity sensor, LOW = something near the opening
#define SERVO_PIN 7
#define MOTOR_EN 15
#define MOTOR_IN1 16
#define MOTOR_IN2 17
const int BTN[3] = {8, 9, 10};   // optional feedback buttons wired to GND (INPUT_PULLUP)

// The processing module (flap + motor) is optional and must stay physically separate from the
// electronics, away from wet food, and supervised. It is OFF until you decide to use it.
const bool ENABLE_PROCESSING = false;

float zeroOffset = 0;
float calibFactor = 420.0;       // set with a known mass: factor = (raw - zero) / known_grams
const int MIN_GRAMS = 5;         // below this there is no valid weighing event
const int STABLE_BAND = 3;       // stable = last STABLE_SAMPLES readings differ by <= 3 g
const int STABLE_SAMPLES = 6;
const unsigned long SETTLE_TIMEOUT = 8000;
const float CONF_MIN = 0.6;      // below this the category is sent as Unknown / Mixed
const char* REASONS[3] = {"Too much served", "Did not like it", "Not hungry"};
String studentId = "";           // leave blank: the server links the drop to the student who just scanned their ID on the ESP32-CAM

HX711 scale;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo flap;
uint32_t bootId;
uint32_t counter = 0;
bool armed = true;               // becomes false after one event until the scale is empty again

// Mass = (Raw Reading - Zero Offset) / Calibration Factor
float readGrams() { return (scale.read_average(3) - zeroOffset) / calibFactor; }

void show(const String& a, const String& b = "") {
  lcd.clear(); lcd.setCursor(0, 0); lcd.print(a); lcd.setCursor(0, 1); lcd.print(b);
}

// Stable-reading check: only accept a weight once it has settled.
bool waitStable(int& grams) {
  float buf[STABLE_SAMPLES]; int n = 0; unsigned long t0 = millis();
  while (millis() - t0 < SETTLE_TIMEOUT) {
    buf[n % STABLE_SAMPLES] = readGrams(); n++;
    if (n >= STABLE_SAMPLES) {
      float lo = buf[0], hi = buf[0], total = 0;
      for (int i = 0; i < STABLE_SAMPLES; i++) { lo = min(lo, buf[i]); hi = max(hi, buf[i]); total += buf[i]; }
      if (hi - lo <= STABLE_BAND) { grams = (int)(total / STABLE_SAMPLES); return true; }
    }
    delay(200);
  }
  return false;
}

// Plug your ESP32-S3-CAM model in here (for example an Edge Impulse classifier).
// Until then it honestly reports "unknown" so nothing is presented as a fact.
String classifyFood(float& conf) { conf = 0.0; return "Unknown / Mixed"; }

String askReason() {
  show("Why left over?", "1 Much 2 Taste 3 Full");
  unsigned long t0 = millis();
  while (millis() - t0 < 6000) {
    for (int i = 0; i < 3; i++) if (digitalRead(BTN[i]) == LOW) return REASONS[i];
    delay(20);
  }
  return "";
}

// The same uid is sent on every retry, so the server keeps only one record per disposal.
bool postEvent(int grams, const String& cat, float conf, const String& reason, const String& uid) {
  String body = "{\"grams\":" + String(grams) + ",\"category\":\"" + cat + "\",\"confidence\":" + String(conf, 2) +
                ",\"uid\":\"" + uid + "\",\"reason\":\"" + reason + "\",\"student\":\"" + studentId + "\"}";
  for (int tries = 0; tries < 3; tries++) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(String(SERVER) + "/api/waste");
      http.addHeader("Content-Type", "application/json");
      http.addHeader("X-Device-Key", DEVICE_KEY);
      http.addHeader("X-Device-Id", DEVICE_ID);
      int code = http.POST(body);
      http.end();
      if (code == 200) return true;
    }
    delay(1000);
  }
  return false;
}

void processWaste() {   // optional module, only runs if ENABLE_PROCESSING is true
  digitalWrite(MOTOR_IN1, HIGH); digitalWrite(MOTOR_IN2, LOW); digitalWrite(MOTOR_EN, HIGH);
  flap.write(90); delay(2000); flap.write(0);
  delay(8000);
  digitalWrite(MOTOR_EN, LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(IR_PIN, INPUT);
  for (int i = 0; i < 3; i++) pinMode(BTN[i], INPUT_PULLUP);
  pinMode(MOTOR_EN, OUTPUT); pinMode(MOTOR_IN1, OUTPUT); pinMode(MOTOR_IN2, OUTPUT);
  lcd.init(); lcd.backlight(); show("FoodNova", "Starting...");
  flap.attach(SERVO_PIN); flap.write(0);
  scale.begin(HX_DT, HX_SCK); delay(500);
  zeroOffset = scale.read_average(20);          // empty platform = zero
  bootId = esp_random();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);
  show("Ready", "Drop your food");
}

void loop() {
  // Duplicate prevention: after one event, wait until the scale is empty and nothing is near the opening.
  if (!armed) {
    if (readGrams() < MIN_GRAMS && digitalRead(IR_PIN) == HIGH) { armed = true; show("Ready", "Drop your food"); }
    delay(200);
    return;
  }
  if (digitalRead(IR_PIN) == LOW) {             // the IR sensor only wakes us up; it does not count as an event
    show("Weighing...");
    int grams = 0;
    if (waitStable(grams) && grams >= MIN_GRAMS) {
      float conf; String cat = classifyFood(conf);
      if (conf < CONF_MIN) cat = "Unknown / Mixed";
      show(String(grams) + " g", cat);
      delay(1000);
      String reason = askReason();
      String uid = String(bootId, HEX) + "-" + String(counter++);
      bool ok = postEvent(grams, cat, conf, reason, uid);
      show(ok ? "Saved!" : "Server error", ok ? "Thank you" : "Not saved");
      if (ENABLE_PROCESSING) processWaste();
    } else {
      show("No valid weight", "Try again");     // unstable or too light: nothing is recorded
    }
    armed = false;
    delay(1500);
  }
  delay(100);
}
