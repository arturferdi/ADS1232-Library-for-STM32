/**
 * @file    ads1232.c
 * @brief   ADS1232 driver implementation untuk STM32 HAL.
 *
 * Protokol kunci dari datasheet:
 *   - DRDY/DOUT goes LOW = data baru siap dibaca
 *   - Pulse SCLK 24x, MSB first, baca DOUT setelah rising edge (t4 max 50ns)
 *   - t3 (SCLK pulse width) >= 100ns -> aman pakai delay ~1us
 *   - Pulse ke-25 untuk paksa DRDY high (memudahkan polling siklus berikutnya)
 *   - Offset cal: 26 pulse total, falling edge ke-26 trigger calibration
 *   - Standby: tahan SCLK high setelah DRDY low
 *   - Power-up: PDWN low->high->low->high dengan timing t15/t16/t17
 */

#include "ads1232.h"

/* ===================== Micro-second delay =====================
 * Implementasi sederhana menggunakan DWT cycle counter (Cortex-M3/M4/M7).
 * Untuk Cortex-M0/M0+ (tanpa DWT), ganti dengan TIM-based delay
 * atau hitung kasar dengan __NOP() loop. */

static void dwt_delay_init(void)
{
    static uint8_t inited = 0;
    if (inited) return;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
    inited = 1;
}

static inline void delay_us(uint32_t us)
{
    uint32_t start  = DWT->CYCCNT;
    uint32_t cycles = (HAL_RCC_GetHCLKFreq() / 1000000U) * us;
    while ((DWT->CYCCNT - start) < cycles) { /* spin */ }
}

/* ===================== Pin helpers ===================== */

static inline void sclk_high(ads1232_t *d)
{
    HAL_GPIO_WritePin(d->sclk_port, d->sclk_pin, GPIO_PIN_SET);
}
static inline void sclk_low(ads1232_t *d)
{
    HAL_GPIO_WritePin(d->sclk_port, d->sclk_pin, GPIO_PIN_RESET);
}
static inline void pdwn_high(ads1232_t *d)
{
    HAL_GPIO_WritePin(d->pdwn_port, d->pdwn_pin, GPIO_PIN_SET);
}
static inline void pdwn_low(ads1232_t *d)
{
    HAL_GPIO_WritePin(d->pdwn_port, d->pdwn_pin, GPIO_PIN_RESET);
}
static inline uint32_t dout_read(ads1232_t *d)
{
    return (HAL_GPIO_ReadPin(d->dout_port, d->dout_pin) == GPIO_PIN_SET) ? 1U : 0U;
}

/* SCLK pulse: high - delay - low - delay. ~2us total, aman untuk t3 min 100ns. */
static inline void sclk_pulse(ads1232_t *d)
{
    sclk_high(d);
    delay_us(1);
    sclk_low(d);
    delay_us(1);
}

/* ===================== Public API ===================== */

void ads1232_init(ads1232_t *dev)
{
    dwt_delay_init();
    sclk_low(dev);
    dev->offset_raw     = 0;
    dev->scale_per_unit = 1.0f;
    ads1232_power_up(dev);
}

void ads1232_power_up(ads1232_t *dev)
{
    /* Power-up sequence per Fig 7-15:
     *   PDWN low (selama supply stabil) -> high (t16) -> low (t17) -> high (run)
     * t15 min 10us, t16 dan t17 min 26us. Pakai 1ms / 50us untuk safety margin. */
    pdwn_low(dev);
    HAL_Delay(1);            /* t15: tunggu supply stabil (>>10us) */
    pdwn_high(dev);
    delay_us(50);            /* t16 >= 26us */
    pdwn_low(dev);
    delay_us(50);            /* t17 >= 26us */
    pdwn_high(dev);
    /* Wake-up dari power-down: internal oscillator butuh ~8us (t13).
     * Konversi pertama yang valid butuh waktu settling (t11): ~52ms (80SPS)
     * atau ~402ms (10SPS). HAL_Delay(500) aman untuk kedua mode. */
    HAL_Delay(500);
}

void ads1232_power_down(ads1232_t *dev)
{
    pdwn_low(dev);
}

bool ads1232_data_ready(ads1232_t *dev)
{
    return dout_read(dev) == 0U;
}

bool ads1232_wait_ready(ads1232_t *dev, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (!ads1232_data_ready(dev)) {
        if ((HAL_GetTick() - start) > timeout_ms) return false;
    }
    return true;
}

int32_t ads1232_read_raw(ads1232_t *dev)
{
    uint32_t data = 0;

    /* Pastikan SCLK start low */
    sclk_low(dev);

    /* Shift in 24 bit, MSB first.
     * Sequence per Fig 7-9: rising edge -> data valid setelah t4 (50ns max).
     * Kita delay 1us sebelum baca, jauh di atas margin. */
    for (int i = 0; i < 24; i++) {
        sclk_high(dev);
        delay_us(1);
        data = (data << 1) | dout_read(dev);
        sclk_low(dev);
        delay_us(1);
    }

    /* Pulse ke-25 untuk force DRDY/DOUT high. Memastikan polling siklus
     * berikutnya bisa langsung mendeteksi falling edge. */
    sclk_pulse(dev);

    /* Sign-extend 24-bit two's complement ke int32_t */
    if (data & 0x800000U) {
        data |= 0xFF000000U;
    }
    return (int32_t)data;
}

float ads1232_raw_to_volts(ads1232_t *dev, int32_t raw)
{
    /* Per datasheet 7.3.9:
     *   Code 7FFFFFh (8388607)  = +0.5 * VREF / Gain
     *   Code 800000h (-8388608) = -0.5 * VREF / Gain
     *   LSB weight = 0.5 * VREF / (Gain * (2^23 - 1))
     *
     * V_in = raw_code * LSB_weight
     *
     * Contoh: VREF=5V, Gain=128 -> FSR = 39.0625mV (full-scale ±19.5mV)
     *         LSB = 5/(2 * 128 * 8388607) ≈ 2.328 nV
     */
    const float lsb = (0.5f * dev->vref) /
                      ((float)dev->gain * 8388607.0f);
    return (float)raw * lsb;
}

bool ads1232_read_voltage(ads1232_t *dev, float *out_volts, uint32_t timeout_ms)
{
    if (!ads1232_wait_ready(dev, timeout_ms)) return false;
    int32_t raw = ads1232_read_raw(dev);
    *out_volts  = ads1232_raw_to_volts(dev, raw);
    return true;
}

bool ads1232_read_raw_avg(ads1232_t *dev, uint8_t n_samples,
                          int32_t *out_avg, uint32_t timeout_per_sample_ms)
{
    if (n_samples == 0) return false;

    int64_t accum = 0;
    for (uint8_t i = 0; i < n_samples; i++) {
        if (!ads1232_wait_ready(dev, timeout_per_sample_ms)) return false;
        accum += ads1232_read_raw(dev);
    }
    *out_avg = (int32_t)(accum / (int64_t)n_samples);
    return true;
}

void ads1232_calibrate_offset(ads1232_t *dev)
{
    /* Per Fig 7-11: shift 24 bit, lalu 25th SCLK (force DRDY high),
     * lalu 26th SCLK (falling edge -> mulai calibration).
     * Setelah itu DRDY akan high sampai cal selesai (t8). */
    if (!ads1232_wait_ready(dev, 1000)) return;

    sclk_low(dev);

    /* 24 SCLK: shift out (discard) data lama */
    for (int i = 0; i < 24; i++) {
        sclk_pulse(dev);
    }
    /* 25th SCLK: force DRDY high */
    sclk_pulse(dev);
    /* 26th SCLK: falling edge mulai calibration */
    sclk_pulse(dev);

    /* Tunggu cal selesai: t8 max ~801ms (10SPS), ~101ms (80SPS).
     * Pakai timeout 1.2s aman untuk dua-duanya. */
    ads1232_wait_ready(dev, 1200);
}

bool ads1232_tare(ads1232_t *dev, uint8_t n_samples)
{
    int32_t avg;
    if (!ads1232_read_raw_avg(dev, n_samples, &avg, 500)) return false;
    dev->offset_raw = avg;
    return true;
}

void ads1232_set_scale(ads1232_t *dev, int32_t raw_at_known_weight,
                       float known_weight)
{
    if (known_weight == 0.0f) return;
    float delta = (float)(raw_at_known_weight - dev->offset_raw);
    dev->scale_per_unit = delta / known_weight;
}

bool ads1232_read_weight(ads1232_t *dev, float *out_weight,
                         uint8_t n_samples, uint32_t timeout_per_sample_ms)
{
    int32_t avg;
    if (!ads1232_read_raw_avg(dev, n_samples, &avg,
                              timeout_per_sample_ms)) return false;
    if (dev->scale_per_unit == 0.0f) return false;
    *out_weight = (float)(avg - dev->offset_raw) / dev->scale_per_unit;
    return true;
}

void ads1232_enter_standby(ads1232_t *dev)
{
    /* Per Fig 7-12: tunggu DRDY low, lalu tahan SCLK high.
     * Setelah t10 (~12.5ms / ~100ms tergantung mode) device akan standby. */
    ads1232_wait_ready(dev, 1000);
    sclk_high(dev);
}

void ads1232_exit_standby(ads1232_t *dev)
{
    /* Set SCLK low untuk wakeup. Data pertama valid setelah t11:
     *   - SPEED=1 (80SPS): ~52.5ms
     *   - SPEED=0 (10SPS): ~402ms */
    sclk_low(dev);
}
