/* MQTT 客户端（中国移动 OneNet 云平台）——从 tcp 工程移植并单独封装。
 *
 * - 使用 ESP-IDF 内置 esp-mqtt 组件，走 TCP（mqtt://，端口 1883）。
 * - 需要外网：ESP32 以 STA 模式连接路由器（见 wifi_http.c），与相机热点 AP 共存。
 * - 上电后自动连接，断线自动重连。
 * - 对外暴露 mqtt_publish() / mqtt_subscribe()，可随时在任意任务里上报数据。
 */
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "mqtt.h"

static const char *TAG = "mqtt";

/* ===== OneNet 云平台接入参数 =====
 * 产品 ID / 设备名 / 鉴权 token，从 OneNet 控制台获取。
 * 请替换成你自己的设备参数。 */
#define ONENET_BROKER_URI "mqtt://mqtts.heclouds.com:1883"
#define ONENET_PRODUCT_ID "kTZBet1Z9b" // 用户名
#define ONENET_DEVICE_NAME "ESP32"     // client_id
#define ONENET_TOKEN "version=2018-10-31&res=products%2FkTZBet1Z9b%2Fdevices%2FESP32&et=2073206025&method=md5&sign=Z25bllgKs58N%2F0pVlCbbuQ%3D%3D"

/* OneNet 属性上报 topic（物模型）。产品 ID / 设备名需与上面一致。
 * 注意：OneNet 标准 topic 没有前导斜杠、也没有 /qos0 后缀；
 *      带前导 / 或 /qos0 会被平台判定为非法 topic 并断开连接。 */
#define ONENET_TOPIC_PROPERTY_POST "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/post"

/* 上电即发布的负载（tcp 工程里的 test_data）：上报一条属性，格式按 OneNet 物模型。 */
static const char test_data[] = "{\"id\":\"123\",\"version\":\"1.0\",\"params\":{\"tem\":{\"value\":33}}}"; // 17

static esp_mqtt_client_handle_t s_client = NULL;

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        // 连上后立即上报一条属性（上电即发布，OneNet 云平台即可收到这条数据）
        mqtt_publish(ONENET_TOPIC_PROPERTY_POST, test_data);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

void mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = ONENET_BROKER_URI,
        .credentials.username = ONENET_PRODUCT_ID,
        .credentials.client_id = ONENET_DEVICE_NAME,
        .credentials.authentication.password = ONENET_TOKEN,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT client started, broker=%s", ONENET_BROKER_URI);
}

int mqtt_publish(const char *topic, const char *payload)
{
    if (s_client == NULL)
    {
        return -1;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
    ESP_LOGI(TAG, "sent publish successful, topic=%s msg_id=%d", topic, msg_id);
    return msg_id;
}

int mqtt_subscribe(const char *topic, int qos)
{
    if (s_client == NULL)
    {
        return -1;
    }
    int msg_id = esp_mqtt_client_subscribe(s_client, topic, qos);
    ESP_LOGI(TAG, "subscribe topic=%s qos=%d msg_id=%d", topic, qos, msg_id);
    return msg_id;
}
