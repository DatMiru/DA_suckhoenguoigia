#include "Algorithm_MAX30102.h"
#include <math.h>

// ================= HỆ SỐ HIỆU CHUẨN THỰC NGHIỆM =================
// Phương trình hiệu chuẩn nhịp tim: HR_calib = HR_CALIB_A * beatAvg + HR_CALIB_B
#define HR_CALIB_A 0.97f
#define HR_CALIB_B 0.65f

// Phương trình hiệu chuẩn SpO2: SpO2_calib = SPO2_CALIB_A * spo2 + SPO2_CALIB_B
#define SPO2_CALIB_A 0.27f
#define SPO2_CALIB_B 70.9f

// --- CẤu hình chung cho Nhịp tim (BPM) và SpO2 ---
const byte RATE_SIZE = 10;
float rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute = 0;

const byte SPO2_RATE_SIZE = 10;
float spo2Rates[SPO2_RATE_SIZE];
byte spo2RateSpot = 0;

// Các biến phục vụ Valley Detection
float beatDC = 0;
float irAC_mean = 0;
float smooth_irAC = 0;
float last_smooth_irAC = 0;
bool is_falling = false;
float peak_threshold = -20.0f;
long current_refractory = 400;
// === BANDPASS FILTER CHO HIỂN THỊ PPG (thay thế display_dc + iir_filtered) ===
// Highpass bậc 2 Butterworth fc=0.5Hz @ 100Hz — loại baseline drift chậm
float hp_x1 = 0, hp_x2 = 0; // input history
float hp_y1 = 0, hp_y2 = 0; // output history

// Lowpass bậc 2 Butterworth fc=8Hz @ 100Hz — giữ harmonic đến H7
float lp_x1 = 0, lp_x2 = 0;
float lp_y1 = 0, lp_y2 = 0;

float ppg_warmup_gain = 0.0f; // Ramp-up 0→1 trong 200 mẫu đầu
int ppg_warmup_cnt = 0;

// Hệ số Highpass Butterworth bậc 2, fc=0.5Hz, fs=100Hz (bilinear transform)
const float HP_B0 = 0.96908869f;
const float HP_B1 = -1.93817737f;
const float HP_B2 = 0.96908869f;
const float HP_A1 = -1.93609074f;
const float HP_A2 = 0.94026481f;

// Hệ số Lowpass Butterworth bậc 2, fc=8Hz, fs=100Hz (bilinear transform)
const float LP_B0 = 0.17218841f;
const float LP_B1 = 0.34437682f;
const float LP_B2 = 0.17218841f;
const float LP_A1 = -0.52827176f;
const float LP_A2 = 0.21702540f;

float iir_filtered = 0;
float oled_max = 100.0f;
float oled_min = -100.0f;
float block_max = -99999.0f;
float block_min = 99999.0f;
int agc_sample_counter = 0;

// Các biến phục vụ SpO2 (Hybrid Sliding Window)
const int SPO2_SUB_WINDOW = 100; // Khối 1 giây (ở 100Hz)
int sampleCount = 0;
int spo2BlockCount = 0; // Đếm số chu kỳ 1 giây đã qua

uint64_t sumIR = 0, sumRed = 0;
float sumSqIR = 0, sumSqRed = 0;
float dcFilterIR = 0, dcFilterRed = 0;
float lowpassAC_IR = 0, lowpassAC_Red = 0;
float max_ac_ir = -99999.0f, min_ac_ir = 99999.0f;

// Bộ đệm Sliding Window (4 khối 1 giây = 4 giây RMS)
uint64_t sumIR_hist[4] = {0};
uint64_t sumRed_hist[4] = {0};
float sumSqIR_hist[4] = {0};
float sumSqRed_hist[4] = {0};
byte hist_idx = 0;

// Lịch sử biên độ AC mỗi block — IR và Red (cho phép tính R theo peak-to-peak)
float max_ac_ir_hist[4] = {-99999.0f, -99999.0f, -99999.0f, -99999.0f};
float min_ac_ir_hist[4] = {99999.0f, 99999.0f, 99999.0f, 99999.0f};
float max_ac_red_hist[4] = {-99999.0f, -99999.0f, -99999.0f, -99999.0f};
float min_ac_red_hist[4] = {99999.0f, 99999.0f, 99999.0f, 99999.0f};

// Peak-to-peak tracker từng block (cả IR và Red)
float max_ac_red = -99999.0f, min_ac_red = 99999.0f;

long lastIRValue =
    0; // export ra SharedData.h → MQTT_Module dùng cho field "finger"
unsigned long fingerOnStartTime = 0;

// ================= ADAPTIVE LED BRIGHTNESS =================
// Trạng thái độ sáng hiện tại của LED (false = SCAN thấp, true = MEASURE cao)
bool isLEDHighBrightness = false;

void resetMAX30102Measurement() {
  lastBeat = 0;
  beatAvg = 0;
  currentSpO2 = 0;
  beatsPerMinute = 0;

  beatDC = 0;
  irAC_mean = 0;
  smooth_irAC = 0;
  last_smooth_irAC = 0;
  is_falling = false;
  peak_threshold = -20.0f;
  current_refractory = 400;
  iir_filtered = 0;
  oled_max = 100.0f;
  oled_min = -100.0f;
  block_max = -99999.0f;
  block_min = 99999.0f;
  agc_sample_counter = 0;
  // Reset Butterworth bandpass filter states
  hp_x1 = hp_x2 = hp_y1 = hp_y2 = 0;
  lp_x1 = lp_x2 = lp_y1 = lp_y2 = 0;
  ppg_warmup_gain = 0.0f;
  ppg_warmup_cnt = 0;
  // Xóa buffer — critical section vì Core 0 có thể đang đọc búcuffer
  portENTER_CRITICAL(&ppgMux);
  memset(ppgBuffer, 0, sizeof(ppgBuffer));
  ppgWriteIdx = 0;
  portEXIT_CRITICAL(&ppgMux);

  sampleCount = 0;
  sumIR = 0;
  sumRed = 0;
  sumSqIR = 0;
  sumSqRed = 0;
  dcFilterIR = 0;
  dcFilterRed = 0;
  lowpassAC_IR = 0;
  lowpassAC_Red = 0;
  max_ac_ir = -99999.0f;
  min_ac_ir = 99999.0f;
  spo2BlockCount = 0;
  for (byte x = 0; x < RATE_SIZE; x++)
    rates[x] = 0.0f;
  for (byte x = 0; x < SPO2_RATE_SIZE; x++)
    spo2Rates[x] = 0.0f;
  rateSpot = 0;
  spo2RateSpot = 0;
  hist_idx = 0;
  for (int i = 0; i < 4; i++) {
    sumIR_hist[i] = 0;
    sumRed_hist[i] = 0;
    sumSqIR_hist[i] = 0;
    sumSqRed_hist[i] = 0;
    max_ac_ir_hist[i] = -99999.0f;
    min_ac_ir_hist[i] = 99999.0f;
    max_ac_red_hist[i] = -99999.0f;
    min_ac_red_hist[i] = 99999.0f;
  }
  max_ac_red = -99999.0f;
  min_ac_red = 99999.0f;
  fingerOnStartTime = 0;
  lastIRValue = 0;
  // Reset cờ độ sáng về mức SCAN (mức thấp — sensor sẽ điều chỉnh lại lần tiếp theo)
  isLEDHighBrightness = false;
}

void processMAX30102Data(MAX30105 &particleSensor) {
  if (hrState != HR_IDLE) {

    particleSensor.check();

    while (particleSensor.available()) {
      long irValue = particleSensor.getFIFOIR();
      long redValue = particleSensor.getFIFORed();
      particleSensor.nextSample();

      lastIRValue = irValue;

      if (irValue < 50000) {
        // Không có ngón tay — giảm độ sáng LED về mức SCAN để tiết kiệm điện
        if (isLEDHighBrightness) {
          particleSensor.setPulseAmplitudeRed(LED_AMP_SCAN);
          particleSensor.setPulseAmplitudeIR(LED_AMP_SCAN);
          isLEDHighBrightness = false;
          Serial.println("[LED] Không có ngón tay → Hạ độ sáng LED xuống mức "
                         "SCAN (~6.2mA)");
        }
        // Xóa dữ liệu đo
        beatAvg = 0;
        currentSpO2 = 0;
        // Xóa buffer trước khi reset (critical section bảo vệ Core 0)
        portENTER_CRITICAL(&ppgMux);
        memset(ppgBuffer, 0, sizeof(ppgBuffer));
        ppgWriteIdx = 0;
        portEXIT_CRITICAL(&ppgMux);
        resetMAX30102Measurement();

        static unsigned long lastNoFingerPrint = 0;
        if (millis() - lastNoFingerPrint > 2000) {
          Serial.println("[DEBUG HR] Cảnh báo: NO FINGER (Không đeo tay)");
          lastNoFingerPrint = millis();
        }
      } else {
        // Đánh dấu thời điểm đặt tay
        if (fingerOnStartTime == 0) {
          fingerOnStartTime = millis();
        }

        // Tăng độ sáng LED lên mức MEASURE khi phát hiện ngón tay
        if (!isLEDHighBrightness) {
          particleSensor.setPulseAmplitudeRed(LED_AMP_MEASURE);
          particleSensor.setPulseAmplitudeIR(LED_AMP_MEASURE);
          isLEDHighBrightness = true;
          Serial.println("[LED] Phát hiện ngón tay → Tăng độ sáng LED lên mức "
                         "MEASURE (~25mA) — Đo chính xác hơn");
        }

        // === 1. Phát hiện nhịp tim (Valley Detection - Medical Grade) ===
        if (beatDC == 0)
          beatDC = (float)irValue;

        beatDC = 0.995f * beatDC + 0.005f * (float)irValue;
        float raw_irAC = (float)irValue - beatDC;

        irAC_mean = 0.995f * irAC_mean + 0.005f * raw_irAC;
        float irAC = raw_irAC - irAC_mean;

        smooth_irAC = 0.9f * smooth_irAC + 0.1f * irAC;

        // === BANDPASS FILTER CHO HIỂN THỊ PPG ===
        // Bước 1: Highpass bậc 2 — loại DC/baseline drift (fc=0.5Hz)
        float x0_hp = (float)irValue;
        float y0_hp = HP_B0 * x0_hp + HP_B1 * hp_x1 + HP_B2 * hp_x2 -
                      HP_A1 * hp_y1 - HP_A2 * hp_y2;
        hp_x2 = hp_x1;
        hp_x1 = x0_hp;
        hp_y2 = hp_y1;
        hp_y1 = y0_hp;

        // Bước 2: Lowpass bậc 2 — cắt nhiễu cao tần, giữ harmonic PPG (fc=8Hz)
        float x0_lp = y0_hp;
        float y0_lp = LP_B0 * x0_lp + LP_B1 * lp_x1 + LP_B2 * lp_x2 -
                      LP_A1 * lp_y1 - LP_A2 * lp_y2;
        lp_x2 = lp_x1;
        lp_x1 = x0_lp;
        lp_y2 = lp_y1;
        lp_y1 = y0_lp;

        // Đảo chiều: đỉnh tâm thu hướng lên (chuẩn y tế)
        float ac_signal = -y0_lp;

        // Ramp-up gain: mute 200 mẫu đầu tiên để tránh transient của filter
        if (ppg_warmup_cnt < 200) {
          ppg_warmup_cnt++;
          ppg_warmup_gain = (float)ppg_warmup_cnt / 200.0f;
        }
        iir_filtered = ac_signal * ppg_warmup_gain;

        // === CẬP NHẬT AGC TRƯỚC khi push (sửa thứ tự — Issue 3) ===
        if (iir_filtered > block_max)
          block_max = iir_filtered;
        if (iir_filtered < block_min)
          block_min = iir_filtered;

        agc_sample_counter++;
        if (agc_sample_counter >= 50) {
          oled_max = oled_max + 0.1f * (block_max - oled_max);
          oled_min = oled_min + 0.1f * (block_min - oled_min);
          if (oled_max - oled_min < 50.0f) {
            oled_max = oled_min + 50.0f;
          }
          block_max = -99999.0f;
          block_min = 99999.0f;
          agc_sample_counter = 0;
        }

        // === PUSH VÀO PPG BUFFER với Critical Section (Issue 4) ===
        portENTER_CRITICAL(&ppgMux);
        ppgBuffer[ppgWriteIdx] = iir_filtered;
        ppgWriteIdx = (ppgWriteIdx + 1) % PPG_BUFFER_SIZE;
        portEXIT_CRITICAL(&ppgMux);

        peak_threshold *= 0.995f;
        if (peak_threshold > -20)
          peak_threshold = -20;

        if (smooth_irAC < last_smooth_irAC) {
          is_falling = true;
        } else if (is_falling && smooth_irAC > last_smooth_irAC) {
          is_falling = false;

          if (last_smooth_irAC < peak_threshold) {
            unsigned long now = millis();

            if (lastBeat > 0) {
              long delta = now - lastBeat;

              if (delta > current_refractory && delta < 2000) {
                beatsPerMinute = 60000.0f / (float)delta;

                if (beatsPerMinute >= 40 && beatsPerMinute <= 160) {
                  if (millis() - fingerOnStartTime >= 500) {
                    rates[rateSpot++] = beatsPerMinute;
                    rateSpot %= RATE_SIZE;

                    int validRates = 0;
                    float tempRates[RATE_SIZE];

                    for (byte x = 0; x < RATE_SIZE; x++) {
                      if (rates[x] > 0.0f) {
                        tempRates[validRates++] = rates[x];
                      }
                    }

                    if (validRates > 0) {
                      for (int i = 0; i < validRates - 1; i++) {
                        for (int j = i + 1; j < validRates; j++) {
                          if (tempRates[i] > tempRates[j]) {
                            float temp = tempRates[i];
                            tempRates[i] = tempRates[j];
                            tempRates[j] = temp;
                          }
                        }
                      }

                      int trimCount = 0;
                      if (validRates >= 8)
                        trimCount = 2;
                      else if (validRates >= 4)
                        trimCount = 1;

                      float sum = 0;
                      int count = 0;
                      for (int i = trimCount; i < validRates - trimCount; i++) {
                        sum += tempRates[i];
                        count++;
                      }

                      if (count > 0) {
                        float rawHR = sum / count;
                        float calibHR = HR_CALIB_A * rawHR + HR_CALIB_B;
                        beatAvg = (int)(calibHR + 0.5f);
                        if (hrState == HR_MANUAL_MEASURE) {
                          baselineHR = beatAvg; // Cập nhật nhịp tim cơ sở từ lần đo thủ công gần nhất
                        }
                      }
                    }
                  }
                }

                peak_threshold = last_smooth_irAC * 0.8f;
                if (peak_threshold < -150)
                  peak_threshold = -150;
                if (peak_threshold > -20)
                  peak_threshold = -20;

                current_refractory = delta * 0.6f;
                if (current_refractory < 400)
                  current_refractory = 400;
                if (current_refractory > 1000)
                  current_refractory = 1000;
              }
            }
            lastBeat = now;
          }
        }
        last_smooth_irAC = smooth_irAC;

        // === 2. Xử lý SpO2 (Hybrid Sliding Window + Peak-to-Peak R) ===
        // Khởi tạo DC filter tức thì khi đặt tay lần đầu
        if (dcFilterIR == 0) {
          dcFilterIR = (float)irValue;
          dcFilterRed = (float)redValue;
          lowpassAC_IR = lowpassAC_Red = 0;
          // Skip 1 block đầu (1 giây) để lowpassAC settle.
          // DC filter đã init tức thì nên không cần skip 4 giây như trước.
          spo2BlockCount = -1;
        }

        dcFilterIR = 0.995f * dcFilterIR + 0.005f * (float)irValue;
        dcFilterRed = 0.995f * dcFilterRed + 0.005f * (float)redValue;

        float rawAC_IR = (float)irValue - dcFilterIR;
        float rawAC_Red = (float)redValue - dcFilterRed;

        lowpassAC_IR = 0.7f * lowpassAC_IR + 0.3f * rawAC_IR;
        lowpassAC_Red = 0.7f * lowpassAC_Red + 0.3f * rawAC_Red;

        // Track peak-to-peak cho cả IR và Red.
        // Bỏ qua 20 mẫu đầu mỗi block (0.2s) để lowpassAC (α=0.7) settle,
        // tránh transient kéo min_ac_ir xuống thấp bất thường → R sai → SpO2 thấp.
        if (sampleCount >= 20) {
          if (lowpassAC_IR > max_ac_ir)
            max_ac_ir = lowpassAC_IR;
          if (lowpassAC_IR < min_ac_ir)
            min_ac_ir = lowpassAC_IR;
          if (lowpassAC_Red > max_ac_red)
            max_ac_red = lowpassAC_Red;
          if (lowpassAC_Red < min_ac_red)
            min_ac_red = lowpassAC_Red;
        }

        sumIR += (uint64_t)irValue;
        sumRed += (uint64_t)redValue;
        sumSqIR += lowpassAC_IR * lowpassAC_IR;
        sumSqRed += lowpassAC_Red * lowpassAC_Red;
        sampleCount++;

        if (sampleCount >= SPO2_SUB_WINDOW) {
          // Lưu block hiện tại vào sliding window
          sumIR_hist[hist_idx] = sumIR;
          sumRed_hist[hist_idx] = sumRed;
          sumSqIR_hist[hist_idx] = sumSqIR;
          sumSqRed_hist[hist_idx] = sumSqRed;
          max_ac_ir_hist[hist_idx] = max_ac_ir;
          min_ac_ir_hist[hist_idx] = min_ac_ir;
          max_ac_red_hist[hist_idx] = max_ac_red;
          min_ac_red_hist[hist_idx] = min_ac_red;
          hist_idx = (hist_idx + 1) % 4;
          spo2BlockCount++;

          // Chờ đủ 2 block hợp lệ (skip 1 block đầu xấu, block 2–3 đã sạch).
          // Sau khi đủ 4 block thì hoạt động y hệt như trước.
          if (spo2BlockCount >= 2) {
            // Số slot thực sự đã được ghi (tối đa 4, tối thiểu 2)
            int validSlots = (spo2BlockCount < 4) ? spo2BlockCount : 4;
            uint64_t total_sumIR = 0, total_sumRed = 0;
            float total_sumSqIR = 0;
            float w_max_ir = -99999.0f, w_min_ir = 99999.0f;
            float w_max_red = -99999.0f, w_min_red = 99999.0f;

            // Lấy validSlots slot gần nhất (tính ngược từ hist_idx hiện tại)
            for (int i = 0; i < validSlots; i++) {
              int slot = (hist_idx - 1 - i + 4) % 4;
              total_sumIR  += sumIR_hist[slot];
              total_sumRed += sumRed_hist[slot];
              total_sumSqIR += sumSqIR_hist[slot];
              if (max_ac_ir_hist[slot]  > w_max_ir)  w_max_ir  = max_ac_ir_hist[slot];
              if (min_ac_ir_hist[slot]  < w_min_ir)  w_min_ir  = min_ac_ir_hist[slot];
              if (max_ac_red_hist[slot] > w_max_red)  w_max_red = max_ac_red_hist[slot];
              if (min_ac_red_hist[slot] < w_min_red)  w_min_red = min_ac_red_hist[slot];
            }

            // Chia đúng cho số mẫu thực có (2×100, 3×100 hoặc 4×100)
            int WINDOW_TOTAL = SPO2_SUB_WINDOW * validSlots;
            float dcIR_mean = (float)total_sumIR / WINDOW_TOTAL;
            float dcRed_mean = (float)total_sumRed / WINDOW_TOTAL;
            float acIR_RMS = sqrtf(total_sumSqIR / WINDOW_TOTAL);

            // Peak-to-peak theo định nghĩa Maxim
            float ppIR = w_max_ir - w_min_ir;
            float ppRed = w_max_red - w_min_red;

            if (dcIR_mean > 0 && dcRed_mean > 0 && ppIR > 0 && ppRed > 0) {
              // Perfusion Index dùng pp/2 (amplitude) thay vì RMS
              float perfusionIndex = (ppIR / 2.0f) / dcIR_mean * 100.0f;

              // SQI: tỉ lệ pp/RMS — sóng PPG thực nằm trong 1.5–5.0
              // Nhiễu ngẫu nhiên: ratio > 5; tín hiệu DC thuần: ratio < 1.5
              float sqi = (acIR_RMS > 0) ? (ppIR / acIR_RMS) : 0;

              if (perfusionIndex >= 0.3f && sqi > 1.5f && sqi < 5.0f) {
                // R theo peak-to-peak — khớp với calibration hệ số Maxim
                float R = (ppRed / dcRed_mean) / (ppIR / dcIR_mean);

                if (R > 0.4f && R < 1.2f) {
                  float rawSpo2 = -45.060f * R * R + 30.354f * R + 94.845f;
                  float calibSpo2 = SPO2_CALIB_A * rawSpo2 + SPO2_CALIB_B;
                  calibSpo2 = constrain(calibSpo2, 80.0f, 100.0f);

                  if (millis() - fingerOnStartTime >= 2000) {
                    spo2Rates[spo2RateSpot++] = calibSpo2;
                    spo2RateSpot %= SPO2_RATE_SIZE;

                    int validSpO2 = 0;
                    float tempSpO2[SPO2_RATE_SIZE];
                    for (byte x = 0; x < SPO2_RATE_SIZE; x++) {
                      if (spo2Rates[x] >= 80.0f)
                        tempSpO2[validSpO2++] = spo2Rates[x];
                    }
                    if (validSpO2 > 0) {
                      // Bubble sort
                      for (int i = 0; i < validSpO2 - 1; i++)
                        for (int j = i + 1; j < validSpO2; j++)
                          if (tempSpO2[i] > tempSpO2[j]) {
                            float t = tempSpO2[i];
                            tempSpO2[i] = tempSpO2[j];
                            tempSpO2[j] = t;
                          }
                      int trim = (validSpO2 >= 8)   ? 2
                                 : (validSpO2 >= 4) ? 1
                                                    : 0;
                      float sum = 0;
                      int cnt = 0;
                      for (int i = trim; i < validSpO2 - trim; i++) {
                        sum += tempSpO2[i];
                        cnt++;
                      }
                      if (cnt > 0)
                        currentSpO2 = (int)(sum / cnt + 0.5f);
                    }
                  }
                }
              }
            }
          }

          // Reset block
          sampleCount = 0;
          sumIR = 0;
          sumRed = 0;
          sumSqIR = 0;
          sumSqRed = 0;
          max_ac_ir = -99999.0f;
          min_ac_ir = 99999.0f;
          max_ac_red = -99999.0f;
          min_ac_red = 99999.0f;
        }
      }
    } // end while (available)

    // ==================================================
    // STATE MACHINE
    // ==================================================
    if (hrState == HR_MEASURING_BASELINE &&
        millis() - hrStateStartTime > 20000) {
      if (beatAvg > 30) {
        baselineHR = beatAvg;
        Serial.print("[DEBUG HR] Đo Baseline THÀNH CÔNG. Baseline mới = ");
        Serial.println(baselineHR);
        Serial.println("[DEBUG HR] Sẽ ngủ 10s rồi thức dậy ĐO LẠI (Cập nhật "
                       "liên tục khi ngủ).");
        baselineMeasuredInThisSession = false;
        baselineMeasureDelay = 10000; // Thay đổi chu kỳ đợi thành 10s
        immobileStartTime = millis();
      } else {
        Serial.println("[DEBUG HR] Đo Baseline THẤT BẠI.");
        if (lastIRValue < 50000) {
          Serial.println("[DEBUG HR] Nguyên nhân: KHÔNG ĐEO THIẾT BỊ. Khóa chu "
                         "kỳ ngầm để tiết kiệm pin.");
          baselineMeasuredInThisSession = true;
        } else {
          Serial.println("[DEBUG HR] Nguyên nhân: Nhiễu. Sẽ thử lại sau 5s nếu "
                         "vẫn đứng im.");
          baselineMeasuredInThisSession = false;
          baselineMeasureDelay = 5000; // Đợi 5s rồi thử lại
          immobileStartTime = millis();
        }
      }
      hrState = HR_IDLE;
      particleSensor.shutDown();
    }
  }
}
