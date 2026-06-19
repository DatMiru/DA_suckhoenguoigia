#ifndef ALGORITHM_BATTERY_H
#define ALGORITHM_BATTERY_H

#include <Arduino.h>
#include "SharedData.h"

// Khởi tạo module MAX17043 trên bus I2C đã có sẵn
void initBattery();

// Gọi trong loop() mỗi 5 giây để cập nhật batteryPercent và batteryVoltage
void updateBattery();

#endif
