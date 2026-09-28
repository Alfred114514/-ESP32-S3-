#ifndef MQTT_H
#define MQTT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 MQTT 客户端（连接中国移动 OneNet 云平台）。
 *
 * 内部使用 ESP-IDF 内置 esp-mqtt 组件，走 TCP（mqtt://，端口 1883）。
 * 需要外网：ESP32 以 STA 模式连接路由器（见 wifi_http.c），与相机热点 AP 共存。
 * 客户端自带断线重连，STA 尚未联网时会自动持续重试，无需手动干预。
 */
void mqtt_start(void);

/**
 * @brief 向指定 topic 发布一条消息（字符串负载，QoS 0，不保留）。
 * @param topic   目标 topic，如 OneNet 属性上报 topic。
 * @param payload JSON 字符串负载。
 * @return 成功返回 msg_id（>0），客户端未启动时返回 -1。
 */
int mqtt_publish(const char *topic, const char *payload);

/**
 * @brief 订阅指定 topic。
 * @return 成功返回 msg_id（>0），客户端未启动时返回 -1。
 */
int mqtt_subscribe(const char *topic, int qos);

#ifdef __cplusplus
}
#endif

#endif // MQTT_H
