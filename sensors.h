#ifndef SENSORS_H
#define SENSORS_H
#include <stdint.h>

typedef struct
{
    int16_t x, y, z;
} SensorVector;
extern volatile SensorVector last_gyro, last_accel, last_mag;
// Bit 0: gyro, bit 1: accelerometer, bit 2: magnetometer valid.
extern volatile uint32_t sensor_status;
// Gyroscope WHO_AM_I and magnetometer type: 1 = DLHC, 2 = AGR.
extern volatile uint32_t gyro_id, mag_type;
void Sensors_Configure(void);
void Sensors_Read(void);
#endif
