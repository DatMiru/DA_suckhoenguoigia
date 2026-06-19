#ifndef MQTT_MODULE_H
#define MQTT_MODULE_H

#include <Arduino.h>
#include "SharedData.h"

// Broker mặc định (dùng chung với web index.html cũ)
#define MQTT_BROKER  "broker.emqx.io"
#define MQTT_PORT    1883
#define MQTT_TOPIC   "esp32/pulsewave"

// Khởi tạo MQTT client (gọi sau khi WiFi kết nối)
void initMQTT();

// Xử lý kết nối lại + publish dữ liệu (gọi trong Core 0 Task)
void updateMQTT();

#endif
