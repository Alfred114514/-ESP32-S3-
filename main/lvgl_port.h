#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>
#include "lvgl.h"

/* 初始化 LVGL（显示驱动 + 刷新回调 + tick + 主循环任务 + 编码器输入设备）。
 * 需在 lcd_init() 和 encoder_init() 之后调用。 */
void lvgl_port_init(void);

/* 跨线程调用 LVGL API 前加锁/解锁（外部任务用，LVGL 主循环任务内部已自行加锁）。 */
void lvgl_port_lock(void);
void lvgl_port_unlock(void);

/* 返回编码器输入设备（人脸菜单用它建 group）。 */
lv_indev_t *lvgl_port_get_encoder_indev(void);

/* true 时 LVGL 暂停刷新，把屏幕让给摄像头实时预览（拍照倒计时流程用）。 */
void lvgl_port_set_preview(bool active);

#endif // LVGL_PORT_H
