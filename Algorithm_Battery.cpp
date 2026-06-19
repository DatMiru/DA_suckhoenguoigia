#include "Algorithm_Battery.h"
#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>

SFE_MAX1704X lipo(MAX1704X_MAX17043);
static bool batteryFound = false;

void initBattery() {
  // Wire đã được begin() ở setup() chính, chỉ cần gọi lipo.begin()
  if (!lipo.begin()) {
    Serial.println(
        "[BATTERY] ⚠️  MAX17043 KHÔNG TÌM THẤY! Kiểm tra kết nối I2C.");
    batteryFound = false;
    batteryPercent = -1;
    batteryVoltage = 0;
    return;
  }
  lipo.quickStart();     // Hiệu chỉnh nhanh SOC
  lipo.setThreshold(15); // Cảnh báo ở 15%
  batteryFound = true;
  Serial.println("[BATTERY] ✅ MAX17043 sẵn sàng.");
}

void updateBattery() {
  if (!batteryFound)
    return;

  static unsigned long lastRead = 0;
  if (millis() - lastRead < 5000)
    return; // Chỉ đọc mỗi 5 giây
  lastRead = millis();

  float soc = lipo.getSOC();
  float volt = lipo.getVoltage();

  // Clamp hợp lệ
  if (soc > 100.0f)
    soc = 100.0f;
  if (soc < 0.0f)
    soc = 0.0f;

  batteryPercent = soc;
  batteryVoltage = volt;

  Serial.print("[BATTERY] SOC = ");
  Serial.print(soc, 1);
  Serial.print("% | Voltage = ");
  Serial.print(volt, 3);
  Serial.println("V");
}
