#include "sensors.h"

#define REG32(a) (*(volatile uint32_t *)(a))
#define RCC_AHBENR REG32(0x40021014u)
#define GPIOA_MODER REG32(0x48000000u)
#define GPIOA_IDR REG32(0x48000010u)
#define GPIOA_BSRR REG32(0x48000018u)
#define GPIOB_MODER REG32(0x48000400u)
#define GPIOB_OTYPER REG32(0x48000404u)
#define GPIOB_PUPDR REG32(0x4800040cu)
#define GPIOB_IDR REG32(0x48000410u)
#define GPIOB_BSRR REG32(0x48000418u)
#define GPIOE_MODER REG32(0x48001000u)
#define GPIOE_BSRR REG32(0x48001018u)
#define SCL (1u << 6)
#define SDA (1u << 7)
#define SCK (1u << 5)
#define MISO (1u << 6)
#define MOSI (1u << 7)
#define CS (1u << 3)

volatile SensorVector last_gyro, last_accel, last_mag;
volatile uint32_t sensor_status, gyro_id, mag_type;
static int accel_configured, mag_configured;

static void pause_bus(void)
{
    for (volatile unsigned i = 0; i < 40; ++i)
        __asm volatile("nop");
}
static void line(uint32_t pin, int high)
{
    GPIOB_BSRR = high ? pin : pin << 16;
    pause_bus();
}
static int clock_high(void)
{
    line(SCL, 1);
    for (unsigned i = 0; i < 1000; ++i)
        if (GPIOB_IDR & SCL)
            return 1;
    return 0;
}
static int start(void)
{
    line(SDA, 1);
    if (!clock_high() || !(GPIOB_IDR & SDA))
        return 0;
    line(SDA, 0);
    line(SCL, 0);
    return 1;
}
static void stop(void)
{
    line(SDA, 0);
    (void)clock_high();
    line(SDA, 1);
}
static int send_byte(uint8_t value)
{
    for (unsigned i = 0; i < 8; ++i)
    {
        line(SDA, value & 0x80);
        if (!clock_high())
            return 0;
        line(SCL, 0);
        value <<= 1;
    }
    line(SDA, 1);
    if (!clock_high())
        return 0;
    int ack = !(GPIOB_IDR & SDA);
    line(SCL, 0);
    return ack;
}
static int recv_byte(uint8_t *value, int more)
{
    uint8_t result = 0;
    line(SDA, 1);
    for (unsigned i = 0; i < 8; ++i)
    {
        if (!clock_high())
            return 0;
        result = (uint8_t)((result << 1) | !!(GPIOB_IDR & SDA));
        line(SCL, 0);
    }
    line(SDA, !more);
    if (!clock_high())
        return 0;
    line(SCL, 0);
    line(SDA, 1);
    *value = result;
    return 1;
}
static int i2c_write(uint8_t addr, uint8_t reg, uint8_t value)
{
    int ok = start() && send_byte(addr << 1) && send_byte(reg) && send_byte(value);
    stop();
    return ok;
}
static int i2c_read(uint8_t addr, uint8_t reg, uint8_t *data, unsigned count)
{
    int ok = start() && send_byte(addr << 1) && send_byte(reg) && start() && send_byte((addr << 1) | 1);
    for (unsigned i = 0; ok && i < count; ++i)
        ok = recv_byte(&data[i], i + 1 < count);
    stop();
    return ok;
}
static uint8_t spi_byte(uint8_t value)
{
    uint8_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
    {
        GPIOA_BSRR = SCK << 16;
        GPIOA_BSRR = (value & 0x80) ? MOSI : MOSI << 16;
        pause_bus();
        GPIOA_BSRR = SCK;
        pause_bus();
        result = (uint8_t)((result << 1) | !!(GPIOA_IDR & MISO));
        value <<= 1;
    }
    return result;
}
static void gyro_read(uint8_t reg, uint8_t *data, unsigned count)
{
    GPIOE_BSRR = CS << 16;
    pause_bus();
    (void)spi_byte(reg | 0x80u | (count > 1 ? 0x40u : 0));
    for (unsigned i = 0; i < count; ++i)
        data[i] = spi_byte(0xff);
    GPIOE_BSRR = CS;
    pause_bus();
}
static void gyro_write(uint8_t reg, uint8_t value)
{
    GPIOE_BSRR = CS << 16;
    pause_bus();
    (void)spi_byte(reg);
    (void)spi_byte(value);
    GPIOE_BSRR = CS;
    pause_bus();
}
static int16_t le(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static int16_t be(const uint8_t *p) { return (int16_t)(((uint16_t)p[0] << 8) | p[1]); }

void Sensors_Configure(void)
{
    RCC_AHBENR |= (1u << 17) | (1u << 18) | (1u << 21);
    // SPI mode 3, software-driven: PA5 SCK, PA6 MISO, PA7 MOSI, PE3 CS.
    GPIOA_BSRR = SCK | MOSI;
    GPIOA_MODER = (GPIOA_MODER & ~((3u << 10) | (3u << 12) | (3u << 14))) | (1u << 10) | (1u << 14);
    GPIOE_BSRR = CS;
    GPIOE_MODER = (GPIOE_MODER & ~(3u << 6)) | (1u << 6);
    // Open-drain I2C: PB6 SCL, PB7 SDA. Board pull-ups are fitted.
    GPIOB_BSRR = SCL | SDA;
    GPIOB_OTYPER |= SCL | SDA;
    GPIOB_PUPDR = (GPIOB_PUPDR & ~((3u << 12) | (3u << 14))) | (1u << 12) | (1u << 14);
    GPIOB_MODER = (GPIOB_MODER & ~((3u << 12) | (3u << 14))) | (1u << 12) | (1u << 14);

    uint8_t id = 0;
    sensor_status = gyro_id = mag_type = 0;
    last_gyro = (SensorVector){0, 0, 0};
    last_accel = (SensorVector){0, 0, 0};
    last_mag = (SensorVector){0, 0, 0};
    accel_configured = mag_configured = 0;
    gyro_read(0x0f, &id, 1);
    gyro_id = id;
    if (id == 0xd4 || id == 0xd3)
    {
        gyro_write(0x20, 0x0f); // normal mode, all axes
        gyro_write(0x23, 0x80); // block update, 250 dps
    }
    // Both accelerometers use the same register layout in normal mode.
    accel_configured = i2c_write(0x19, 0x20, 0x57) && // 100 Hz, all axes
                       i2c_write(0x19, 0x23, 0x80);   // block update, +/-2g
    if (i2c_read(0x1e, 0x4f, &id, 1) && id == 0x40)
    {
        mag_type = 2;
        mag_configured = i2c_write(0x1e, 0x60, 0x00) && // AGR continuous
                         i2c_write(0x1e, 0x62, 0x10);   // block update
    }
    else if (i2c_read(0x1e, 0x0a, &id, 1) && id == 0x48)
    {
        mag_type = 1;
        mag_configured = i2c_write(0x1e, 0x00, 0x14) && // DLHC 30 Hz
                         i2c_write(0x1e, 0x01, 0x20) && // +/-1.3 gauss
                         i2c_write(0x1e, 0x02, 0x00);   // continuous
    }
}

void Sensors_Read(void)
{
    uint8_t b[6];
    uint32_t status = 0;
    if (gyro_id == 0xd4 || gyro_id == 0xd3)
    {
        gyro_read(0x28, b, 6);
        last_gyro.x = le(b);
        last_gyro.y = le(b + 2);
        last_gyro.z = le(b + 4);
        status |= 1;
    }
    if (accel_configured && i2c_read(0x19, 0xa8, b, 6))
    {
        last_accel.x = le(b);
        last_accel.y = le(b + 2);
        last_accel.z = le(b + 4);
        status |= 2;
    }
    if (mag_configured && mag_type == 1 && i2c_read(0x1e, 0x03, b, 6))
    {
        last_mag.x = be(b);
        last_mag.z = be(b + 2);
        last_mag.y = be(b + 4);
        status |= 4;
    }
    else if (mag_configured && mag_type == 2 && i2c_read(0x1e, 0xe8, b, 6))
    {
        last_mag.x = le(b);
        last_mag.y = le(b + 2);
        last_mag.z = le(b + 4);
        status |= 4;
    }
    sensor_status = status;
}
