#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"
#include "wifi_http.h"
#include "servo.h"
#include "face_client.h"
#include "local_face_http.h"
#include "door_ctrl.h"

static const char *TAG = "wifi_http";

/* ====== ESP32 自己开热点（AP 模式），电脑/手机都直接连这个 WiFi ====== */
#define WIFI_AP_SSID "ESP32-CAM"
#define WIFI_AP_PASS "12345678" // 至少 8 位
// 0 = 自动：AP 自动跟随 STA(路由器)的信道。ESP32-S3 只有一个射频，
// 若把 AP 锁死在固定信道、又和路由器信道不同，APSTA 会互相抢信道，
// 导致 STA 连不上路由器（auth->init 循环）并连带 MQTT 断网。
#define WIFI_AP_CHANNEL 0
#define WIFI_AP_MAX_CONN 4

/* ====== STA 模式：连接手机热点，让 ESP32 和电脑处于同一局域网（摆脱切网/静态IP），与相机热点 AP 共存 ====== */
#define WIFI_STA_SSID "haqimi"
#define WIFI_STA_PASS "75800412"

/* MJPEG 推流使用的 boundary 分隔符 */
#define PART_BOUNDARY "123456789000000000000987654321"

static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static const char INDEX_HTML[] =
    "<!DOCTYPE html>\n"
    "<html><head><meta charset=\"utf-8\"><title>ESP32-S3 Camera</title></head>\n"
    "<body style=\"margin:0;background:#000;\">\n"
    "<img src=\"/stream\" style=\"width:100%;height:auto;\">\n"
    "</body></html>\n";

/* 首页：返回一个内嵌 /stream 的简单网页，兼容 Chrome 等浏览器 */
static esp_err_t index_handler(httpd_req_t *req)
{
    // 记住电脑 IP（供 TFT/编码器流程的人脸上报使用）
    char client_ip[64];
    face_client_get_ip(req, client_ip, sizeof(client_ip));

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

/* 视频流：以 multipart/x-mixed-replace 持续推送 JPEG 帧 */
static esp_err_t stream_handler(httpd_req_t *req)
{
    char client_ip[64];
    face_client_get_ip(req, client_ip, sizeof(client_ip)); // 记住电脑 IP

    camera_fb_t *fb = NULL;
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    char part_buf[64];

    httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true)
    {
        fb = esp_camera_fb_get();
        if (!fb)
        {
            // 帧缓冲暂时被 LCD 任务占用，稍等重试而不是中断推流
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 当前摄像头是 RGB565 格式，需要软件转成 JPEG
        if (fb->format == PIXFORMAT_JPEG)
        {
            jpg_buf = fb->buf;
            jpg_len = fb->len;
        }
        else if (!frame2jpg(fb, 80, &jpg_buf, &jpg_len))
        {
            ESP_LOGE(TAG, "JPEG compression failed");
            esp_camera_fb_return(fb);
            break;
        }

        int part_len = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, (unsigned)jpg_len);
        esp_err_t res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, part_buf, part_len);
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len);
        }

        if (fb->format != PIXFORMAT_JPEG)
        {
            free(jpg_buf);
        }
        esp_camera_fb_return(fb);

        if (res != ESP_OK)
        {
            // 浏览器关闭页面，连接断开
            ESP_LOGW(TAG, "Stream closed (client disconnected)");
            break;
        }

        /* 推流限帧 ~5fps：本地识别预览与 /stream 共用摄像头缓冲（仅 2 块），
         * 无限帧会互相抢、导致推流忽快忽慢；门禁判决 5fps 足够，留帧率给预览、省带宽。 */
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        ESP_LOGI(TAG, "有设备连入热点");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        ESP_LOGI(TAG, "有设备断开连接");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "STA 断开, reason=%d, 正在重连...", d->reason);
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA 已联网, IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    }
}

/* 舵机控制：GET /servo?pan=90&tilt=45 */
static esp_err_t servo_handler(httpd_req_t *req)
{
    char query[64];
    char param[16];
    uint32_t pan = 90, tilt = 90;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "pan", param, sizeof(param)) == ESP_OK)
        {
            pan = (uint32_t)atoi(param);
        }
        if (httpd_query_key_value(query, "tilt", param, sizeof(param)) == ESP_OK)
        {
            tilt = (uint32_t)atoi(param);
        }
    }

    servo_set_pan(pan);
    servo_set_tilt(tilt);

    char resp[64];
    int len = snprintf(resp, sizeof(resp), "{\"pan\":%lu,\"tilt\":%lu}",
                       (unsigned long)pan, (unsigned long)tilt);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/* 门状态查询：GET /door_status → {"door":"open"/"closed","stranger":bool,"waving":bool}
 * PC 端脚本轮询它，发现"陌生人+挥手"就弹出实时画面，由人来决定开不开门。 */
static esp_err_t door_status_handler(httpd_req_t *req)
{
    /* pc_door.py 每秒轮询本接口，并在 URL 里带 ?pc=电脑IP 上报自己的地址，
     * 供日志上报 POST 用。getpeername 在本热点下取不到对端 IP，故改用显式上报。 */
    char qbuf[128] = {0};
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK)
    {
        char pc[64] = {0};
        if (httpd_query_key_value(qbuf, "pc", pc, sizeof(pc)) == ESP_OK && pc[0] != 0)
        {
            static char s_last_pc[64] = {0};
            if (strcmp(pc, s_last_pc) != 0)
            {
                face_client_set_cached_host(pc);
                ESP_LOGI(TAG, "电脑 IP 已上报并缓存: %s（日志上报目标）", pc);
                strncpy(s_last_pc, pc, sizeof(s_last_pc) - 1);
                s_last_pc[sizeof(s_last_pc) - 1] = 0;
            }
        }
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"door\":\"%s\",\"stranger\":%s,\"waving\":%s}",
             door_ctrl_is_open() ? "open" : "closed",
             door_ctrl_alert_stranger() ? "true" : "false",
             door_ctrl_alert_waving() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* 门控制：GET /door?action=open|close（PC 端人决定后调它，TFT 同步显示开/关门） */
static esp_err_t door_handler(httpd_req_t *req)
{
    char client_ip[64];
    face_client_get_ip(req, client_ip, sizeof(client_ip)); // 记住电脑 IP，供日志上报

    char query[64], param[16];
    bool open = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "action", param, sizeof(param)) == ESP_OK)
    {
        open = (strcmp(param, "open") == 0);
    }
    door_ctrl_set_open(open);
    ESP_LOGI(TAG, "门状态 -> %s", open ? "已开门" : "已关门");

    char resp[64];
    int len = snprintf(resp, sizeof(resp), "{\"door\":\"%s\"}", open ? "open" : "closed");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/* 单帧快照：GET /snapshot → 返回一帧 JPEG（image/jpeg）。
 * 单独给电脑端做低画质「灰度」小图（160x120 @ 低画质），PC 端只看脸判定、不要清晰度；
 * 本地识别仍用摄像头原画质，互不影响。省带宽、断线自动恢复、不会卡死。
 * 注意：esp_jpeg 解码(RGB565, swap=0) 是小端输出，而 fmt2jpg 期望大端 RGB565，
 * 直接喂会红蓝颠倒（颜色花）；这里转灰度时顺手换成大端，一次循环解决。 */
static esp_err_t snapshot_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
    {
        ESP_LOGW(TAG, "快照抓帧失败（缓冲忙）");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const int W = fb->width / 2, H = fb->height / 2;
    uint8_t *rgb = (uint8_t *)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    uint8_t *out = NULL;
    size_t out_len = 0;
    if (rgb && jpg2rgb565(fb->buf, fb->len, rgb, JPEG_IMAGE_SCALE_1_2))
    {
        // 转灰度 + 小端→大端（fmt2jpg 的 RGB565 按大端读）
        uint16_t *px = (uint16_t *)rgb;
        for (int i = 0, n = W * H; i < n; i++)
        {
            uint16_t c = px[i];
            int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            int r8 = (r << 3) | (r >> 2), g8 = (g << 2) | (g >> 4), b8 = (b << 3) | (b >> 2);
            int y = (299 * r8 + 587 * g8 + 114 * b8) / 1000;
            uint16_t gv = (uint16_t)(((y >> 3) << 11) | ((y >> 2) << 5) | (y >> 3));
            px[i] = (uint16_t)((gv >> 8) | (gv << 8));
        }
        fmt2jpg(rgb, W * H * 2, W, H, PIXFORMAT_RGB565, 40, &out, &out_len);
    }
    esp_camera_fb_return(fb);
    if (rgb) free(rgb);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res;
    if (out && out_len)
    {
        res = httpd_resp_send(req, (const char *)out, out_len);
    }
    else
    {
        ESP_LOGW(TAG, "快照转码失败");
        res = httpd_resp_send_500(req);
    }
    if (out) free(out);
    return res;
}

/* 全彩高清单帧：GET /snapshot_full → 返回摄像头原始 JPEG（全分辨率全彩，不重编码）。
 * 给电脑端联动 gesture_track.py 用：舵机追踪 + 手势识别需要清晰度和颜色，
 * 不能用 /snapshot 的 160x120 灰度小图。单帧拉取比 /stream 连续推流更抗手机热点断线。 */
static esp_err_t snapshot_full_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
    {
        ESP_LOGW(TAG, "高清快照抓帧失败（缓冲忙）");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20; // 新增 /snapshot 等接口后仍留余量
    config.stack_size = 16384; // 推流 + 本地人脸推理（HTTP 录入）需要较大栈空间
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t uri_index = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &uri_index);

        httpd_uri_t uri_stream = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = stream_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &uri_stream);

        httpd_uri_t uri_servo = {
            .uri = "/servo",
            .method = HTTP_GET,
            .handler = servo_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &uri_servo);

        httpd_uri_t uri_door_status = {
            .uri = "/door_status",
            .method = HTTP_GET,
            .handler = door_status_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &uri_door_status);

        httpd_uri_t uri_door = {
            .uri = "/door",
            .method = HTTP_GET,
            .handler = door_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &uri_door);

        httpd_uri_t uri_snapshot = {
            .uri = "/snapshot",
            .method = HTTP_GET,
            .handler = snapshot_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &uri_snapshot);

        httpd_uri_t uri_snapshot_full = {
            .uri = "/snapshot_full",
            .method = HTTP_GET,
            .handler = snapshot_full_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &uri_snapshot_full);

        // 人脸识别调试接口：GET /recognize、GET /enroll?name=xxx
        face_http_register(server);

        // 本地人脸识别接口：POST /local_enroll、GET/DELETE /local_faces
        local_face_http_register(server);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
    return server;
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    // 纯 STA：连接手机热点，让 ESP32 和电脑处于同一局域网（增删查/门禁判决都走这个）。
    // 之前试过 APSTA（同时开自身热点），但 ESP32-S3 单射频共存会让 STA 刚连上就断开
    // （reason=5 ASSOC_TOOMANY 反复循环、拿不到 IP），关省电也没用，纯 STA 最稳。
    wifi_config_t sta_config = {
        .sta = {
            .ssid = WIFI_STA_SSID,
            .password = WIFI_STA_PASS,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 禁用 modem 睡眠，避免省电导致断连 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "正在连接手机热点: SSID=%s", WIFI_STA_SSID);
}

/* SNTP 授时：连上手机热点（有公网）后后台异步同步北京时间（UTC+8），
 * 供日志上报生成带时间戳的记录。wait_for_sync=false 不阻塞启动。 */
static void sntp_init_time(void)
{
    setenv("TZ", "CST-8", 1);   // 中国标准时间 = UTC+8
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("cn.pool.ntp.org");
    config.wait_for_sync = false; /* 后台同步，别拖慢启动（几秒后时间自动校准） */
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SNTP 授时已启动（cn.pool.ntp.org，UTC+8）");
    } else {
        ESP_LOGW(TAG, "SNTP 初始化失败: %s", esp_err_to_name(err));
    }
}

void wifi_http_start(void)
{
    // 初始化 NVS（Wi-Fi 需要）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();

    // 启动 SNTP 授时（联网后后台同步，日志上报要用到实时时间）
    sntp_init_time();

    ESP_LOGI(TAG, "Starting HTTP server...");
    start_webserver();
}
