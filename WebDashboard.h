#ifndef WEBDASHBOARD_H
#define WEBDASHBOARD_H

#include <Arduino.h>
#include "SharedData.h"

// Khởi tạo web server (gọi trong setup sau khi WiFi đã kết nối)
void initWebDashboard();

// Xử lý request đến từ client (gọi trong FreeRTOS task hoặc loop)
void handleWebDashboard();

#endif
