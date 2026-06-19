#ifndef ALGORITHM_MPU6050_H
#define ALGORITHM_MPU6050_H

#include <Arduino.h>
#include <MPU6050.h>
#include "MAX30105.h"
#include "SharedData.h"

void processMPU6050Data(MPU6050 &mpu, MAX30105 &particleSensor);

// stableAngle: góc nền ổn định (tham chiếu để so sánh sau va chạm)
extern float stableAngle;

#endif
