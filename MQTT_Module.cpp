#include "MQTT_Module.h"
#include <PubSubClient.h>
#include <WiFi.h>

// ================= MQTT Client =================
static WiFiClient mqttWifiClient;
static PubSubClient mqttClient(mqttWifiClient);

static unsigned long lastPublish = 0;
static unsigned long lastReconnect = 0;
static bool mqttEnabled = false;

// ================= Reconnect (non-blocking) =================
static void tryReconnect() {
  if (millis() - lastReconnect < 5000)
    return; // Thử lại mỗi 5 giây
  lastReconnect = millis();

  // Client ID ngẫu nhiên để tránh xung đột trên broker công cộng
  String clientId = "ESP32Health_";
  clientId += String((uint32_t)(ESP.getEfuseMac() >> 16), HEX);

  Serial.print("[MQTT] Đang kết nối broker... ");
  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("OK ✅");
    Serial.print("[MQTT] Topic: ");
    Serial.println(MQTT_TOPIC);
  } else {
    Serial.print("Thất bại, rc=");
    Serial.println(mqttClient.state());
  }
}

// ================= Public API =================
void initMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[MQTT] Bỏ qua: WiFi chưa kết nối.");
    return;
  }
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(3);
  mqttEnabled = true;
  Serial.println("[MQTT] Module khởi tạo. Broker: " + String(MQTT_BROKER) +
                 ":" + String(MQTT_PORT));
  tryReconnect(); // Kết nối ngay lần đầu
}

void updateMQTT() {
  if (!mqttEnabled || WiFi.status() != WL_CONNECTED)
    return;

  // Reconnect nếu mất kết nối
  if (!mqttClient.connected()) {
    tryReconnect();
    return;
  }

  // MQTT keepalive loop (quan trọng — phải gọi liên tục)
  mqttClient.loop();

  // Đóng gói JSON
  // "active": 1 = MAX30102 đang đo, 0 = cảm biến tắt
  int isActive = (hrState != HR_IDLE) ? 1 : 0;
  int hasFinger = (isActive && lastIRValue >= 50000) ? 1 : 0;

  static int lastSentIdx = -1;
  if (lastSentIdx == -1) {
    lastSentIdx = ppgWriteIdx;
  }

  // Tính số lượng mẫu mới được ghi kể từ lần gửi trước
  int newSamplesCount = 0;
  int currentWriteIdx = ppgWriteIdx;
  if (currentWriteIdx >= lastSentIdx) {
    newSamplesCount = currentWriteIdx - lastSentIdx;
  } else {
    newSamplesCount = currentWriteIdx + PPG_BUFFER_SIZE - lastSentIdx;
  }

  // Xác định chu kỳ / điều kiện gửi
  bool shouldPublish = false;
  int sendSamples = 0;

  if (isActive) {
    // Khi đang đo: Gửi khi gom đủ tối thiểu 10 mẫu mới (~100ms)
    if (newSamplesCount >= 10) {
      shouldPublish = true;
      sendSamples = newSamplesCount; // Gửi toàn bộ các mẫu mới (lossless)
    }
  } else {
    // Khi IDLE: Chỉ gửi 1 msg/s (mỗi 1000ms)
    if (millis() - lastPublish >= 1000) {
      shouldPublish = true;
      sendSamples = (newSamplesCount > 10) ? 10 : newSamplesCount;
      if (sendSamples == 0) sendSamples = 2; // Dự phòng
    }
  }

  if (!shouldPublish)
    return;

  lastPublish = millis();

  // Lấy các mẫu cần gửi từ buffer ra mảng cục bộ
  float localBuf[PPG_BUFFER_SIZE];
  portENTER_CRITICAL(&ppgMux);
  int startIdx = (isActive) ? lastSentIdx : (currentWriteIdx - sendSamples + PPG_BUFFER_SIZE) % PPG_BUFFER_SIZE;
  for (int i = 0; i < sendSamples; i++) {
    localBuf[i] = ppgBuffer[(startIdx + i) % PPG_BUFFER_SIZE];
  }
  portEXIT_CRITICAL(&ppgMux);

  // Cập nhật chỉ số đã gửi
  if (isActive) {
    lastSentIdx = (lastSentIdx + sendSamples) % PPG_BUFFER_SIZE;
  } else {
    lastSentIdx = currentWriteIdx; // Reset khi không hoạt động để tránh dồn ứ lúc bật lại
  }

  String waveArr = "[";
  for (int i = 0; i < sendSamples; i++) {
    waveArr += String(localBuf[i], 1);
    if (i < sendSamples - 1) waveArr += ",";
  }
  waveArr += "]";

  String payload = "{";
  payload += "\"wave\":" + waveArr + ",";
  payload += "\"bpm\":" + String(beatAvg) + ",";
  payload += "\"spo2\":" + String(currentSpO2) + ",";
  payload += "\"bat\":" + String(batteryPercent, 1) + ",";
  payload += "\"fall\":" + String(fallConfirmed ? 1 : 0) + ",";
  payload += "\"impact\":" + String(impactDetected ? 1 : 0) + ",";
  payload += "\"active\":" + String(isActive) + ",";
  payload += "\"finger\":" + String(hasFinger);
  payload += "}";

  bool ok = mqttClient.publish(MQTT_TOPIC, payload.c_str());

  // Debug mỗi giây — hiển thị số message thực tế
  static unsigned long lastDbg = 0;
  static int msgCount = 0;
  if (ok) msgCount++;
  if (millis() - lastDbg >= 1000) {
    lastDbg = millis();
    Serial.print("[MQTT] ~"); Serial.print(msgCount);
    Serial.print(" msg/s | wave[2] bpm=");
    Serial.print(beatAvg);
    Serial.print(" spo2="); Serial.print(currentSpO2);
    Serial.print(" bat="); Serial.print(batteryPercent, 1);
    Serial.println("%");
    msgCount = 0;
  }
}
