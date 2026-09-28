#ifndef FACE_CLIENT_H
#define FACE_CLIENT_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 电脑端（人脸识别服务 face_server.py）端口，默认 5000。 */
#define FACE_PC_PORT 5000

/* 电脑端 IP 的兜底值。正常情况下不用管它：ESP32 会从触发请求里自动拿到电脑的 IP，
 * 直接回 POST 给那台电脑。只有自动探测失败时才会退回这个写死的地址。 */
#define FACE_PC_IP   "192.168.4.2"

typedef struct {
    bool detected;    // 电脑端是否检测到人脸
    bool matched;     // 是否已录入（detected == true 时有效）
    char name[32];    // 识别出的姓名（matched == true 时有效）
    float similarity; // 余弦相似度 0~1，越大越像
    char msg[64];     // 电脑端返回的提示信息（如"未检测到人脸"/"缺少 image"）
    int box[4];       // 人脸框 (x, y, w, h)，摄像头输出坐标系（QVGA 320x240）；has_box 时有效
    bool has_box;
} face_result_t;

/* 多人识别：电脑端 /recognize_multi 一次返回画面里所有人脸的结果 */
#define FACE_MULTI_MAX 8

typedef struct {
    int box[4];       // 人脸框 (x, y, w, h)，摄像头输出坐标系（QVGA 320x240）
    bool matched;     // 是否已录入
    char name[32];    // 姓名（matched == true 时有效）
    float similarity; // 余弦相似度 0~1
} face_multi_item_t;

typedef struct {
    int count;
    face_multi_item_t items[FACE_MULTI_MAX];
} face_multi_result_t;

/* 把一帧 JPEG POST 到 /recognize_multi，返回所有人脸（短超时，PC 不可达快速失败）。 */
esp_err_t face_client_recognize_multi_frame(const char *host, const uint8_t *jpeg,
                                            size_t jpeg_len, face_multi_result_t *out);

/* 拉取已录入姓名列表（GET /faces）。返回 ESP_OK 且 *count 有效（可为 0=空库）。 */
esp_err_t face_client_list_faces(const char *host, char names[][32], int max, int *count);

/* 拉取某人的头像 JPEG（GET /avatar?name=xxx）。返回 ESP_OK 且 *len_out>0 表示有头像。
 * 头像由电脑端在录入时裁剪成 48x48 JPEG，通常 1~2KB，buf 给 4KB 足够。 */
esp_err_t face_client_get_avatar(const char *host, const char *name,
                                 uint8_t *buf, size_t max, size_t *len_out);

/* 删除某个人脸（DELETE /faces?name=xxx）。返回 ESP_OK 表示删除成功。 */
esp_err_t face_client_delete_face(const char *host, const char *name);

/**
 * @brief 把已采集好的 JPEG 帧 POST 到电脑端 /recognize 并解析结果。
 *        供实时预览画绿/红框用（不内部抓帧）。超时较短，PC 不可达会快速失败。
 * @param host 电脑端 IP。@return ESP_OK 表示 HTTP + 解析都成功（此时 out 有效）。
 */
esp_err_t face_client_recognize_frame(const char *host, const uint8_t *jpeg,
                                      size_t jpeg_len, face_result_t *out);

/**
 * @brief 采集一帧 JPEG 并 POST 到电脑端 /recognize，解析结果。
 * @param host 电脑端 IP，如 "192.168.4.2"（可用 FACE_PC_IP 作兜底）。
 * @return ESP_OK 表示采集 + HTTP + 解析都成功（此时 out 有效）。
 */
esp_err_t face_client_recognize(const char *host, face_result_t *out);

/**
 * @brief 采集一帧 JPEG 并以指定姓名 POST 到电脑端 /enroll。
 * @return ESP_OK 表示录入成功。
 */
esp_err_t face_client_enroll(const char *host, const char *name);

/**
 * @brief 在现有 HTTP 服务器上注册两个调试接口：
 *        GET /recognize          触发一次识别，返回 JSON 结果
 *        GET /enroll?name=张三   触发一次录入
 *        两个接口都会自动探测发起请求的电脑 IP 作为上报目标。
 */
void face_http_register(httpd_handle_t server);

/* ---- 供 TFT/编码器流程使用（该流程没有"发起请求的客户端"，靠缓存的电脑 IP 上报） ---- */

/* 从 HTTP 请求里取出发起请求的客户端 IP（并缓存，供 face_client_get_cached_host 用）。 */
esp_err_t face_client_get_ip(httpd_req_t *req, char *ip, size_t ip_len);

/* 拿到电脑端 IP：默认 FACE_PC_IP，可被 face_client_get_ip 自动学习 / face_client_set_cached_host 显式覆盖。 */
const char *face_client_get_cached_host(void);

/* 显式设置电脑端 IP（pc_door.py 通过 ?pc= 上报，覆盖默认 FACE_PC_IP）。 */
void face_client_set_cached_host(const char *host);

/* 最近一次电脑端响应的 msg 字段（如"未检测到人脸"/"录入成功: 张三"），无则返回 ""。 */
const char *face_client_last_msg(void);

#ifdef __cplusplus
}
#endif

#endif // FACE_CLIENT_H
