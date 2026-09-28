#ifndef SERVO_H
#define SERVO_H

#include <stdint.h>

// 两轴舵机引脚（避开摄像头/LCD/PSRAM 已占用的 GPIO）
#define SERVO_PAN_GPIO 1  // 水平转动
#define SERVO_TILT_GPIO 2 // 俯仰转动

/* 安全行程范围（度）：限制舵机转角，防止转到绞线的位置。
 * 请按你的云台安装结构实测后调整！下面只是保守的示例值。 */
#define SERVO_PAN_MIN 10
#define SERVO_PAN_MAX 180
#define SERVO_TILT_MIN 10
#define SERVO_TILT_MAX 180

/* 上电回中角度：设成云台机械上的“正前方/水平”，上电就不会乱扭绞线 */
#define SERVO_HOME_PAN 1
#define SERVO_HOME_TILT 1

void servo_init(void);
void servo_set_pan(uint32_t angle_deg);  // 0~180°
void servo_set_tilt(uint32_t angle_deg); // 0~180°
void servo_center(void);                 // 回到 90° 中间位置
void servo_server_start(void);           // 独立 UDP 控制服务器（端口 8080）

#endif // SERVO_H
