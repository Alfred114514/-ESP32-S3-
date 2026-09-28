#ifndef __LCD_H__
#define __LCD_H__

// Using SPI2 in the example
#define LCD_HOST SPI2_HOST

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL 1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
#define EXAMPLE_PIN_NUM_SCLK 3
#define EXAMPLE_PIN_NUM_MOSI 45
#define EXAMPLE_PIN_NUM_MISO 46
#define EXAMPLE_PIN_NUM_LCD_DC 47
#define EXAMPLE_PIN_NUM_LCD_RST 21
#define EXAMPLE_PIN_NUM_LCD_CS 14
#define EXAMPLE_PIN_NUM_BK_LIGHT 0
#define EXAMPLE_PIN_NUM_TOUCH_CS -256P

// The pixel number in horizontal and vertical

#define EXAMPLE_LCD_H_RES 240
#define EXAMPLE_LCD_V_RES 320
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
// #elif CONFIG_EXAMPLE_LCD_CONTROLLER_GC9A01
// #define EXAMPLE_LCD_H_RES              240
// #define EXAMPLE_LCD_V_RES              240
// #endif
// Bit number used to represent command and parameter
#define EXAMPLE_LCD_CMD_BITS 8
#define EXAMPLE_LCD_PARAM_BITS 8

#define EXAMPLE_LVGL_DRAW_BUF_LINES 20 // number of display lines in each draw buffer
#define EXAMPLE_LVGL_TICK_PERIOD_MS 2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ
#define EXAMPLE_LVGL_TASK_STACK_SIZE (4 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY 2

void lcd_init();

/* 供 LVGL 使用：注册显示驱动，LCD 每完成一次颜色传输就回调它的 lv_disp_flush_ready。 */
struct _lv_disp_drv_t;
void lcd_register_flush_disp(struct _lv_disp_drv_t *d);

#endif
