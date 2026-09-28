#ifndef LOG_REPORT_H
#define LOG_REPORT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化日志上报：创建日志队列 + 后台发送任务。
 *        在 wifi_http_start() 之后调用一次即可（发送任务只在有日志时才发 HTTP，
 *        不需要网络已就绪；但 SNTP 授时也随 wifi_http_start 启动，故放其后）。
 */
esp_err_t log_report_init(void);

/**
 * @brief 异步上报一条门禁日志（非阻塞，队列满则丢弃，绝不影响预览/推理循环）。
 * @param event 事件名："open"(开门) / "close"(关门) / "dwell"(陌生人逗留)。
 * @param name  相关人名（open 时填识别出的姓名；close/dwell 填 NULL 或 ""）。
 */
esp_err_t log_report_send(const char *event, const char *name);

#ifdef __cplusplus
}
#endif

#endif // LOG_REPORT_H
