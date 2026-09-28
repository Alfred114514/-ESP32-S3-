#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "encoder.h"

static const char *TAG = "encoder";

/* 累计旋转步数：encoder_task 写，其它任务读（encoder_read_delta 读后清零）。 */
static volatile int32_t g_enc_count = 0;
/* 按键是否按下（已消抖）。 */
static volatile bool g_btn_pressed = false;
/* 按键连续按下时长（毫秒），松开清零。 */
static volatile uint32_t g_btn_press_ms = 0;

static void encoder_task(void *arg)
{
    int last_clk = gpio_get_level(ENCODER_CLK_GPIO);
    uint8_t btn_debounce = 0;

    while (1) {
        int clk = gpio_get_level(ENCODER_CLK_GPIO);

        /* 检测 CLK 下降沿：一次物理"咔哒"约对应一个下降沿，正好一步一个菜单项。
         * 若方向反了，交换 ENCODER_CLK/DT 两个宏，或把这里的加减号对调。 */
        if (clk == 0 && last_clk == 1) {
            int dt = gpio_get_level(ENCODER_DT_GPIO);
            g_enc_count += (dt ? 1 : -1);
        }
        last_clk = clk;

        /* 按键消抖：连续 3 次(约30ms)读到低电平才算按下 */
        if (gpio_get_level(ENCODER_SW_GPIO) == 0) {
            if (btn_debounce < 3) btn_debounce++;
            if (btn_debounce == 3) {
                g_btn_pressed = true;
                g_btn_press_ms += 10;   // 每循环约 10ms（100Hz tick）
            }
        } else {
            btn_debounce = 0;
            g_btn_pressed = false;
            g_btn_press_ms = 0;
        }

        /* 关键：vTaskDelay(1) = 1 个 tick（本工程 CONFIG_FREERTOS_HZ=100，即 10ms）。
         * 不能用 pdMS_TO_TICKS(1)，它在 100Hz 下整数截断成 0，vTaskDelay(0) 等于不阻塞，
         * 会死循环占满 CPU 饿死空闲任务，触发任务看门狗复位。 */
        vTaskDelay(1);
    }
}

void encoder_init(void)
{
    gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << ENCODER_CLK_GPIO) |
                        (1ULL << ENCODER_DT_GPIO)  |
                        (1ULL << ENCODER_SW_GPIO),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&cfg);

    xTaskCreatePinnedToCore(encoder_task, "encoder", 2048, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "编码器已初始化: CLK=%d DT=%d SW=%d（GPIO，可在 encoder.h 修改）",
             ENCODER_CLK_GPIO, ENCODER_DT_GPIO, ENCODER_SW_GPIO);
}

int32_t encoder_read_delta(void)
{
    int32_t d = g_enc_count;
    g_enc_count = 0;
    return d;
}

bool encoder_is_pressed(void)
{
    return g_btn_pressed;
}

uint32_t encoder_press_duration_ms(void)
{
    return g_btn_press_ms;
}
