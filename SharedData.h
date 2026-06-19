#ifndef SHARED_DATA_H
#define SHARED_DATA_H

#define PPG_BUFFER_SIZE 128   // 128 mẫu @ 100Hz = 1.28s — đủ OLED + Web
#include <Arduino.h>

enum HRState {
    HR_IDLE,
    HR_MEASURING_BASELINE,
    HR_EMERGENCY_MEASURE,
    HR_MANUAL_MEASURE
};

extern HRState hrState;
extern unsigned long hrStateStartTime;

extern bool isManualMode;
extern int baselineHR;
extern int currentHR;
extern int beatAvg;
extern int currentSpO2;

extern bool isImmobile;
extern unsigned long immobileStartTime;
extern bool baselineMeasuredInThisSession;
extern unsigned long baselineMeasureDelay;

extern bool impactDetected;
extern bool fallConfirmed;
extern bool telegramSentForThisFall;
extern unsigned long fallConfirmTime;

// ================= PPG WAVEFORM BUFFER =================
extern float ppgBuffer[PPG_BUFFER_SIZE];
extern volatile int ppgWriteIdx;
extern float oled_max;
extern float oled_min;

// Spinlock bảo vệ ppgBuffer từ 2 core (portENTER/EXIT_CRITICAL)
extern portMUX_TYPE ppgMux;

// ================= BATTERY =================
extern float batteryPercent;
extern float batteryVoltage;

// ================= ACCEL/ANGLE (for web) =================
extern float filteredAccel;
extern float angle;
// ================= SENSOR STATE =================
extern long lastIRValue;  // Giá trị IR mới nhất từ MAX30102 (>50000 = có ngón tay)

#endif
