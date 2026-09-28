#ifndef DOOR_CTRL_H
#define DOOR_CTRL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 门状态 + 陌生人挥手告警，作为 UI（TFT 显示）与 HTTP（PC 控制/查询）之间的共享状态。
 * 纯内存标志位，掉电即复位（"开门"只是模拟，不落盘）。 */

/* 设置/读取门是否打开（PC 端 /door 接口写入，TFT 显示读取）。 */
void door_ctrl_set_open(bool open);
bool door_ctrl_is_open(void);

/* 本地实时预览任务每帧上报：当前是否陌生人(未录入)、是否在大幅挥手。
 * PC 端轮询 /door_status 读这两个标志，命中"陌生人+挥手"时弹出实时画面。 */
void door_ctrl_set_alert(bool stranger, bool waving);
bool door_ctrl_alert_stranger(void);
bool door_ctrl_alert_waving(void);

#ifdef __cplusplus
}
#endif

#endif // DOOR_CTRL_H
