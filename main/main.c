#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <led/led.h>
#include <camera/camera.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include "esp_psram.h"
#include "esp_log.h"
#include "esp_lcd_ili9341.h"
#include <lcd.h>
#include "wifi_http.h"
#include "log_report.h"
#include "servo.h"
#include "mqtt.h"
#include "encoder.h"
#include "lvgl_port.h"
#include "face_ui.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "esp_err.h"
#include "esp_timer.h"
static SemaphoreHandle_t lvgl_mux = NULL;

static const char *TAG = "example";
extern esp_lcd_panel_handle_t panel_handle;

void led_blink_task(void *param)
{

    while (1)
    {
        led_on();
        vTaskDelay(pdMS_TO_TICKS(500));
        led_off();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

#define BUFFER_COUNT 2
static uint16_t *fb[BUFFER_COUNT] = {NULL};
static size_t current_buffer = 0;

void init_double_buffering()
{
    // 分配帧缓冲区（使用 PSRAM）
    for (int i = 0; i < BUFFER_COUNT; i++)
    {
        fb[i] = heap_caps_malloc(320 * 240 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!fb[i])
        {
            ESP_LOGE("LCD", "Failed to allocate buffer %d", i);
            return;
        }
        memset(fb[i], 0, 320 * 240 * sizeof(uint16_t));
    }
}

uint16_t *get_current_fb()
{
    return fb[current_buffer];
}

void swap_buffers()
{
    // 发送当前缓冲区到屏幕
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
        panel_handle,
        0, 0,
        320, 240,
        fb[current_buffer]));

    // 切换到下一个缓冲区
    current_buffer = (current_buffer + 1) % BUFFER_COUNT;
}

void lcd_task(void *param)
{
    while (1)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb)
        {
            // 如果分辨率和LCD一致
            if (fb->width == LCD_WIDTH && fb->height == LCD_HEIGHT && fb->format == PIXFORMAT_RGB565)
            {

                // 直接送LCD
                esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_WIDTH, LCD_HEIGHT, fb->buf);
            }
            else
            {
                ESP_LOGE("LCD", "Frame size/format mismatch: %dx%d fmt:%d", fb->width, fb->height, fb->format);
            }

            // 释放摄像头缓冲
            esp_camera_fb_return(fb);
        }
        else
        {
            ESP_LOGE("LCD", "Failed to get camera frame");
        }

        // 根据需要调整刷新率
        vTaskDelay(pdMS_TO_TICKS(33)); // ~100fps上限
    }
}

void app_main(void)
{
    esp_err_t ret;
    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions

    led_init();
    // 检测 PSRAM 存在性和大小
    if (!esp_psram_is_initialized())
    {
        ESP_LOGE("BOOT", "PSRAM NOT DETECTED! Check hardware connection or menuconfig.");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart(); // 自动重启
    }
    size_t psram_size = esp_psram_get_size();
    ESP_LOGI("BOOT", "PSRAM Size: %d KB", psram_size / 1024);

    // // 初始化 PSRAM 缓存
    // esp_psram_init();

    // 启用 PSRAM 分配
    // heap_caps_malloc_extmem_enable(64); // 最小分配单元64字节
    bsp_camera_init();

    lcd_init();

    // 初始化两轴舵机（水平 pan / 俯仰 tilt）
    servo_init();

    // 旋转编码器（TFT 菜单的人机输入）
    encoder_init();

    // 启动 Wi-Fi（AP 模式）并开启 HTTP 视频推流服务器
    wifi_http_start();

    // 日志上报（队列 + 后台发送任务），门禁事件 POST 到电脑端 /log
    log_report_init();

    // 启动独立 UDP 伺服控制服务器（端口 8080，避免被 /stream 阻塞）
    servo_server_start();

    // 启动 MQTT 客户端（连接 OneNet 云平台，自动重连）
    // 暂不启用：上电时 STA(路由器)尚未联网，DNS 解析 mqtts.heclouds.com 失败(202)，
    // 会刷一批 esp-tls 报错。等真正要云上报时，取消下面这行注释即可。
    // mqtt_start();

    // LVGL 界面 + 人脸菜单（TFT 屏 + 编码器操作）
    lvgl_port_init();
    face_ui_start();

    // 本地屏幕直显暂不启用（已被 LVGL 界面取代）
    // xTaskCreatePinnedToCore(lcd_task, "lcd", 4096, NULL, 3, NULL, 1);

    xTaskCreatePinnedToCore(led_blink_task, "led", 4096, NULL, 3, NULL, 1);
    //  xTaskCreatePinnedToCore(camera_run_task,"capture",4096,NULL,3,NULL,1);
}
//     // ESP_LOGI(TAG, "Initialize LVGL library");
//     // lv_init();
//     // // alloc draw buffers used by LVGL
//     // // it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
//     // lv_color_t *buf1 = heap_caps_malloc(EXAMPLE_LCD_H_RES * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
//     // assert(buf1);
//     // lv_color_t *buf2 = heap_caps_malloc(EXAMPLE_LCD_H_RES * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
//     // assert(buf2);
//     // // initialize LVGL draw buffers
//     // lv_disp_draw_buf_init(&disp_buf, buf1, buf2, EXAMPLE_LCD_H_RES * 20);

//     // ESP_LOGI(TAG, "Register display driver to LVGL");
//     // lv_disp_drv_init(&disp_drv);
//     // disp_drv.hor_res = EXAMPLE_LCD_H_RES;
//     // disp_drv.ver_res = EXAMPLE_LCD_V_RES;
//     // disp_drv.flush_cb = example_lvgl_flush_cb;
//     // disp_drv.drv_update_cb = example_lvgl_port_update_callback;
//     // disp_drv.draw_buf = &disp_buf;
//     // disp_drv.user_data = panel_handle;
//     // lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

//     // ESP_LOGI(TAG, "Install LVGL tick timer");
//     // // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
//     // const esp_timer_create_args_t lvgl_tick_timer_args = {
//     //     .callback = &example_increase_lvgl_tick,
//     //     .name = "lvgl_tick"
//     // };
//     // esp_timer_handle_t lvgl_tick_timer = NULL;
//     // ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
//     // ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

//     // lvgl_mux = xSemaphoreCreateRecursiveMutex();
//     // assert(lvgl_mux);
//     // ESP_LOGI(TAG, "Create LVGL task");
//     // xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);

//     // ESP_LOGI(TAG, "Display LVGL Meter Widget");
//     // // Lock the mutex due to the LVGL APIs are not thread-safe
//     // if (example_lvgl_lock(-1)) {
//     //     example_lvgl_demo_ui(disp);
//     //     // Release the mutex
//     //     example_lvgl_unlock();
//     // }
