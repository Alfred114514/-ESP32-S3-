#ifndef WIFI_HTTP_H
#define WIFI_HTTP_H

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 启动 Wi-Fi（AP+STA 共存）并开启 HTTP 视频推流服务器。
     *        AP：相机热点，供电脑/手机连入观看视频；
     *        STA：连接家用路由器，给 MQTT 云平台提供外网。
     *        内部完成 NVS、Wi-Fi、事件循环和 HTTP 服务器的初始化。
     */
    void wifi_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_HTTP_H
