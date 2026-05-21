/**
 * @file    main_example.c
 * @brief   Contoh pemakaian driver ADS1232 untuk weigh-scale (Fig 8-1).
 *
 * Hardware mapping (silakan sesuaikan dengan pinout PCB):
 *   ADS1232 SCLK       <- STM32 PA5  (output PP)
 *   ADS1232 DRDY/DOUT  -> STM32 PA6  (input, no pull - DOUT push-pull)
 *   ADS1232 PDWN       <- STM32 PA7  (output PP)
 *
 * Catatan:
 *   - DVDD ADS1232 disarankan disambung ke 3.3V (sama dengan STM32 VDD)
 *     untuk menghindari level mismatch. Pin AVDD tetap 5V untuk
 *     optimal noise / bridge excitation.
 *   - GAIN1, GAIN0 di-tie ke 3.3V (gain = 128)
 *   - SPEED, A0, TEMP, CLKIN/XTAL1 di-tie ke GND
 *   - 0.1uF X7R cap di tiap pin VDD + 0.1uF C0G/X7R antara pin 9-10 (CAP)
 */

#include "stm32f4xx_hal.h"
#include "ads1232.h"
#include <stdio.h>
#include <string.h>

/* Asumsikan ada UART2 untuk debug print. Ganti sesuai project. */
extern UART_HandleTypeDef huart2;

/* Redirect printf ke UART (opsional) */
int __io_putchar(int ch) {
    HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
    return ch;
}

static ads1232_t adc = {
    .sclk_port = GPIOA, .sclk_pin = GPIO_PIN_5,
    .dout_port = GPIOA, .dout_pin = GPIO_PIN_6,
    .pdwn_port = GPIOA, .pdwn_pin = GPIO_PIN_7,
    .gain      = ADS1232_GAIN_128,
    .speed     = ADS1232_SPEED_10SPS,
    .vref      = 5.0f,
};

/* Wajib panggil ini di MX_GPIO_Init() atau buat sendiri */
static void ads1232_gpio_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gi = {0};

    /* SCLK + PDWN : output push-pull, no pull, low speed cukup */
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    gi.Pin   = GPIO_PIN_5 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &gi);

    /* DRDY/DOUT : input, no pull (DOUT push-pull dari ADS) */
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_NOPULL;
    gi.Pin  = GPIO_PIN_6;
    HAL_GPIO_Init(GPIOA, &gi);

    /* Default state */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET); /* SCLK low */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET); /* PDWN low (start) */
}

/* ===================== Demo loop ===================== */

void app_main(void)
{
    ads1232_gpio_init();
    ads1232_init(&adc);

    /* Optional: lakukan offset calibration setelah power-up */
    printf("Calibrating offset...\r\n");
    ads1232_calibrate_offset(&adc);

    /* Optional: tare timbangan (asumsi tanpa beban saat ini) */
    printf("Taring (no load)...\r\n");
    if (!ads1232_tare(&adc, 10)) {
        printf("Tare failed (timeout)\r\n");
    }

    /* Optional: kalibrasi scale dengan known weight.
     * Contoh manual workflow di runtime:
     *   1. user pasang beban referensi (mis. 1000 gram)
     *   2. baca raw rata-rata
     *   3. panggil ads1232_set_scale(&adc, raw, 1000.0f);
     * Atau hardcode dari hasil kalibrasi sebelumnya:
     *   adc.scale_per_unit = 12345.0f;  // count per gram
     */

    while (1) {
        /* === Mode 1: baca tegangan diferensial input langsung === */
        float v;
        if (ads1232_read_voltage(&adc, &v, 200)) {
            /* v adalah tegangan output bridge (sebelum gain).
             * Untuk load cell 2 mV/V dengan eksitasi 5V:
             *   FS bridge output = 10 mV
             *   Range ADC pada gain=128 = ±19.5 mV (cukup) */
            printf("V_in = %+.4f mV\r\n", v * 1000.0f);
        } else {
            printf("ADC timeout\r\n");
        }

        /* === Mode 2: baca berat (jika scale sudah di-set) === */
        /*
        float w;
        if (ads1232_read_weight(&adc, &w, 4, 200)) {
            printf("Weight = %.2f g\r\n", w);
        }
        */

        /* Tanpa delay tambahan: pada 10 SPS, loop ini akan idle
         * di wait_ready selama ~100ms per pembacaan. */
    }
}

/* ===================== Catatan tambahan =====================
 *
 * 1. Polling vs Interrupt:
 *    Versi ini polling. Untuk power-sensitive design atau RTOS,
 *    gunakan EXTI falling-edge pada pin DRDY/DOUT:
 *
 *      void EXTI9_5_IRQHandler(void) {
 *          HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_6);
 *      }
 *      void HAL_GPIO_EXTI_Callback(uint16_t pin) {
 *          if (pin == GPIO_PIN_6) {
 *              data_ready_flag = 1;  // signal task / semaphore
 *          }
 *      }
 *
 *    Catatan: setelah ads1232_read_raw() selesai, pin DOUT akan
 *    high (karena pulse ke-25). EXTI falling edge akan trigger lagi
 *    ketika konversi berikutnya siap.
 *
 * 2. Filtering:
 *    Per datasheet section 8.2.2, post-averaging (moving average N=4)
 *    meningkatkan noise-free resolution dari 85k counts -> 135k counts
 *    dengan tradeoff settling time 500ms -> 900ms. Fungsi
 *    ads1232_read_raw_avg() sudah menyediakan ini.
 *
 * 3. Channel switching (jika pakai ADS1234 atau A0/TEMP dinamis):
 *    Setelah toggle A0/A1/TEMP pin, DRDY akan high sampai filter
 *    fully settled (4 cycles, ~400ms @ 10SPS). Driver ini tidak
 *    mengontrol A0/A1 -- tambahkan jika diperlukan.
 *
 * 4. Pemilihan keluarga STM32:
 *    Ganti `#include "stm32f4xx_hal.h"` di ads1232.h dan file ini
 *    sesuai chip. Untuk Cortex-M0/M0+ (F0, L0, G0), DWT cycle counter
 *    tidak tersedia -- ganti delay_us() dengan TIM-based atau
 *    SysTick reload.
 */
