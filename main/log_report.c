#include <string.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "log_report.h"
#include "face_client.h"   // face_client_get_cached_host() + FACE_PC_PORT

static const char *TAG = "log_report";

/* 一条日志：事件名 + 相关人名（名字只可能是数字/中文，无引号转义风险）。 */
typedef struct {
    char event[16];
    char name[32];
} log_entry_t;

#define LOG_QUEUE_LEN 8
static QueueHandle_t s_log_queue = NULL;

/* 取当前北京时间 HH:MM:SS；SNTP 尚未同步（年份 < 2024）时返回 "--:--:--"。 */
static void get_time_str(char *buf, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2024) {
        strncpy(buf, "--:--:--", cap - 1);
        buf[cap - 1] = 0;
    } else {
        strftime(buf, cap, "%H:%M:%S", &tm);
    }
}

/* 后台发送任务：从队列取日志，格式化成 JSON POST 到电脑端 /log。
 * 电脑端 face_server.py 收到后打印到命令行并追加进 door.log。 */
static void log_send_task(void *arg)
{
    log_entry_t e;
    while (xQueueReceive(s_log_queue, &e, portMAX_DELAY) == pdTRUE) {
        char tbuf[16];
        get_time_str(tbuf, sizeof(tbuf));

        char body[128];
        snprintf(body, sizeof(body),
                 "{\"name\":\"%s\",\"event\":\"%s\",\"time\":\"%s\"}",
                 e.name, e.event, tbuf);

        const char *host = face_client_get_cached_host();
        char url[96];
        snprintf(url, sizeof(url), "http://%s:%d/log", host, FACE_PC_PORT);

        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 1500,   /* 短超时快速失败：PC 不可达时丢日志，不拖后台任务 */
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGW(TAG, "日志客户端创建失败");
            continue;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "日志上报失败 %s (err=%s http=%d)", url, esp_err_to_name(err), status);
        } else {
            ESP_LOGI(TAG, "日志已上报: %s", body);
        }
    }
    vTaskDelete(NULL);
}

esp_err_t log_report_init(void)
{
    if (s_log_queue) {
        return ESP_OK;
    }
    s_log_queue = xQueueCreate(LOG_QUEUE_LEN, sizeof(log_entry_t));
    if (!s_log_queue) {
        return ESP_FAIL;
    }
    BaseType_t r = xTaskCreatePinnedToCore(log_send_task, "log_report", 5120, NULL, 4, NULL, 0);
    if (r != pdPASS) {
        vQueueDelete(s_log_queue);
        s_log_queue = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "日志上报已就绪（PC 端需运行 face_server.py 接收 /log）");
    return ESP_OK;
}

esp_err_t log_report_send(const char *event, const char *name)
{
    if (!s_log_queue || !event) {
        return ESP_FAIL;
    }
    log_entry_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.event, event, sizeof(e.event) - 1);
    if (name) {
        strncpy(e.name, name, sizeof(e.name) - 1);
    }
    /* 队列满时丢弃（不阻塞调用方），保证门禁/预览循环零卡顿。 */
    if (xQueueSend(s_log_queue, &e, 0) != pdTRUE) {
        ESP_LOGW(TAG, "日志队列满，丢弃 event=%s", event);
        return ESP_FAIL;
    }
    return ESP_OK;
}
