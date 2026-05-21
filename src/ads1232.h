/**
 * @file    ads1232.h
 * @brief   Driver ADS1232 24-bit ADC untuk STM32 (HAL-based, bit-banged GPIO)
 *
 * Asumsi hardware (Fig 8-1 datasheet, weigh-scale config):
 *   - Channel 1 (A0=0, TEMP=0 hardwired to GND)
 *   - Gain = 128 (GAIN1=GAIN0=1 hardwired to VCC)
 *   - SPEED = 0 (10 SPS, hardwired to GND)
 *   - Internal oscillator (CLKIN/XTAL1 = GND)
 *   - VREF = 5V (excitation bridge = AVDD)
 *
 * Pin yang dikontrol MCU:
 *   - PDWN       : output push-pull (active-low power-down)
 *   - SCLK       : output push-pull (serial clock)
 *   - DRDY/DOUT  : input (dual-purpose data-ready + data-out)
 *
 * Catatan level: ADS1232 DVDD pada Fig 8-1 = 3V. Jika MCU = 3.3V,
 * pasang DVDD = 3.3V juga (range 2.7-5.3V), jangan biarkan beda.
 */

#ifndef ADS1232_H
#define ADS1232_H

#include "stm32f4xx_hal.h"   /* ganti sesuai keluarga STM32 (f1xx/l4xx/h7xx/etc) */
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PGA gain hardware-strapped pada PCB. Nilai di sini hanya untuk
 * konversi software dari raw code ke tegangan. */
typedef enum {
    ADS1232_GAIN_1   = 1,
    ADS1232_GAIN_2   = 2,
    ADS1232_GAIN_64  = 64,
    ADS1232_GAIN_128 = 128
} ads1232_gain_t;

typedef enum {
    ADS1232_SPEED_10SPS = 0,
    ADS1232_SPEED_80SPS = 1
} ads1232_speed_t;

typedef struct {
    /* Pin definitions */
    GPIO_TypeDef *sclk_port;
    uint16_t      sclk_pin;
    GPIO_TypeDef *dout_port;     /* DRDY/DOUT pin (input from ADC) */
    uint16_t      dout_pin;
    GPIO_TypeDef *pdwn_port;
    uint16_t      pdwn_pin;

    /* Hardware configuration (untuk konversi software) */
    ads1232_gain_t  gain;
    ads1232_speed_t speed;
    float           vref;        /* dalam Volt, mis. 5.0f */

    /* Internal state */
    int32_t  offset_raw;         /* offset hasil zero-tare (raw code) */
    float    scale_per_unit;     /* misal: count per gram, untuk weigh-scale */
} ads1232_t;

/* ===================== API ===================== */

/** Init driver: power-up sequence + set SCLK low. */
void    ads1232_init(ads1232_t *dev);

/** Apakah data baru siap (DRDY/DOUT == LOW). */
bool    ads1232_data_ready(ads1232_t *dev);

/** Tunggu sampai DRDY/DOUT low. Return false jika timeout. */
bool    ads1232_wait_ready(ads1232_t *dev, uint32_t timeout_ms);

/** Baca 24-bit raw two's complement, sign-extended ke int32_t.
 *  Range: -8388608 ... +8388607.
 *  Asumsi DRDY/DOUT sudah low (panggil wait_ready dulu). */
int32_t ads1232_read_raw(ads1232_t *dev);

/** Konversi raw code ke tegangan diferensial input (V). */
float   ads1232_raw_to_volts(ads1232_t *dev, int32_t raw);

/** Kombinasi wait_ready + read_raw + konversi tegangan. */
bool    ads1232_read_voltage(ads1232_t *dev, float *out_volts, uint32_t timeout_ms);

/** Rata-rata N pembacaan untuk reduksi noise (moving average sederhana). */
bool    ads1232_read_raw_avg(ads1232_t *dev, uint8_t n_samples,
                             int32_t *out_avg, uint32_t timeout_per_sample_ms);

/** Trigger internal offset calibration. Blocking sampai DRDY low lagi.
 *  Lakukan setelah suhu/supply stabil, atau periodik. */
void    ads1232_calibrate_offset(ads1232_t *dev);

/** Set current reading sebagai zero (tare). Berguna untuk timbangan. */
bool    ads1232_tare(ads1232_t *dev, uint8_t n_samples);

/** Setelah tare + load reference weight, hitung scale (count per unit). */
void    ads1232_set_scale(ads1232_t *dev, int32_t raw_at_known_weight,
                          float known_weight);

/** Baca berat (atau besaran fisik lain) dalam unit yang sudah dikalibrasi. */
bool    ads1232_read_weight(ads1232_t *dev, float *out_weight,
                            uint8_t n_samples, uint32_t timeout_per_sample_ms);

/** Masuk standby mode (low-power, ~100uA). SCLK ditahan HIGH. */
void    ads1232_enter_standby(ads1232_t *dev);

/** Keluar dari standby. Data pertama valid setelah ~52ms (80SPS) atau ~402ms (10SPS). */
void    ads1232_exit_standby(ads1232_t *dev);

/** Power-down penuh (PDWN low, ~0 uA). */
void    ads1232_power_down(ads1232_t *dev);

/** Power-up sequence sesuai Fig 7-15. */
void    ads1232_power_up(ads1232_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* ADS1232_H */
