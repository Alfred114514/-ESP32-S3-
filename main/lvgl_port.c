#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "lcd.h"
#include "encoder.h"
#include "lvgl_port.h"

static const char *TAG = "lvgl_port";

extern esp_lcd_panel_handle_t panel_handle;   // 定义在 cVr.c

#define DISP_H_RES 320
#define DISP_V_RES 240

static SemaphoreHandle_t lvgl_mux = NULL;
static lv_indev_t *encoder_indev = NULL;
static volatile bool preview_active = false;

/* LVGL 刷新回调：把一块区域交给 LCD 驱动（异步发送，完成后由 lcd.c 的
 * on_color_trans_done 回调触发 lv_disp_flush_ready）。 */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
}

/* 编码器输入读取回调（LVGL 周期调用） */
static void encoder_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    int32_t d = encoder_read_delta();
    if (d > 32767) d = 32767;
    if (d < -32768) d = -32768;
    data->enc_diff = (int16_t)d;
    data->state = encoder_is_pressed() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void lvgl_task(void *arg)
{
    while (1) {
        if (!preview_active) {
            lvgl_port_lock();
            lv_tick_inc(10);   // 100Hz tick 下，每次循环 = 10ms
            lv_timer_handler();
            lvgl_port_unlock();
        }
        /* 关键：vTaskDelay(1) = 1 tick（100Hz 下 10ms）。pdMS_TO_TICKS(5) 在 100Hz 下
         * 截断成 0，等于死循环饿死空闲任务触发看门狗复位。 */
        vTaskDelay(1);
    }
}

void lvgl_port_init(void)
{
    lv_init();

    /* 显示缓冲：320x240 RGB565，两段各 40 行（约 1/6 屏，DMA 内存） */
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t disp_drv;
    const uint32_t buf_px = DISP_H_RES * 40;
    lv_color_t *buf1 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1 && buf2);
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_px);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISP_H_RES;
    disp_drv.ver_res = DISP_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /* 告诉 lcd.c：每完成一次颜色传输，回调这个驱动的 lv_disp_flush_ready */
    lcd_register_flush_disp(&disp_drv);

    /* 编码器输入设备 */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_ENCODER;
    indev_drv.read_cb = encoder_read_cb;
    encoder_indev = lv_indev_drv_register(&indev_drv);

    lvgl_mux = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8 * 1024, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "LVGL 初始化完成: %dx%d", DISP_H_RES, DISP_V_RES);
}

void lvgl_port_lock(void)   { if (lvgl_mux) xSemaphoreTake(lvgl_mux, portMAX_DELAY); }
void lvgl_port_unlock(void) { if (lvgl_mux) xSemaphoreGive(lvgl_mux); }
lv_indev_t *lvgl_port_get_encoder_indev(void) { return encoder_indev; }
void lvgl_port_set_preview(bool active) { preview_active = active; }
