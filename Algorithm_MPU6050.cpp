#include "Algorithm_MPU6050.h"
#include "Algorithm_MAX30102.h" // for resetMAX30102Measurement()

// ================= MPU Variables =================
float ax, ay, az;
float gx_deg, gy_deg, gz_deg;
float totalGyro;
float totalAccel;
float filteredAccel = 1.0;
const float alpha = 0.85;
float angle = 0;
float stableAngle = 0;

float peakGyro = 0;
float postImpactMaxG = 0;
float postImpactMinG = 0;
float postImpactMaxAngle = 0;
float postImpactMinAngle = 0;
bool isTrackingImmobility = false;

unsigned long impactTime = 0;
bool freefallDetected = false;
unsigned long freefallTime = 0;

void processMPU6050Data(MPU6050 &mpu, MAX30105 &particleSensor) {
    // ==================================================
    // 2. MPU6050 READ & FILTER
    // ==================================================
    int16_t axRaw, ayRaw, azRaw, gxRaw, gyRaw, gzRaw;
    mpu.getMotion6(&axRaw, &ayRaw, &azRaw, &gxRaw, &gyRaw, &gzRaw);
    ax = axRaw / 8192.0; ay = ayRaw / 8192.0; az = azRaw / 8192.0;
    
    gx_deg = gxRaw / 65.5;
    gy_deg = gyRaw / 65.5;
    gz_deg = gzRaw / 65.5;
    totalGyro = sqrt(gx_deg*gx_deg + gy_deg*gy_deg + gz_deg*gz_deg);

    totalAccel = sqrt(ax * ax + ay * ay + az * az);
    filteredAccel = alpha * filteredAccel + (1 - alpha) * totalAccel;

    float pitch = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;
    float roll = atan2(ay, az) * 180.0 / PI;
    angle = sqrt(pitch * pitch + roll * roll);

    // Chỉ cập nhật góc nền (stableAngle) khi hệ thống tĩnh lặng
    if (!impactDetected && !fallConfirmed && totalAccel > 0.8 && totalAccel < 1.2 && totalGyro < 50) {
        stableAngle = 0.95 * stableAngle + 0.05 * angle;
    }

    // ==================================================
    // 3. BACKGROUND BASELINE HR MEASUREMENT (Đã vô hiệu hóa để tối ưu pin khi đeo ở bụng)
    // ==================================================
    if (!isManualMode && filteredAccel > 0.92 && filteredAccel < 1.08 && !impactDetected && !fallConfirmed) {
        if (!isImmobile) {
            isImmobile = true; immobileStartTime = millis();
            Serial.println("[DEBUG MPU] Bắt đầu đứng im (Immobile)");
        }
    } else {
        isImmobile = false;
    }

    // ==================================================
    // 5. THUẬT TOÁN PHÁT HIỆN NGÃ (SENSOR FUSION)
    // ==================================================
    
    if (!fallConfirmed) {
        // Nạn nhân phải thực sự rơi tự do thì tổng gia tốc mới giảm sâu. Vung tay bình thường chỉ giảm xuống ~0.5g - 0.6g.
        if (totalAccel < 0.4) { 
            if (!freefallDetected) Serial.println("[DEBUG FALL] BƯỚC 1: Rơi Tự Do (Freefall)");
            freefallDetected = true; freefallTime = millis(); 
        }
        if (freefallDetected && (millis() - freefallTime > 1000)) freefallDetected = false;

        if (!impactDetected) {
            if (totalAccel > 2.5 || (freefallDetected && totalAccel > 2.0)) {
                impactDetected = true; impactTime = millis(); freefallDetected = false;
                peakGyro = totalGyro; isTrackingImmobility = false;
                Serial.print("[DEBUG FALL] BƯỚC 2: VA CHẠM! Đỉnh G = "); Serial.println(totalAccel);
                Serial.println("[DEBUG FALL] Đang theo dõi cửa sổ 2.5s để phân tích tư thế...");
            }
        }
    }

    if (impactDetected && !fallConfirmed) {
        // Theo dõi Peak Gyro trong 500ms đầu tiên
        if (millis() - impactTime <= 500) {
            if (totalGyro > peakGyro) peakGyro = totalGyro;
        }
        
        // Từ 500ms đến 2500ms (và tiếp tục sau đó): Theo dõi độ dao động G và Góc
        if (millis() - impactTime > 500 && !isTrackingImmobility) {
            isTrackingImmobility = true;
            postImpactMaxG = filteredAccel; postImpactMinG = filteredAccel;
            postImpactMaxAngle = angle; postImpactMinAngle = angle;
        }

        if (isTrackingImmobility) {
            if (filteredAccel > postImpactMaxG) postImpactMaxG = filteredAccel;
            if (filteredAccel < postImpactMinG) postImpactMinG = filteredAccel;
            if (angle > postImpactMaxAngle) postImpactMaxAngle = angle;
            if (angle < postImpactMinAngle) postImpactMinAngle = angle;
        }

        // Hết 2.5 giây, chốt kết quả dựa hoàn toàn vào MPU6050
        if (millis() - impactTime > 2500) {
            float gRange = postImpactMaxG - postImpactMinG;
            float angleRange = postImpactMaxAngle - postImpactMinAngle;
            
            float gLimit = 1.5;
            float angleLimit = 60;

            bool isImmobilePostFall = (gRange < gLimit) && (angleRange < angleLimit) && (filteredAccel > 0.7 && filteredAccel < 1.3);

            if (isImmobilePostFall) {
                if (peakGyro > 150) {
                    float finalAngleDiff = abs(angle - stableAngle);
                    if (finalAngleDiff > 30) {
                        Serial.println("[DEBUG FALL] BƯỚC 3: Bất động + Gyro Cao + Góc đổi >30* -> CHỐT NGÃ!");
                        fallConfirmed = true; fallConfirmTime = millis(); telegramSentForThisFall = false;
                    } else {
                        Serial.print("[DEBUG FALL] Hủy báo ngã: Góc thay đổi nhỏ (");
                        Serial.print(finalAngleDiff); Serial.println("*)");
                        impactDetected = false;
                    }
                } else {
                    Serial.println("[DEBUG FALL] Hủy báo ngã: Tốc độ xoay chậm (Có thể ngồi/nằm từ từ xuống).");
                    impactDetected = false; 
                }
            } else {
                Serial.print("[DEBUG FALL] Hủy báo ngã: Không bất động (G_range = "); 
                Serial.print(gRange); Serial.print("g, Angle_range = ");
                Serial.print(angleRange); Serial.println("*)");
                impactDetected = false; 
            }
        }

        if (millis() - impactTime > 15000) {
            impactDetected = false;
        }
    }
}
