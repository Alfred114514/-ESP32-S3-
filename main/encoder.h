#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>
#include <stdbool.h>

/* 旋转编码器引脚。这里选 GPIO 40/41/42（JTAG 复用脚），在 N16R8 板上确认空闲：
 * 摄像头占 4~18、屏幕占 0/3/14/21/45/46/47、八线 PSRAM 占 26/33~37、舵机 1/2、
 * LED 48、UART0 43/44。如果你的接线不同，改这三个宏即可。 */
#define ENCODER_CLK_GPIO 40
#define ENCODER_DT_GPIO  41
#define ENCODER_SW_GPIO  42

void encoder_init(void);

/* 自上次调用以来累计的旋转步数（正/负代表方向），读后清零。 */
int32_t encoder_read_delta(void);

/* 按键当前是否按下（已消抖）。按下=低电平。 */
bool encoder_is_pressed(void);

/* 按键连续按下的时长（毫秒，约 10ms 精度）。未按下返回 0。用于区分短按/长按。 */
uint32_t encoder_press_duration_ms(void);

#endif // ENCODER_H
