#ifndef ALGORITHM_MAX30102_H
#define ALGORITHM_MAX30102_H

#include "MAX30105.h"
#include "SharedData.h"
#include <Arduino.h>

// ================= CẤU HÌNH ĐỘ SÁNG LED MAX30102 =================
// ⚠️  Ngưỡng phát hiện ngón tay: irValue >= 50,000 (cố định theo đặc tuyến ADC
// sensor)
//     → LED_AMP_SCAN PHẢI đủ mạnh để phản xạ IR từ tay vượt ngưỡng 50,000
//     → 0x0A (~6.2mA) quá yếu → irValue chỉ ~30,000-45,000 dù có tay → luôn báo
//     NO FINGER → 0x1F (~12.5mA) = mức mặc định setup() → irValue
//     ~80,000-150,000 khi có tay ✅
#define LED_AMP_SCAN                                                           \
  0x1F // ~12.5 mA — chế độ chờ / quét ngón tay (đủ để phát hiện)
// Khi CÓ ngón tay: tăng lên mức cao để tín hiệu AC mạnh, SNR tốt hơn
#define LED_AMP_MEASURE                                                        \
  0x3F // ~50 mA  — chế độ đo chính xác (biên độ AC lớn hơn ~2x)

extern bool isLEDHighBrightness; // true khi đang ở mức MEASURE

void resetMAX30102Measurement();
void processMAX30102Data(MAX30105 &particleSensor);

#endif
