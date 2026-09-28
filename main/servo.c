#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "servo.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "servo";

// SG90 典型参数：50Hz PWM，0.5ms ~ 2.5ms 对应 0° ~ 180°
#define SERVO_FREQ_HZ 50
#define SERVO_PERIOD_US (1000000 / SERVO_FREQ_HZ) // 20000us
#define SERVO_MIN_US 500
#define SERVO_MAX_US 2500
#define SERVO_DUTY_RES LEDC_TIMER_14_BIT
#define SERVO_DUTY_MAX ((1 << 14) - 1) // 16383

// 避开摄像头占用的 LEDC_TIMER_1 / LEDC_CHANNEL_1
#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_PAN_CHANNEL LEDC_CHANNEL_0
#define SERVO_TILT_CHANNEL LEDC_CHANNEL_2

static uint32_t angle_to_duty(uint32_t angle)
{
    if (angle > 180)
    {
        angle = 180;
    }
    uint32_t pulse_us = SERVO_MIN_US + (angle * (SERVO_MAX_US - SERVO_MIN_US)) / 180;
    return pulse_us * SERVO_DUTY_MAX / SERVO_PERIOD_US;
}

static void servo_channel_init(ledc_channel_t channel, int gpio)
{
    ledc_channel_config_t ch = {
        .gpio_num = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = SERVO_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void servo_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_DUTY_RES,
        .timer_num = SERVO_TIMER,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    servo_channel_init(SERVO_PAN_CHANNEL, SERVO_PAN_GPIO);
    servo_channel_init(SERVO_TILT_CHANNEL, SERVO_TILT_GPIO);

    // servo_center();
    ESP_LOGI(TAG, "Servo init done (pan=GPIO%d, tilt=GPIO%d)", SERVO_PAN_GPIO, SERVO_TILT_GPIO);
}

static uint32_t clamp_angle(uint32_t angle, uint32_t lo, uint32_t hi)
{
    if (angle < lo)
    {
        return lo;
    }
    if (angle > hi)
    {
        return hi;
    }
    return angle;
}

void servo_set_pan(uint32_t angle_deg)
{
    angle_deg = clamp_angle(angle_deg, SERVO_PAN_MIN, SERVO_PAN_MAX);
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_PAN_CHANNEL, angle_to_duty(angle_deg)));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_PAN_CHANNEL));
}

void servo_set_tilt(uint32_t angle_deg)
{
    angle_deg = clamp_angle(angle_deg, SERVO_TILT_MIN, SERVO_TILT_MAX);
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_TILT_CHANNEL, angle_to_duty(angle_deg)));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_TILT_CHANNEL));
}

void servo_center(void)
{
    servo_set_pan(SERVO_HOME_PAN);
    servo_set_tilt(SERVO_HOME_TILT);
}

/* ===== 独立 UDP 伺服控制服务器 =====
 * 为什么不用 HTTP：esp_http_server 是单任务 select 模型，/stream 的
 * 无限循环会阻塞整个 HTTP 服务器，导致 /servo 请求永远得不到处理。
 * 所以舵机命令走独立的 UDP socket + 独立任务，完全不受视频流影响。
 */
#define SERVO_UDP_PORT 8080

static void servo_udp_server_task(void *arg)
{
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char rx_buf[32];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "servo UDP socket create failed");
        vTaskDelete(NULL);
        return;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(SERVO_UDP_PORT);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        ESP_LOGE(TAG, "servo UDP bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Servo UDP server listening on port %d", SERVO_UDP_PORT);

    while (1)
    {
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0,
                           (struct sockaddr *)&client_addr, &client_addr_len);
        if (len > 0)
        {
            rx_buf[len] = '\0';
            unsigned int pan = 90, tilt = 90;
            if (sscanf(rx_buf, "%u %u", &pan, &tilt) == 2)
            {
                servo_set_pan(pan);
                servo_set_tilt(tilt);
            }
            else
            {
                ESP_LOGW(TAG, "servo UDP bad packet: %s", rx_buf);
            }
        }
    }
}

void servo_server_start(void)
{
    xTaskCreate(servo_udp_server_task, "servo_udp", 4096, NULL, 5, NULL);
}
