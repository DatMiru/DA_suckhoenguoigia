#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

#include "MAX30105.h"
#include "heartRate.h"
#include <MPU6050.h>

// ================= HEADER CÁC THUẬT TOÁN =================
#include "Algorithm_Battery.h"
#include "Algorithm_MAX30102.h"
#include "Algorithm_MPU6050.h"
#include "MQTT_Module.h"
#include "SharedData.h"
#include "WebDashboard.h"

// ================= WIFI & TELEGRAM =================
#include <UniversalTelegramBot.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

const char *ssid = "DAT_MIRU";
const char *password = "zxcvbnml";
#define BOT_TOKEN "8832059485:AAG9HWg_oSrwP9bZGCC6ZV1fYhB9edUiwcE"
#define CHAT_ID "8239627250"

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

// ================= OLED =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool isDisplayOn = true;

// ================= MPU6050 / MAX30102 =================
MPU6050 mpu;
MAX30105 particleSensor;

// ================= BUZZER / BUTTON =================
#define BUZZER_PIN 23
#define BUTTON_PIN 32
bool lastButtonState = HIGH; // Nút nhả = HIGH (active-low)
unsigned long btnPressTime = 0;
unsigned long btnReleaseTime = 0;
int clickCount = 0;
bool isPressing = false;
bool longPressPrimed = false;
bool awaitingSecondClick =
    false; // true sau khi thả long press, chờ click thứ 2

// ================= BIẾN SHARED — ĐỊNH NGHĨA Ở ĐÂY =================
HRState hrState = HR_IDLE;
unsigned long hrStateStartTime = 0;

bool isManualMode = false;
int baselineHR = 75;
int currentHR = 0;
int beatAvg = 0;
int currentSpO2 = 0;

bool isImmobile = false;
unsigned long immobileStartTime = 0;
bool baselineMeasuredInThisSession = false;
unsigned long baselineMeasureDelay = 5000;

bool impactDetected = false;
bool fallConfirmed = false;
bool telegramSentForThisFall = false;
unsigned long fallConfirmTime = 0;

// ================= PPG BUFFER =================
float ppgBuffer[PPG_BUFFER_SIZE];
volatile int ppgWriteIdx = 0;

// Spinlock bảo vệ ppgBuffer giữa Core 0 (đọc) và Core 1 (ghi)
portMUX_TYPE ppgMux = portMUX_INITIALIZER_UNLOCKED;

// ================= BATTERY =================
float batteryPercent = -1.0f;
float batteryVoltage = 0.0f;

// ================= BIẾN RIÊNG CỦA MAIN =================
unsigned long manualModeStartTime = 0;
unsigned long lastUpdate = 0;
unsigned long showBatteryUntil =
    0; // Thời điểm tắt màn hình pin (0 = không hiển thị)

// ================= TELEGRAM TASK (Core 0) =================
String telegramMsgQueue = "";
bool hasTelegramMsg = false;
TaskHandle_t TelegramTask;

void telegramTaskCode(void *pvParameters) {
  for (;;) {
    // ── WiFi Reconnect (kiểm tra mỗi 30 giây, non-blocking) ──────────
    static unsigned long lastWiFiCheck = 0;
    if (millis() - lastWiFiCheck > 30000) {
      lastWiFiCheck = millis();
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Mất kết nối! Đang thử kết nối lại...");
        WiFi.reconnect();
        // Chờ tối đa 5 giây để kết nối lại
        unsigned long t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 5000) {
          vTaskDelay(500 / portTICK_PERIOD_MS);
        }
        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("[WiFi] Kết nối lại THÀNH CÔNG! IP: " +
                         WiFi.localIP().toString());
        } else {
          Serial.println("[WiFi] Kết nối lại THẤT BẠI. Sẽ thử lại sau 30s.");
        }
      }
    }

    // Xử lý Web Dashboard
    handleWebDashboard();

    // Gửi Telegram nếu có tin nhắn chờ
    if (hasTelegramMsg) {
      String msg = telegramMsgQueue;
      hasTelegramMsg = false;
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[TELEGRAM-TASK] Đang gửi...");
        if (bot.sendMessage(CHAT_ID, msg, ""))
          Serial.println("[TELEGRAM-TASK] => Gửi THÀNH CÔNG!");
        else
          Serial.println("[TELEGRAM-TASK] => LỖI gửi Telegram.");
      } else {
        Serial.println("[TELEGRAM-TASK] => Không có WiFi.");
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); // 10ms → Core 0 xử lý ~100Hz web
  }
}

void sendTelegramAlert(String msg) {
  telegramMsgQueue = msg;
  hasTelegramMsg = true;
  Serial.println("[TELEGRAM] Đẩy tin nhắn vào hàng đợi.");
}

// =========================================================
// HÀM VẼ SÓNG PPG TRÊN OLED (24px cuối màn hình)
// =========================================================
void drawPPGWaveform() {
  const int Y_TOP = 12;    // Pixel bắt đầu vùng sóng (để lại 1 dòng text trên cùng)
  const int Y_BOTTOM = 63; // Pixel cuối màn hình

  if (lastIRValue < 50000) {
    // Không có ngón tay: Vẽ đường thẳng ngang
    display.drawFastHLine(0, (Y_TOP + Y_BOTTOM) / 2, 128, WHITE);
    display.drawFastHLine(0, Y_TOP - 1, 128, WHITE);
    return;
  }

  int prev_x = 0;
  int prev_y = -1;

  // Lấy giá trị min/max từ AGC để vẽ
  float dispMax = oled_max;
  float dispMin = oled_min;
  if (dispMax - dispMin < 10.0f) { // Chống chia cho 0 hoặc biên độ quá hẹp
    dispMax = dispMin + 10.0f;
  }

  int localIdx;
  portENTER_CRITICAL(&ppgMux);
  localIdx = ppgWriteIdx;
  portEXIT_CRITICAL(&ppgMux);

  for (int x = 0; x < 128; x++) {
    // Buffer 128 điểm, mỗi pixel = 1 mẫu (1.28s lịch sử)
    // x=127 = mẫu mới nhất (localIdx - 1)
    int buffer_idx = (localIdx - 128 + x + PPG_BUFFER_SIZE) % PPG_BUFFER_SIZE;

    float val = ppgBuffer[buffer_idx];

    // Ràng buộc giới hạn (Clamp) chống tràn
    if (val > dispMax)
      val = dispMax;
    if (val < dispMin)
      val = dispMin;

    // Ánh xạ tọa độ Y (tọa độ màn hình: 0 ở trên, 63 ở dưới)
    long y = map((long)val, (long)dispMin, (long)dispMax, Y_BOTTOM, Y_TOP);
    y = constrain(y, Y_TOP, Y_BOTTOM);

    // Vẽ nối điểm
    if (prev_y != -1) {
      display.drawLine(prev_x, prev_y, x, y, WHITE);
    }

    prev_x = x;
    prev_y = y;
  }

  // Vẽ đường kẻ ngang ngăn cách vùng text và sóng
  display.drawFastHLine(0, Y_TOP - 1, 128, WHITE);
}

// =========================================================
// SETUP
// =========================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(400000);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP); // Nút nhả=HIGH, nhấn=LOW (active-low)

  // Khởi OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found");
    while (1)
      ;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("HEALTH MONITOR");
  display.println("Connecting WiFi...");
  display.display();

  // Khởi WiFi
  WiFi.begin(ssid, password);
  secured_client.setInsecure();

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi Connected: " + WiFi.localIP().toString());
    display.println("WiFi: OK");
    display.println("IP: " + WiFi.localIP().toString());
    display.display();
    initWebDashboard(); // ← Web Server nội bộ
    initMQTT();         // ← MQTT publish lên broker công cộng
    sendTelegramAlert("🟢 Hệ thống khởi động!\nIP Local: http://" +
                      WiFi.localIP().toString() +
                      "\nMQTT Topic: " + String(MQTT_TOPIC));
  } else {
    Serial.println("WiFi Failed. Offline mode.");
    display.println("WiFi: FAILED (Offline)");
    display.display();
  }
  delay(2000);

  // Tắt màn hình để tiết kiệm pin
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  isDisplayOn = false;

  // Khởi MPU6050
  Serial.println("Initializing MPU6050...");
  mpu.initialize();
  mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_4);
  mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_500);

  // Khởi MAX30102
  Serial.println("Initializing MAX30102...");
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 not found");
    while (1)
      ;
  }
  particleSensor.setup();
  // Mức khởi động: SCAN (~12.5mA) — đủ để irValue > 50,000 khi có tay, tiết
  // kiệm hơn mức đo Khi phát hiện ngón tay, Algorithm_MAX30102 sẽ tự động tăng
  // lên MEASURE (~25mA)
  particleSensor.setPulseAmplitudeRed(LED_AMP_SCAN);
  particleSensor.setPulseAmplitudeIR(LED_AMP_SCAN);
  particleSensor.setPulseAmplitudeGreen(0);
  particleSensor.shutDown();

  // Khởi MAX17043
  initBattery();

  // Khởi PPG buffer
  memset(ppgBuffer, 0, sizeof(ppgBuffer));
  ppgWriteIdx = 0;

  // FreeRTOS Task: Telegram + WebServer → Core 0
  xTaskCreatePinnedToCore(telegramTaskCode, "Core0Task", 10240, NULL, 1,
                          &TelegramTask, 0);

  Serial.println("System Ready.");
}

// =========================================================
// LOOP (Core 1 — 100Hz)
// =========================================================
void loop() {
  if (millis() - lastUpdate >= 10) {
    lastUpdate = millis();

    // ==================================================
    // 1. BUTTON STATE MACHINE (Fixed v2)
    // ==================================================
    bool currentBtn = digitalRead(BUTTON_PIN);

    // === PHÁT HIỆN NHẤN XUỐNG (LOW = đang nhấn) ===
    if (currentBtn == LOW && lastButtonState == HIGH) {
      btnPressTime = millis();
      isPressing = true;
      // Nếu đang chờ click thứ 2 (awaitingSecondClick), lần nhấn này chính là
      // click đó.
      Serial.println("[DEBUG BUTTON] Nút được nhấn xuống!");
    }

    // === PHÁT HIỆN LONG PRESS (chỉ khi đang giữ và CHƯA primed từ trước) ===
    if (isPressing && !longPressPrimed && (millis() - btnPressTime >= 1500)) {
      longPressPrimed = true;
      awaitingSecondClick = false; // Reset, vừa bắt đầu long press mới
      digitalWrite(BUZZER_PIN, HIGH);
      delay(50);
      digitalWrite(BUZZER_PIN, LOW);
      Serial.println("[DEBUG BUTTON] Đã nhấn giữ >1.5s (Primed)");
    }

    // === PHÁT HIỆN THẢ NÚT (HIGH = đã nhả) ===
    if (currentBtn == HIGH && lastButtonState == LOW) {
      unsigned long dur = millis() - btnPressTime;
      isPressing = false;
      btnReleaseTime = millis();
      Serial.print("[DEBUG BUTTON] Nút nhả ra. Thời gian giữ: ");
      Serial.print(dur);
      Serial.println(" ms");

      if (longPressPrimed && !awaitingSecondClick) {
        // Trường hợp 1: Vừa thả sau long press → bắt đầu chờ click thứ 2
        awaitingSecondClick = true;
      } else if (longPressPrimed && awaitingSecondClick) {
        // Trường hợp 2: Đây là click thứ 2 sau long press
        if (dur < 1000)
          clickCount++; // Nhấn ngắn mới tính là click hợp lệ
        awaitingSecondClick = false;
      } else {
        // Trường hợp 3: Nhấn ngắn thông thường (không có long press)
        clickCount++;
      }
    }

    // === XỬ LÝ HÀNH ĐỘNG (sau khi thả nút và hết cửa sổ chờ 400ms) ===
    if (!isPressing && !awaitingSecondClick &&
        millis() - btnReleaseTime > 400) {

      if (longPressPrimed && clickCount >= 1) {
        // Long Press + Click → Bật/Tắt Manual Mode
        Serial.println(
            "[DEBUG BUTTON] LONG PRESS + CLICK -> Bật/Tắt Manual Mode");
        if (!fallConfirmed) {
          if (!isManualMode) {
            Serial.println("[DEBUG HR] Vào Manual Mode (WakeUp MAX30102)");
            isManualMode = true;
            manualModeStartTime = millis();
            hrState = HR_MANUAL_MEASURE;
            hrStateStartTime = millis();
            resetMAX30102Measurement();
            particleSensor.wakeUp();
          } else {
            Serial.println("[DEBUG HR] Thoát Manual Mode (ShutDown MAX30102)");
            isManualMode = false;
            hrState = HR_IDLE;
            particleSensor.shutDown();
          }
        }
        clickCount = 0;
        longPressPrimed = false;
      } else if (longPressPrimed && clickCount == 0) {
        // Long Press đơn (không có click thứ 2 sau cửa sổ chờ) → hết hạn
        Serial.println(
            "[DEBUG BUTTON] Long Press hết hạn (không có click thứ 2)");
        longPressPrimed = false;
      } else if (!longPressPrimed && clickCount == 2) {
        // Double Click
        if (fallConfirmed ||
            (impactDetected && hrState == HR_EMERGENCY_MEASURE)) {
          // Đang trong trạng thái va chạm/ngã → Hủy báo động
          Serial.println("[DEBUG BUTTON] DOUBLE CLICK -> Hủy báo động!");
          if (fallConfirmed && telegramSentForThisFall)
            sendTelegramAlert("🟢 Báo động TẮT THỦ CÔNG. Mọi thứ an toàn!");
          fallConfirmed = impactDetected = false;
          digitalWrite(BUZZER_PIN, LOW);
          if (isManualMode) {
            hrState = HR_MANUAL_MEASURE;
            Serial.println(
                "[DEBUG BUTTON] Quay về Manual Mode sau khi hủy báo động.");
          } else {
            hrState = HR_IDLE;
            particleSensor.shutDown();
          }
        } else {
          // Không trong trạng thái nguy hiểm → Hiển thị % pin 8 giây
          Serial.println("[DEBUG BUTTON] DOUBLE CLICK -> Hiển thị pin 8s");
          showBatteryUntil = millis() + 8000;
          // Bật màn hình nếu đang tắt
          if (!isDisplayOn) {
            display.ssd1306_command(SSD1306_DISPLAYON);
            isDisplayOn = true;
          }
        }
        clickCount = 0;
      } else if (!longPressPrimed && clickCount == 1) {
        Serial.println("[DEBUG BUTTON] SINGLE CLICK (không có hành động)");
        clickCount = 0;
      } else if (!longPressPrimed && clickCount > 2) {
        clickCount = 0;
      }
    }

    // === TIMEOUT chờ click thứ 2 (nếu không nhấn trong 1.5s sau Long Press)
    if (awaitingSecondClick && !isPressing &&
        millis() - btnReleaseTime > 1500) {
      Serial.println(
          "[DEBUG BUTTON] Hết thời gian chờ click thứ 2 -> Hủy Long Press");
      awaitingSecondClick = false;
      longPressPrimed = false;
      clickCount = 0;
    }

    lastButtonState = currentBtn;

    // Manual mode timeout 75s
    if (isManualMode && millis() - manualModeStartTime > 75000) {
      isManualMode = false;
      hrState = HR_IDLE;
      particleSensor.shutDown();
      Serial.println("[HR] Manual Mode timeout 75s.");
    }

    // ==================================================
    // 2+3+5. XỬ LÝ MPU6050 & PHÁT HIỆN NGÃ
    // ==================================================
    processMPU6050Data(mpu, particleSensor);

    // ==================================================
    // 4. XỬ LÝ MAX30102 (HR + SpO2)
    // ==================================================
    processMAX30102Data(particleSensor);

    // ==================================================
    // BATTERY — đọc mỗi 5s (non-blocking bên trong)
    // ==================================================
    updateBattery();

    // ==================================================
    // 5. MQTT PUBLISH (25Hz — Core 1, timing ổn định)
    // ==================================================
    updateMQTT();

    // Đếm tần số loop() — debug
    static unsigned long lastLoopDbg = 0;
    static int loopCount = 0;
    loopCount++;
    if (millis() - lastLoopDbg >= 1000) {
      Serial.print("[LOOP] ~");
      Serial.print(loopCount);
      Serial.println(" Hz (Core1)");
      loopCount = 0;
      lastLoopDbg = millis();
    }

    // ==================================================
    // 6. BUZZER + TELEGRAM + AUTO RESET
    // ==================================================
    if (fallConfirmed) {
      digitalWrite(BUZZER_PIN, (millis() / 250) % 2 == 0 ? HIGH : LOW);

      if (millis() - fallConfirmTime > 15000 && !telegramSentForThisFall) {
        String msg = "🚨 PHÁT HIỆN TÉ NGÃ KHẨN CẤP! 🚨\n";
        msg += "- Nạn nhân bất động không phản hồi.\n";
        msg += "- Nhịp tim cơ sở: " + String(baselineHR) + " BPM\n";
        msg += "- Pin thiết bị: " +
               (batteryPercent >= 0 ? String(batteryPercent, 1) + "%" : "N/A");
        msg += "\n⚡ Yêu cầu kiểm tra ngay!";
        sendTelegramAlert(msg);
        telegramSentForThisFall = true;
      }

      if (millis() - fallConfirmTime > 60000) {
        fallConfirmed = impactDetected = false;
        if (!isManualMode) {
          hrState = HR_IDLE;
          particleSensor.shutDown();
        }
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("[FALL] Auto-reset sau 60s.");
      }
    } else {
      digitalWrite(BUZZER_PIN, LOW);
    }

    // ==================================================
    // 7. QUẢN LÝ MÀN HÌNH OLED
    // ==================================================
    bool showingBattery = (showBatteryUntil > 0 && millis() < showBatteryUntil);
    bool showDisplay =
        (isManualMode || fallConfirmed ||
         (impactDetected && hrState == HR_EMERGENCY_MEASURE) || showingBattery);
    if (showDisplay && !isDisplayOn) {
      display.ssd1306_command(SSD1306_DISPLAYON);
      isDisplayOn = true;
    }
    if (!showDisplay && isDisplayOn) {
      showBatteryUntil = 0; // Reset khi tắt màn hình
      display.clearDisplay();
      display.display();
      display.ssd1306_command(SSD1306_DISPLAYOFF);
      isDisplayOn = false;
    }

    if (isDisplayOn) {
      display.clearDisplay();
      display.setCursor(0, 0);

      // ---- Màn hình HIỂN THỊ PIN (ưu tiên cao nếu đang xem pin) ----
      if (showingBattery && !fallConfirmed &&
          !(impactDetected && hrState == HR_EMERGENCY_MEASURE)) {
        display.setTextSize(1);
        display.setCursor(20, 4);
        display.println("-- BATTERY --");
        if (batteryPercent >= 0) {
          display.setTextSize(2);
          display.setCursor(24, 22);
          display.print(batteryPercent, 0);
          display.println(" %");
          display.setTextSize(1);
          display.setCursor(20, 48);
          display.print(batteryVoltage, 2);
          display.print(" V");
        } else {
          display.setTextSize(1);
          display.setCursor(28, 28);
          display.println("N/A");
        }
        // Thanh tiến trình
        int barW =
            (batteryPercent >= 0) ? (int)(batteryPercent / 100.0f * 108) : 0;
        barW = constrain(barW, 0, 108);
        display.drawRect(10, 38, 108, 8, WHITE);
        display.fillRect(10, 38, barW, 8, WHITE);
      }
      // ---- Màn hình TÉ NGÃ ----
      else if (fallConfirmed) {
        display.setTextSize(2);
        display.setCursor(10, 4);
        display.println("FALL!");
        display.setCursor(0, 24);
        display.println("DETECTED");
        display.setTextSize(1);
        display.setCursor(0, 48);
        if (!telegramSentForThisFall) {
          int tl = 15 - (int)((millis() - fallConfirmTime) / 1000);
          display.print("SOS in ");
          display.print(tl);
          display.print("s  2x=CANCEL");
        } else {
          display.print("SOS SENT! 2x=CANCEL");
        }
      }
      // ---- Màn hình PHÂN TÍCH VA CHẠM ----
      else if (impactDetected && hrState == HR_EMERGENCY_MEASURE) {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print("IMPACT! HR: ");
        if (beatAvg > 0) display.print(beatAvg);
        else display.print("--");
        display.print(" BPM");
        
        // Vẽ sóng PPG ở phần không gian còn lại
        drawPPGWaveform();
      }
      // ---- Màn hình MANUAL MODE ----
      else if (isManualMode) {
        display.setTextSize(1);
        display.setCursor(0, 0);
        
        if (beatAvg == 0) {
          display.print("HR: --     SpO2: --%");
        } else {
          display.print("HR: ");
          display.print(beatAvg);
          display.print("    SpO2: ");
          display.print(currentSpO2);
          display.print("%");
        }
        
        // Vẽ sóng PPG chiếm toàn bộ phần màn hình dưới
        drawPPGWaveform();
      }

      display.display();
    }

    // ==================================================
    // 8. SERIAL STATUS (mỗi 500ms)
    // ==================================================
    static unsigned long lastSerial = 0;
    if (millis() - lastSerial > 500) {
      lastSerial = millis();
      Serial.print("[STATE] ");
      if (isManualMode)
        Serial.print("MANUAL ");
      else if (fallConfirmed)
        Serial.print("ALARM! ");
      else if (impactDetected)
        Serial.print("IMPACT ");
      else if (isImmobile)
        Serial.print("STILL  ");
      else
        Serial.print("NORMAL ");

      Serial.print("| HR_Mode:");
      switch (hrState) {
      case HR_IDLE:
        Serial.print("IDLE  ");
        break;
      case HR_MEASURING_BASELINE:
        Serial.print("BASE  ");
        break;
      case HR_EMERGENCY_MEASURE:
        Serial.print("EMRG  ");
        break;
      case HR_MANUAL_MEASURE:
        Serial.print("MANL  ");
        break;
      }
      Serial.print("| Accel:");
      Serial.print(filteredAccel, 2);
      Serial.print("g | Angle:");
      Serial.print(angle, 1);
      Serial.print("* | HR:");
      Serial.print(beatAvg);
      Serial.print(" | SpO2:");
      Serial.print(currentSpO2);
      Serial.print("% | Bat:");
      if (batteryPercent >= 0) {
        Serial.print(batteryPercent, 1);
        Serial.println("%");
      } else
        Serial.println("N/A");
    }
  }
}
