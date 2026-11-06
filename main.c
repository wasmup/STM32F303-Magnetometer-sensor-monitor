
#include <stdint.h>
#include "sensors.h"

#define REG32(addr) (*(volatile uint32_t *)(addr))

// RCC
#define RCC_AHBENR REG32(0x40021014u)

// GPIOE
#define GPIOE_MODER REG32(0x48001000u)
#define GPIOE_BSRR REG32(0x48001018u)

// SysTick
#define SYST_CSR REG32(0xE000E010u)
#define SYST_RVR REG32(0xE000E014u)
#define SYST_CVR REG32(0xE000E018u)

// ADC1
#define ADC1_ISR REG32(0x50000000u)
#define ADC1_CR REG32(0x50000008u)
#define ADC1_CFGR REG32(0x5000000Cu)
#define ADC1_SMPR2 REG32(0x50000018u)
#define ADC1_SQR1 REG32(0x50000030u)
#define ADC1_DR REG32(0x50000040u)

// ADC1/ADC2 common control
#define ADC12_CCR REG32(0x50000308u)

#define CPU_HZ 8000000u
static const uint8_t led_pins[8] = {
    13, 14, 15, 8, 9, 10, 11, 12};

void delay_ms(uint32_t ms)
{
    SYST_RVR = (CPU_HZ / 1000u) - 1u;
    SYST_CVR = 0;
    SYST_CSR = (1u << 2) | (1u << 0);

    while (ms--)
    {
        while (!(SYST_CSR & (1u << 16)))
        {
        }
    }

    SYST_CSR = 0;
}

void LED_Configure(void)
{
    RCC_AHBENR |= (1u << 21);

    GPIOE_MODER =
        (GPIOE_MODER & ~0xFFFF0000u) |
        0x55550000u;

    GPIOE_BSRR = 0xFF000000u;
}

void LED_SetByte(uint8_t value)
{
    uint32_t set = 0;
    uint32_t reset = 0;

    for (unsigned bit = 0; bit < 8; ++bit)
    {
        uint32_t pin = 1u << led_pins[bit];

        if (value & (1u << bit))
            set |= pin;
        else
            reset |= pin;
    }

    GPIOE_BSRR = set | (reset << 16);
}

#define ADC_TEMP_CHANNEL 16u
#define ADC_VREF_CHANNEL 18u
#define TS_CAL1 (*(const volatile uint16_t *)0x1FFFF7B8u)
#define VREFINT_CAL (*(const volatile uint16_t *)0x1FFFF7BAu)
#define TS_CAL2 (*(const volatile uint16_t *)0x1FFFF7C2u)

// Latest raw conversions, exposed to GDB.
volatile uint16_t last_temp_adc;
volatile uint16_t last_vref_adc;

void ADC_Configure(void)
{
    RCC_AHBENR |= (1u << 28);

    // ADC synchronous clock = HCLK / 1.
    ADC12_CCR = (ADC12_CCR & ~(3u << 16)) | (1u << 16);

    // Enable internal voltage regulator.
    ADC1_CR &= ~(3u << 28);
    ADC1_CR |= (1u << 28);
    delay_ms(1);

    // Single-ended calibration.
    ADC1_CR &= ~(1u << 30);
    ADC1_CR |= (1u << 31);

    while (ADC1_CR & (1u << 31))
    {
    }

    // 12-bit, single conversion, software trigger.
    ADC1_CFGR = 0;

    // Long sampling time for channels 16 and 18.
    // SMP16: SMPR2 bits 20:18
    // SMP18: SMPR2 bits 26:24
    ADC1_SMPR2 |= (7u << 18) | (7u << 24);

    // Enable temperature sensor and VREFINT.
    ADC12_CCR |= (1u << 23) | (1u << 22);
    delay_ms(1);

    ADC1_ISR = 1u;
    ADC1_CR |= 1u;

    while (!(ADC1_ISR & 1u))
    {
    }
}

uint16_t ADC_ReadChannel(uint8_t channel)
{
    // Select one regular conversion.
    ADC1_SQR1 = ((uint32_t)channel << 6);

    // Clear end-of-conversion flag.
    ADC1_ISR = (1u << 2);

    // Start conversion.
    ADC1_CR |= (1u << 2);

    while (!(ADC1_ISR & (1u << 2)))
    {
    }

    return (uint16_t)(ADC1_DR & 0x0FFFu);
}

// Calculate the same factory-calibrated die temperature as the monitor.
// The LED byte is a signed, rounded Celsius value in two's complement.
static uint8_t Temperature_LED(void)
{
    int32_t cal1 = TS_CAL1, cal2 = TS_CAL2, vref_cal = VREFINT_CAL;
    int32_t vref = last_vref_adc;
    if (!vref || !vref_cal || vref_cal > 4095 || !cal1 || cal1 > 4095 ||
        !cal2 || cal2 > 4095 || cal1 == cal2)
        return 0;

    int32_t denominator = (cal2 - cal1) * vref;
    int32_t numerator = 30 * denominator +
        80 * ((int32_t)last_temp_adc * vref_cal - cal1 * vref);
    if (denominator < 0) {
        denominator = -denominator;
        numerator = -numerator;
    }
    int32_t temperature = numerator / denominator;
    int32_t remainder = numerator % denominator;
    if (remainder >= (denominator + 1) / 2)
        ++temperature;
    else if (remainder <= -(denominator + 1) / 2)
        --temperature;
    if (temperature > 127) temperature = 127;
    if (temperature < -128) temperature = -128;
    return (uint8_t)temperature;
}

int main(void)
{
    LED_Configure();
    ADC_Configure();
    Sensors_Configure();

    for (;;)
    {
        // Discard the first conversion after each channel change.
        (void)ADC_ReadChannel(ADC_TEMP_CHANNEL);
        last_temp_adc = ADC_ReadChannel(ADC_TEMP_CHANNEL);
        (void)ADC_ReadChannel(ADC_VREF_CHANNEL);
        last_vref_adc = ADC_ReadChannel(ADC_VREF_CHANNEL);

        Sensors_Read();

        LED_SetByte(Temperature_LED());
    }
}
