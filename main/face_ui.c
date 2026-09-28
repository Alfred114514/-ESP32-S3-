#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"   // 低功耗休眠 + 唤醒
#include "esp_wifi.h"    // 睡前停热点、醒后重启（AP 保活会挡浅睡）
#include "driver/gpio.h" // gpio_wakeup_enable（按键唤醒）
#include "esp_camera.h"  // 顺带引入 img_converters.h（jpg2rgb565）
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "lcd.h" // LCD_WIDTH / LCD_HEIGHT
#include "face_client.h"
#include "local_face_recog.h"
#include "door_ctrl.h"
#include "log_report.h"
#include "lvgl_port.h"
#include "encoder.h" // encoder_is_pressed（预览时手动拍照）
#include "servo.h"   // servo_center（联动模式回中）

static const char *TAG = "face_ui";

extern esp_lcd_panel_handle_t panel_handle; // 定义在 lcd.c

/* 自定义汉字库（simhei 黑体，仅含界面用到的汉字），替代内置的 simsun_16_cjk */
LV_FONT_DECLARE(lv_font_face_cjk_16);

/* 拍照前倒计时秒数（实时预览期间可调整姿势，到 0 自动拍） */
#define FACE_COUNTDOWN_SEC 10

/* 缩略图小窗：QVGA(320x240) 的 1/2 = 160x120，贴在屏幕左上角，尽量省解码/刷屏资源 */
#define THUMB_W 160
#define THUMB_H 120
#define THUMB_OFF_X 8
#define THUMB_OFF_Y 8

/* RGB565 常用色 */
#define COLOR_GREEN 0x07E0
#define COLOR_RED 0xF800
#define COLOR_GRAY 0x7BEF

/* 预设姓名表：录入时用编码器上下滚选 */
static const char *PRESET_NAMES[] = {"张三", "李四", "王五", "赵六", "陈七"};
#define PRESET_NAME_COUNT (sizeof(PRESET_NAMES) / sizeof(PRESET_NAMES[0]))

static lv_indev_t *s_indev = NULL;
static lv_group_t *s_group = NULL;
static lv_obj_t *s_scr = NULL;
static lv_timer_t *s_return_timer = NULL;

typedef enum
{
    CAP_ENROLL,
    CAP_RECOGNIZE,
    CAP_LOCAL_ENROLL,
    CAP_LOCAL_RECOGNIZE
} cap_mode_t;

typedef struct
{
    cap_mode_t mode;
    char name[64];
} capture_job_t;

static void show_main_menu(void);
static void show_name_select(void);
static void show_message(const char *msg);
static void show_result(const char *msg);
static void show_preview_screen(cap_mode_t mode, const char *name);
static void start_capture(cap_mode_t mode, const char *name);
static void start_library(void);
static void start_link(void);
static void show_more_menu(void);
static void start_local_library(void);
static void start_local_live(void);
static void start_local_enroll(void);
static esp_err_t capture_jpeg_local(uint8_t **jpeg_out, size_t *len_out);
static void enter_sleep(void);

/* 长按退出后、恢复 LVGL 前，等按键完全松开。否则 LVGL 恢复后会把"松手"当成一次点击，
 * 误触发当前聚焦按钮（通常是"录入人脸"），造成退出后又自动进入录入页。 */
static void wait_btn_release(void)
{
    while (encoder_is_pressed())
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    encoder_read_delta(); /* 顺手丢弃冻结期间累积的旋钮增量，避免恢复后焦点乱跳 */
    vTaskDelay(pdMS_TO_TICKS(30));
}

/* ---- 中文标签（统一用 simsun 中文字库） ---- */
static lv_obj_t *ui_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_face_cjk_16, 0);
    return l;
}

static void ui_clean(void)
{
    if (s_return_timer)
    {
        lv_timer_del(s_return_timer);
        s_return_timer = NULL;
    }
    lv_group_remove_all_objs(s_group);
    lv_obj_clean(s_scr);
}

/* ---- 倒计时数字（5x7 点阵写进 RGB565 缓冲，白字黑底，画在右上角） ---- */
static const uint8_t FONT5x7[10][7] = {
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};

static void draw_countdown(uint16_t *buf, int w, int h, int sec, int scale)
{
    const int gw = 5 * scale, gh = 7 * scale, pad = 4;
    int bx = w - gw - pad, by = pad;
    for (int y = by - pad; y < by + gh + pad; y++)
        for (int x = bx - pad; x < bx + gw + pad; x++)
            if (x >= 0 && x < w && y >= 0 && y < h)
                buf[y * w + x] = 0x0000;
    const uint8_t *g = FONT5x7[sec];
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 5; c++)
            if (g[r] & (1 << (4 - c)))
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                    {
                        int yy = by + r * scale + dy, xx = bx + c * scale + dx;
                        if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                            buf[yy * w + xx] = 0xFFFF;
                    }
}

/* 在 RGB565 缓冲里画一个描边矩形（thick 为线宽），坐标越界自动裁剪 */
static void draw_rect(uint16_t *buf, int w, int h, int x, int y, int rw, int rh,
                      uint16_t color, int thick)
{
    if (rw <= 0 || rh <= 0)
        return;
    int x2 = x + rw - 1, y2 = y + rh - 1;
    for (int t = 0; t < thick; t++)
    {
        int X1 = x + t, Y1 = y + t, X2 = x2 - t, Y2 = y2 - t;
        if (X1 > X2 || Y1 > Y2)
            break;
        for (int i = X1; i <= X2; i++)
        {
            if (i >= 0 && i < w && Y1 >= 0 && Y1 < h)
                buf[Y1 * w + i] = color;
            if (i >= 0 && i < w && Y2 >= 0 && Y2 < h)
                buf[Y2 * w + i] = color;
        }
        for (int j = Y1; j <= Y2; j++)
        {
            if (X1 >= 0 && X1 < w && j >= 0 && j < h)
                buf[j * w + X1] = color;
            if (X2 >= 0 && X2 < w && j >= 0 && j < h)
                buf[j * w + X2] = color;
        }
    }
}

/* RGB565 → 灰度 RGB565（原位，每像素只留亮度。黑白观感，与录入/识别小窗一致） */
static void gray565_buf(uint16_t *buf, int n)
{
    for (int i = 0; i < n; i++)
    {
        uint16_t c = buf[i];
        int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
        int r8 = (r << 3) | (r >> 2), g8 = (g << 2) | (g >> 4), b8 = (b << 3) | (b >> 2);
        int y = (299 * r8 + 587 * g8 + 114 * b8) / 1000; // 加权亮度 0~255
        buf[i] = (uint16_t)(((y >> 3) << 11) | ((y >> 2) << 5) | (y >> 3));
    }
}

/* 解码 JPEG → RGB565 并转灰度（去彩色）。
 * - 只留亮度，符合"只要知道脸在框里对准了"的需求；
 * - 输出原生小端 uint16，字节序统一在发送前用 bswap16_buf() 换成大端（见下）。 */
static bool jpg2gray565(const uint8_t *src, size_t src_len, uint16_t *out, int w, int h)
{
    static uint8_t work[3100]; // esp_jpeg 工作缓冲（与 jpg2rgb565 同规格）

    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)src,
        .indata_size = src_len,
        .outbuf = (uint8_t *)out,
        .outbuf_size = (uint32_t)w * h * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_1_2,
        .flags.swap_color_bytes = 0, // 输出 [低,高]；按 uint16 读正好得到真值
        .advanced.working_buffer = work,
        .advanced.working_buffer_size = sizeof(work),
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_decode(&cfg, &info) != ESP_OK)
        return false;

    gray565_buf(out, w * h);
    return true;
}

/* 把缓冲按 uint16 逐字交换高低字节。ST7789 面板被配成"大端"(MSB first)，而 ESP32 是
 * 小端，所以 RGB565 缓冲在发送前必须换成大端，否则颜色会花屏。 */
static void bswap16_buf(uint16_t *buf, int n)
{
    for (int i = 0; i < n; i++)
    {
        buf[i] = (uint16_t)((buf[i] >> 8) | (buf[i] << 8));
    }
}

/* ---- 多人实时识别：全屏解码 + 画绿/红框 + 姓名牌 ---- */

/* 解码 JPEG → 全屏灰度 RGB565（QVGA 320x240 全尺寸，1:1 铺满屏幕，黑白） */
static bool jpg2gray565_full(const uint8_t *src, size_t src_len, uint16_t *out, int w, int h)
{
    static uint8_t work[3100];
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)src,
        .indata_size = src_len,
        .outbuf = (uint8_t *)out,
        .outbuf_size = (uint32_t)w * h * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0, // 0 = 原尺寸，QVGA 320x240 直接铺满屏
        .flags.swap_color_bytes = 0,
        .advanced.working_buffer = work,
        .advanced.working_buffer_size = sizeof(work),
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_decode(&cfg, &info) != ESP_OK)
        return false;
    gray565_buf(out, w * h);
    return true;
}

/* 解码 JPEG → 1/4 尺寸灰度 RGB565（QVGA 320x240 -> 80x60），供本地实时预览。
 * 预览画质与识别无关（识别走独立的全尺寸 RGB888 管线），这里降到最低只省解码时间。 */
static bool jpg2gray565_quarter(const uint8_t *src, size_t src_len, uint16_t *out, int w, int h)
{
    static uint8_t work[3100];
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)src,
        .indata_size = src_len,
        .outbuf = (uint8_t *)out,
        .outbuf_size = (uint32_t)w * h * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_1_4,
        .flags.swap_color_bytes = 0,
        .advanced.working_buffer = work,
        .advanced.working_buffer_size = sizeof(work),
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_decode(&cfg, &info) != ESP_OK)
        return false;
    gray565_buf(out, w * h);
    return true;
}

/* 按 4bit 透明度把前景色混到背景色上（a: 0~15） */
static uint16_t rgb565_blend(uint16_t bg, uint16_t fg, int a)
{
    int r = (((bg >> 11) & 0x1F) * (15 - a) + ((fg >> 11) & 0x1F) * a) / 15;
    int g = (((bg >> 5) & 0x3F) * (15 - a) + ((fg >> 5) & 0x3F) * a) / 15;
    int b = ((bg & 0x1F) * (15 - a) + (fg & 0x1F) * a) / 15;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* 解码下一个 UTF-8 码点，返回字节数（用于遍历姓名里的中文） */
static int utf8_next(const uint8_t *s, uint32_t *cp)
{
    if (s[0] < 0x80)
    {
        *cp = s[0];
        return 1;
    }
    if ((s[0] & 0xE0) == 0xC0)
    {
        *cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((s[0] & 0xF0) == 0xE0)
    {
        *cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((s[0] & 0xF8) == 0xF0)
    {
        *cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = s[0];
    return 1;
}

/* 用 16px CJK 字库量一行 UTF-8 文本的像素宽度 */
static int text_width_px(const char *utf8)
{
    const lv_font_t *font = &lv_font_face_cjk_16;
    int w = 0;
    const uint8_t *s = (const uint8_t *)utf8;
    while (*s)
    {
        uint32_t cp = 0, next = 0;
        int n = utf8_next(s, &cp);
        const uint8_t *ns = s + n;
        if (*ns)
            utf8_next(ns, &next);
        lv_font_glyph_dsc_t d;
        if (lv_font_get_glyph_dsc(font, &d, cp, next))
            w += d.adv_w;
        s = ns;
    }
    return w;
}

/* 用 16px CJK 字库在 RGB565 缓冲里画一行文字（UTF-8，4bpp 抗锯齿）。缺字自动跳过。 */
static void draw_text_rgb565(uint16_t *buf, int w, int h, int x, int y,
                             const char *utf8, uint16_t fg)
{
    const lv_font_t *font = &lv_font_face_cjk_16;
    int cx = x;
    const uint8_t *s = (const uint8_t *)utf8;
    while (*s)
    {
        uint32_t cp = 0, next = 0;
        int n = utf8_next(s, &cp);
        const uint8_t *ns = s + n;
        if (*ns)
            utf8_next(ns, &next);
        lv_font_glyph_dsc_t d;
        if (lv_font_get_glyph_dsc(font, &d, cp, next))
        {
            const uint8_t *bmp = lv_font_get_glyph_bitmap(font, cp);
            int gx = cx + d.ofs_x, gy = y + d.ofs_y;
            int stride = (d.box_w + 1) / 2; // 4bpp：2 像素/字节
            for (int py = 0; py < d.box_h; py++)
            {
                for (int px = 0; px < d.box_w; px++)
                {
                    uint8_t v = bmp[py * stride + (px >> 1)];
                    int a = (px & 1) ? (v & 0x0F) : (v >> 4);
                    int sx = gx + px, sy = gy + py;
                    if (a && sx >= 0 && sx < w && sy >= 0 && sy < h)
                    {
                        uint16_t bg = buf[sy * w + sx];
                        buf[sy * w + sx] = (a == 15) ? fg : rgb565_blend(bg, fg, a);
                    }
                }
            }
            cx += d.adv_w;
        }
        s = ns;
    }
}

/* 在缓冲里画一个"状态牌"：黑底 + 指定颜色文字（越界自动裁到屏内）。
 * 姓名牌/门状态/CPU/PSRAM 都用它，仅文字颜色不同。 */
static void draw_badge(uint16_t *buf, int w, int h, int x, int y,
                       const char *text, uint16_t fg)
{
    int tw = text_width_px(text);
    if (tw <= 0)
        return;
    int pad = 4, hgt = 20;
    int bx = x, by = y;
    if (bx < 0)
        bx = 0;
    if (bx + tw + pad * 2 > w)
        bx = w - tw - pad * 2;
    if (bx < 0)
        bx = 0;
    if (by < 0)
        by = 0;
    if (by + hgt > h)
        by = h - hgt;
    for (int yy = by; yy < by + hgt; yy++)
        for (int xx = bx; xx < bx + tw + pad * 2; xx++)
            buf[yy * w + xx] = 0x0000; // 黑底
    draw_text_rgb565(buf, w, h, bx + pad, by + 2, text, fg);
}

/* 姓名牌 = 白字状态牌（绿框旁显示已录入者名字） */
static void draw_nameplate(uint16_t *buf, int w, int h, int x, int y, const char *name)
{
    draw_badge(buf, w, h, x, y, name, 0xFFFF);
}

/* 右对齐的状态牌（右上角 PSRAM、右下角门状态用），x 忽略、按文字宽度贴右边 */
static void draw_badge_right(uint16_t *buf, int w, int h, int y,
                             const char *text, uint16_t fg)
{
    int tw = text_width_px(text);
    if (tw <= 0)
        return;
    draw_badge(buf, w, h, w - tw - 8, y, text, fg);
}

/* 多人实时识别任务：全屏取景，绿框=已录入(带姓名)，红框=未录入，长按退出 */
static void multi_task(void *arg)
{
    const char *host = face_client_get_cached_host();
    const int FW = LCD_WIDTH, FH = LCD_HEIGHT;

    /* 多人识别用小画质 JPEG 上传（更小更快、长时间跑更稳），退出时恢复标准画质 */
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor)
        sensor->set_quality(sensor, 30);

    uint16_t *fbuf[2];
    fbuf[0] = heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
    fbuf[1] = heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
    if (!fbuf[0] || !fbuf[1])
    {
        ESP_LOGE(TAG, "全屏缓冲分配失败");
        free(fbuf[0]);
        free(fbuf[1]);
        vTaskDelete(NULL);
        return;
    }

    lvgl_port_set_preview(true);     // 冻结 LVGL，屏幕让给实时画面
    memset(fbuf[0], 0, FW * FH * 2); // 先清黑，避免菜单残影
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, FW, FH, fbuf[0]);

    int cur = 0;

    while (1)
    {
        /* 长按 1 秒退出 */
        bool bnow = encoder_is_pressed();
        if (bnow && encoder_press_duration_ms() >= 1000)
            break;

        camera_fb_t *fb = esp_camera_fb_get();
        if (fb)
        {
            if (fb->format == PIXFORMAT_JPEG &&
                jpg2gray565_full(fb->buf, fb->len, fbuf[cur], FW, FH))
            {
                face_multi_result_t mr;
                if (face_client_recognize_multi_frame(host, fb->buf, fb->len, &mr) == ESP_OK)
                {
                    for (int i = 0; i < mr.count; i++)
                    {
                        face_multi_item_t *it = &mr.items[i];
                        uint16_t col = it->matched ? COLOR_GREEN : COLOR_RED;
                        draw_rect(fbuf[cur], FW, FH, it->box[0], it->box[1],
                                  it->box[2], it->box[3], col, 2);
                        if (it->matched && it->name[0])
                        {
                            /* 姓名牌优先贴框上方，放不下就贴框下方 */
                            int ny = it->box[1] - 22;
                            if (ny < 0)
                                ny = it->box[1] + it->box[3] + 2;
                            draw_nameplate(fbuf[cur], FW, FH, it->box[0], ny, it->name);
                        }
                    }
                }
                draw_nameplate(fbuf[cur], FW, FH, 2, FH - 22, "长按退出"); // 左下角提示
                bswap16_buf(fbuf[cur], FW * FH);
                esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, FW, FH, fbuf[cur]);
                cur ^= 1;
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 多人识别降刷新率（约 10 FPS 上限），降 CPU/发热更稳
    }

    wait_btn_release();
    lvgl_port_set_preview(false);
    if (sensor)
        sensor->set_quality(sensor, 20); // 恢复标准画质
    lvgl_port_lock();
    show_main_menu();
    lvgl_port_unlock();
    free(fbuf[0]);
    free(fbuf[1]);
    vTaskDelete(NULL);
}

static void start_multi(void)
{
    xTaskCreatePinnedToCore(multi_task, "face_multi", 12 * 1024, NULL, 5, NULL, 0);
}

/* ---- 本地实时识别（双任务）：预览任务(核0)半分辨率快速刷屏，推理任务(核1)全分辨率比对。
 * 绿框=已录入(带姓名)、红框=未录入，长按退出。 ----
 * 摄像头只有预览任务一个消费者：它拿到帧后把 JPEG 复制到共享缓冲，推理任务从共享缓冲读，
 * 避免两个任务抢 esp_camera_fb_get() 导致低优先级任务饿死。 */

#define LOCAL_JPEG_CAP (128 * 1024) /* QVGA 高质量 JPEG 远小于 128KB，够用 */
#define HOLD_FRAMES 3               /* 连续"有脸但未匹配"几轮才判"未录入"（容忍转头/戴眼镜的短暂掉分） */
#define HOLD_MIN_SIM 0.30f          /* 相似度低于此值=换人了，立即判未录入，不再保持上一张脸（不同人<0.3） */
#define DOOR_OPEN_MS 3000           /* 识别命中后模拟"门开"时长，到点自动关门 */
#define STRANGER_DWELL_MS 10000     /* 陌生人(未录入)连续停留超过此时长，记一条逗留日志 */

static SemaphoreHandle_t s_live_mtx = NULL;
static volatile bool s_live_infer_run = false;
static uint8_t *s_live_jpeg = NULL; /* 预览任务发布的最新 JPEG 快照 */
static size_t s_live_jpeg_len = 0;
static volatile bool s_live_jpeg_ready = false;
static int s_live_box[4];
static int s_live_state; /* 0=无脸 1=已录入 2=未录入 */
static char s_live_name[32];
static float s_live_sim;
static TickType_t s_door_open_at = 0;   /* 开门时刻（tick），用于延时关门 */
static bool s_door_armed = true;        /* 是否允许再次触发开门（等人离开后重新武装） */
static TickType_t s_stranger_since = 0; /* 陌生人开始逗留时刻（tick），用于 >10s 判逗留 */
static bool s_stranger_logged = false;  /* 本波陌生人逗留是否已上报过日志 */
static StaticTask_t s_live_preview_tcb; /* 预览任务静态 TCB/栈（栈放 PSRAM，省内部 RAM） */
static StackType_t *s_live_preview_stack = NULL;

/* ---- CPU 占用率 / PSRAM 剩余（每 500ms 采样一次，预览每帧只画缓存值） ---- */
static volatile int s_cpu_percent = 0;
static volatile int s_psram_free_kb = 0;

static void update_sys_stats(void)
{
    enum
    {
        MAX_TASKS = 24
    };
    TaskStatus_t st[MAX_TASKS]; // 栈上分配（预览任务栈在 PSRAM，不占内部 RAM）
    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(st, MAX_TASKS, &total);
    uint64_t idle = 0;
    for (UBaseType_t i = 0; i < n; i++)
    {
        /* 空闲任务名为 "IDLE0"/"IDLE1"（双核各一个），前缀 IDLE 判定 */
        if (strncmp(st[i].pcTaskName, "IDLE", 4) == 0)
            idle += st[i].ulRunTimeCounter;
    }
    if (total > 0)
    {
        int pct = (int)(100 - (idle * 100ULL / total));
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
        s_cpu_percent = pct;
    }
    s_psram_free_kb = (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

/* ---- 帧差法挥手检测（在 80x60 半分辨率灰度图上做，开销可忽略） ---- */
#define WAVE_DIFF_THR 8    /* 相邻帧亮度差阈值（绿通道 0~63） */
#define WAVE_MOTION_PX 300 /* 4800 像素中，变化像素 > 此值 = 大幅挥手（摇头/走动幅度小到不了） */

static int detect_motion(const uint16_t *cur, const uint16_t *prev, int n)
{
    int motion = 0;
    for (int i = 0; i < n; i++)
    {
        int a = (cur[i] >> 5) & 0x3F; // 灰图的绿通道 ≈ 亮度
        int b = (prev[i] >> 5) & 0x3F;
        int d = a - b;
        if (d < 0)
            d = -d;
        if (d > WAVE_DIFF_THR)
            motion++;
    }
    return motion;
}

/* 核 1：本地识别推理。每轮都做完整识别（检测+特征+比对），一次同时更新框和身份，避免
 * "检测一轮、识别一轮"的拆分把身份刷新拖慢一倍。身份做时间平滑：单帧受转头/戴眼镜/姿态
 * 影响会瞬时掉分，连续多帧不命中才判"未录入"；相似度掉到 HOLD_MIN_SIM 以下视为换人立即清。 */
static void local_live_infer_task(void *arg)
{
    ESP_LOGW(TAG, "本地识别推理任务启动");
    uint8_t *jpeg = heap_caps_malloc(LOCAL_JPEG_CAP, MALLOC_CAP_SPIRAM);
    if (!jpeg)
    {
        ESP_LOGE(TAG, "推理缓冲分配失败");
        s_live_infer_run = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    char held_name[32] = {0}; /* 当前保持的身份（时间平滑） */
    float held_sim = 0.0f;
    int held_state = 0;  /* 0=无 1=已录入 2=未录入 */
    int fail_streak = 0; /* 连续"有脸但未匹配"的次数 */
    bool logged_first = false;

    while (s_live_infer_run)
    {
        size_t len = 0;
        xSemaphoreTake(s_live_mtx, portMAX_DELAY);
        if (s_live_jpeg_ready && s_live_jpeg && s_live_jpeg_len <= LOCAL_JPEG_CAP)
        {
            memcpy(jpeg, s_live_jpeg, s_live_jpeg_len);
            len = s_live_jpeg_len;
            s_live_jpeg_ready = false;
        }
        xSemaphoreGive(s_live_mtx);

        if (len > 0)
        {
            if (!logged_first)
            {
                ESP_LOGI(TAG, "推理收到首帧 len=%u", (unsigned)len);
                logged_first = true;
            }
            int box[4];
            char name[32] = {0};
            float sim = 0.0f;
            esp_err_t r = local_face_recognize_jpeg_box(jpeg, len, box, name, sizeof(name), &sim);

            xSemaphoreTake(s_live_mtx, portMAX_DELAY);
            memcpy(s_live_box, box, sizeof(box));
            if (r == ESP_OK)
            {
                if (held_state != 1 || strcmp(held_name, name) != 0)
                    ESP_LOGI(TAG, "识别命中: %s sim=%.2f", name, (double)sim);
                held_state = 1;
                fail_streak = 0;
                strncpy(held_name, name, sizeof(held_name) - 1);
                held_name[sizeof(held_name) - 1] = 0;
                held_sim = sim;
            }
            else if (r == ESP_ERR_NOT_FOUND)
            {
                if (held_state == 1 && sim >= HOLD_MIN_SIM)
                {
                    /* 同一个人暂时掉分（转头/戴眼镜）：保持身份，只更新框位置 */
                    fail_streak++;
                    if (fail_streak >= HOLD_FRAMES)
                    {
                        ESP_LOGW(TAG, "持续掉分，判为未录入 sim=%.2f", (double)sim);
                        held_state = 2;
                        held_name[0] = 0;
                    }
                }
                else
                {
                    /* 相似度太低=换人了：立即判未录入，不再把旧名字挂到新脸上 */
                    if (held_state != 2)
                        ESP_LOGW(TAG, "相似度过低(换人)，立即判未录入 sim=%.2f", (double)sim);
                    held_state = 2;
                    held_name[0] = 0;
                    fail_streak = 0;
                }
                held_sim = sim;
            }
            else
            {
                /* 没检测到脸：立即清身份（人离开画面） */
                held_state = 0;
                held_name[0] = 0;
                held_sim = 0.0f;
                fail_streak = 0;
            }
            s_live_state = held_state;
            strncpy(s_live_name, held_name, sizeof(s_live_name) - 1);
            s_live_name[sizeof(s_live_name) - 1] = 0;
            s_live_sim = held_sim;
            xSemaphoreGive(s_live_mtx);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(jpeg);
    vTaskDeleteWithCaps(NULL);
}

/* 核 0：半分辨率解码 + 最近邻放大刷屏（预览帧率高、画质降级），叠加共享识别结果 */
static void local_live_task(void *arg)
{
    const int FW = LCD_WIDTH, FH = LCD_HEIGHT;
    const int HW = FW / 4, HH = FH / 4; /* 80x60（SCALE_1_4 输出，预览画质最低，不影响识别） */

    /* 本地识别要更清晰的特征，画质调高（越小越清晰）；退出时恢复标准画质 */
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor)
        sensor->set_quality(sensor, 18); /* 18 兼顾识别与推流体量（12 更清晰但 JPEG 大一倍、推流卡） */

    uint16_t *half = heap_caps_malloc(HW * HH * 2, MALLOC_CAP_SPIRAM);
    uint16_t *prev = heap_caps_malloc(HW * HH * 2, MALLOC_CAP_SPIRAM); // 上一帧，帧差法用
    uint16_t *fbuf[2];
    fbuf[0] = heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
    fbuf[1] = heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
    if (!half || !prev || !fbuf[0] || !fbuf[1])
    {
        ESP_LOGE(TAG, "本地实时缓冲分配失败");
        free(half);
        free(prev);
        free(fbuf[0]);
        free(fbuf[1]);
        s_live_infer_run = false;
        vTaskDelete(NULL);
        return;
    }
    memset(prev, 0, HW * HH * 2);

    lvgl_port_set_preview(true);
    memset(fbuf[0], 0, FW * FH * 2);
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, FW, FH, fbuf[0]);

    int cur = 0;
    TickType_t last_stats = 0; /* CPU/PSRAM 采样节流：每 500ms 才重算一次 */
    while (1)
    {
        bool bnow = encoder_is_pressed();
        if (bnow && encoder_press_duration_ms() >= 1000)
            break;

        camera_fb_t *fb = esp_camera_fb_get();
        if (fb)
        {
            if (fb->format == PIXFORMAT_JPEG)
            {
                /* 发布最新 JPEG 快照给推理任务（唯一的摄像头消费者） */
                xSemaphoreTake(s_live_mtx, portMAX_DELAY);
                if (s_live_jpeg && fb->len <= LOCAL_JPEG_CAP)
                {
                    memcpy(s_live_jpeg, fb->buf, fb->len);
                    s_live_jpeg_len = fb->len;
                    s_live_jpeg_ready = true;
                }
                xSemaphoreGive(s_live_mtx);

                if (jpg2gray565_quarter(fb->buf, fb->len, half, HW, HH))
                {
                    /* 帧差法：与上一帧比，检测大幅挥手（供 PC 弹实时画面用） */
                    int motion = detect_motion(half, prev, HW * HH);
                    memcpy(prev, half, HW * HH * 2);
                    bool waving = (motion > WAVE_MOTION_PX);

                    /* 80x60 -> 320x240 最近邻 4x 放大（画质降到最低换帧率，识别不受影响） */
                    for (int y = 0; y < HH; y++)
                    {
                        for (int x = 0; x < HW; x++)
                        {
                            uint16_t c = half[y * HW + x];
                            for (int dy = 0; dy < 4; dy++)
                            {
                                uint16_t *d = &fbuf[cur][(4 * y + dy) * FW];
                                d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = d[4 * x + 3] = c;
                            }
                        }
                    }

                    /* 取共享识别结果（短临界区，不影响预览帧率） */
                    int box[4], state;
                    char name[32];
                    float sim;
                    xSemaphoreTake(s_live_mtx, portMAX_DELAY);
                    memcpy(box, s_live_box, sizeof(box));
                    state = s_live_state;
                    strncpy(name, s_live_name, sizeof(name) - 1);
                    name[sizeof(name) - 1] = 0;
                    sim = s_live_sim;
                    xSemaphoreGive(s_live_mtx);

                    /* ---- 门禁状态机：已录入命中→开门，延时→关门（模拟人进入） ---- */
                    if (state == 1 && !door_ctrl_is_open() && s_door_armed)
                    {
                        door_ctrl_set_open(true);
                        s_door_open_at = xTaskGetTickCount();
                        s_door_armed = false;          /* 本次开门后，要等人离开才重新武装 */
                        log_report_send("open", name); /* 功能2：开门日志上报 PC */
                    }
                    if (state == 0)
                    {
                        s_door_armed = true; /* 人离开，重新允许下一次开门 */
                    }
                    if (door_ctrl_is_open() &&
                        (xTaskGetTickCount() - s_door_open_at) >= pdMS_TO_TICKS(DOOR_OPEN_MS))
                    {
                        door_ctrl_set_open(false);
                        log_report_send("close", ""); /* 延时到点自动关门，记日志 */
                    }

                    /* ---- 陌生人逗留：连续"未录入"超 10s 记一条日志（每波只记一次） ---- */
                    if (state == 2)
                    {
                        if (s_stranger_since == 0)
                            s_stranger_since = xTaskGetTickCount();
                        else if (!s_stranger_logged &&
                                 (xTaskGetTickCount() - s_stranger_since) >= pdMS_TO_TICKS(STRANGER_DWELL_MS))
                        {
                            log_report_send("dwell", "");
                            s_stranger_logged = true;
                        }
                    }
                    else
                    {
                        s_stranger_since = 0;
                        s_stranger_logged = false;
                    }

                    /* 上报陌生人+挥手告警（PC 端轮询 /door_status 读取） */
                    door_ctrl_set_alert(state == 2, waving);

                    if (state == 1)
                    {
                        draw_rect(fbuf[cur], FW, FH, box[0], box[1],
                                  box[2] - box[0], box[3] - box[1], COLOR_GREEN, 2);
                        char label[48];
                        snprintf(label, sizeof(label), "%s %.2f", name, sim);
                        int ny = box[1] - 22;
                        if (ny < 0)
                            ny = box[1] + (box[3] - box[1]) + 2;
                        draw_nameplate(fbuf[cur], FW, FH, box[0], ny, label);
                    }
                    else if (state == 2)
                    {
                        draw_rect(fbuf[cur], FW, FH, box[0], box[1],
                                  box[2] - box[0], box[3] - box[1], COLOR_RED, 2);
                        int ny = box[1] - 22;
                        if (ny < 0)
                            ny = box[1] + (box[3] - box[1]) + 2;
                        draw_nameplate(fbuf[cur], FW, FH, box[0], ny, "未录入");
                    }

                    draw_nameplate(fbuf[cur], FW, FH, 2, FH - 22, "本地识别 长按退出");

                    /* 状态栏：左上 CPU 占用、右上 PSRAM 剩余、右下门开关（CPU/PSRAM 每 500ms 采样） */
                    if (xTaskGetTickCount() - last_stats >= pdMS_TO_TICKS(500))
                    {
                        update_sys_stats();
                        last_stats = xTaskGetTickCount();
                    }
                    char stat[24];
                    snprintf(stat, sizeof(stat), "CPU %d%%", s_cpu_percent);
                    draw_badge(fbuf[cur], FW, FH, 2, 2, stat, 0xFFFF);
                    snprintf(stat, sizeof(stat), "PSRAM %dK", s_psram_free_kb);
                    draw_badge_right(fbuf[cur], FW, FH, 2, stat, 0xFFFF);

                    /* 醒目门状态：顶部居中大色块（绿=门开，红=门关） */
                    const char *door_txt = door_ctrl_is_open() ? "门已开" : "门已关";
                    uint16_t door_col = door_ctrl_is_open() ? COLOR_GREEN : COLOR_RED;
                    int dw = text_width_px(door_txt) + 16, dh = 26;
                    int dx = (FW - dw) / 2, dy = 0;
                    for (int yy = dy; yy < dy + dh && yy < FH; yy++)
                        for (int xx = dx; xx < dx + dw && xx < FW; xx++)
                            fbuf[cur][yy * FW + xx] = 0x0000;
                    draw_text_rgb565(fbuf[cur], FW, FH, dx + 8, dy + 4, door_txt, door_col);
                    draw_rect(fbuf[cur], FW, FH, dx, dy, dw, dh, door_col, 2);

                    bswap16_buf(fbuf[cur], FW * FH);
                    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, FW, FH, fbuf[cur]);
                    cur ^= 1;
                }
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(30)); /* 预览目标 ~30fps（实际受半分辨率解码限制） */
    }

    s_live_infer_run = false; /* 通知推理任务退出 */
    wait_btn_release();
    lvgl_port_set_preview(false);
    if (sensor)
        sensor->set_quality(sensor, 20); /* 恢复标准画质 */
    lvgl_port_lock();
    show_main_menu();
    lvgl_port_unlock();
    free(half);
    free(prev);
    free(fbuf[0]);
    free(fbuf[1]);
    door_ctrl_set_alert(false, false); /* 退出本地识别，清掉陌生人/挥手告警 */
    vTaskDelete(NULL);
}

static void start_local_live(void)
{
    if (!s_live_mtx)
        s_live_mtx = xSemaphoreCreateMutex();
    if (!s_live_jpeg)
        s_live_jpeg = heap_caps_malloc(LOCAL_JPEG_CAP, MALLOC_CAP_SPIRAM);
    /* 预览任务栈放 PSRAM（预览不碰 flash/NVS，安全），把紧俏的内部 RAM 留给推理任务 */
    if (!s_live_preview_stack)
        s_live_preview_stack = (StackType_t *)heap_caps_aligned_alloc(16, 12 * 1024, MALLOC_CAP_SPIRAM);

    s_live_state = 0;
    s_live_name[0] = 0;
    s_live_sim = 0.0f;
    s_live_jpeg_ready = false;
    s_live_infer_run = true;

    uint32_t free_ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    BaseType_t rp, ri;
    if (s_live_preview_stack)
    {
        TaskHandle_t h = xTaskCreateStaticPinnedToCore(local_live_task, "local_live", 12 * 1024,
                                                       NULL, 5, s_live_preview_stack,
                                                       &s_live_preview_tcb, 0);
        rp = (h != NULL) ? pdPASS : pdFAIL;
    }
    else
    {
        rp = xTaskCreatePinnedToCore(local_live_task, "local_live", 12 * 1024, NULL, 5, NULL, 0);
    }
    ri = xTaskCreatePinnedToCoreWithCaps(local_live_infer_task, "local_live_infer", 12 * 1024,
                                         NULL, 4, NULL, 1, MALLOC_CAP_SPIRAM);
    ESP_LOGW(TAG, "本地识别任务创建: 预览=%d 推理=%d 空闲内部RAM=%u", (int)rp, (int)ri, (unsigned)free_ram);
}

/* ---- 本地录入：全屏实时预览（像本地识别），编码器滚选编号 1~4，短按录入，长按退出 ---- */
/* 本地录入跨任务状态：预览/提特征跑在 PSRAM 栈（不碰 NVS），短按录入时把特征交给
 * 内部 RAM 栈的小任务写 NVS（绕开「PSRAM 栈碰 NVS → cache 禁用断言崩溃」）。 */
static SemaphoreHandle_t s_enroll_store_done = NULL;
static float s_enroll_feat[LOCAL_FACE_FEAT_LEN];     /* 平均后的特征（累加器）-> 写库任务传递 */
static float s_enroll_feat_tmp[LOCAL_FACE_FEAT_LEN]; /* 每帧临时特征 */
static char s_enroll_name[8];
static esp_err_t s_enroll_store_result = ESP_FAIL;

/* 写库任务：内部 RAM 栈（浅），只做 NVS 写入 + 刷新缓存，完成后给信号量。 */
static void enroll_store_task(void *arg)
{
    s_enroll_store_result = local_face_store_feat(s_enroll_feat, s_enroll_name);
    if (s_enroll_store_done)
        xSemaphoreGive(s_enroll_store_done);
    vTaskDelete(NULL);
}

static void local_enroll_task(void *arg)
{
    ESP_LOGW(TAG, "本地录入任务启动");
    const int FW = LCD_WIDTH, FH = LCD_HEIGHT;
    const int HW = FW / 4, HH = FH / 4; /* 80x60 预览（画质最低，识别不受影响） */

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor)
        sensor->set_quality(sensor, 18); /* 和本地识别一样，录入要更清晰的特征 */

    uint16_t *half = heap_caps_malloc(HW * HH * 2, MALLOC_CAP_SPIRAM);
    uint16_t *fbuf[2];
    fbuf[0] = heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
    fbuf[1] = heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
    if (!half || !fbuf[0] || !fbuf[1])
    {
        ESP_LOGE(TAG, "本地录入缓冲分配失败");
        free(half);
        free(fbuf[0]);
        free(fbuf[1]);
        vTaskDelete(NULL);
        return;
    }

    /* 默认编号 = 已录人数 + 1，封顶 4；编码器可随时滚选 1~4 */
    int num = local_face_count() + 1;
    if (num < 1)
        num = 1;
    if (num > 4)
        num = 4;

    lvgl_port_set_preview(true);
    memset(fbuf[0], 0, FW * FH * 2);
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, FW, FH, fbuf[0]);

    int cur = 0;
    bool prev = encoder_is_pressed(), lh = false, enrolled = false;
    char result[64] = {0};

    while (1)
    {
        bool now = encoder_is_pressed();
        int delta = encoder_read_delta();

        if (now && !prev)
            lh = false;

        if (delta)
        { /* 滚选编号 1~4 */
            num += delta;
            if (num < 1)
                num = 1;
            if (num > 4)
                num = 4;
        }

        if (now && encoder_press_duration_ms() >= 1000 && !lh)
        {
            lh = true;
            break;
        } /* 长按退出 */

        if (!now && prev && !lh)
        { /* 短按 = 录入当前画面里的脸 */
            /* 连拍 3 张求平均特征：削弱单帧噪声/角度抖动，显著提高后续识别命中率 */
            memset(s_enroll_feat, 0, LOCAL_FACE_FEAT_LEN * sizeof(float));
            int got = 0;
            esp_err_t err = ESP_OK;
            for (int k = 0; k < 3; k++)
            {
                uint8_t *jpeg = NULL;
                size_t jlen = 0;
                esp_err_t e = capture_jpeg_local(&jpeg, &jlen);
                if (e == ESP_OK)
                {
                    e = local_face_extract_feat_jpeg(jpeg, jlen, s_enroll_feat_tmp); /* PSRAM 栈，纯计算，不碰 NVS */
                    free(jpeg);
                }
                if (e != ESP_OK)
                {
                    err = e;
                    continue;
                } /* 单帧失败跳过，至少一帧成功即可 */
                for (int i = 0; i < LOCAL_FACE_FEAT_LEN; i++)
                    s_enroll_feat[i] += s_enroll_feat_tmp[i];
                got++;
                if (k < 2)
                    vTaskDelay(pdMS_TO_TICKS(150)); /* 帧间留空，让人脸稳定/微调角度 */
            }
            if (got > 0)
            {
                for (int i = 0; i < LOCAL_FACE_FEAT_LEN; i++)
                    s_enroll_feat[i] /= (float)got; /* 平均 */
                snprintf(s_enroll_name, sizeof(s_enroll_name), "%d", num);
                s_enroll_store_result = ESP_FAIL;
                BaseType_t cr = xTaskCreatePinnedToCore(enroll_store_task, "enroll_store",
                                                        4 * 1024, NULL, 4, NULL, 0);
                if (cr == pdPASS)
                {
                    xSemaphoreTake(s_enroll_store_done, portMAX_DELAY);
                    err = s_enroll_store_result;
                }
                else
                {
                    ESP_LOGW(TAG, "写库任务创建失败 err=%d", (int)cr);
                    err = ESP_FAIL;
                }
            }
            snprintf(result, sizeof(result), "%s",
                     (err == ESP_OK) ? "录入成功" : (err == ESP_ERR_NOT_FOUND)   ? "未检测到人脸"
                                                : (err == ESP_ERR_NO_MEM)        ? "特征库已满(最多32人)"
                                                : (err == ESP_ERR_INVALID_STATE) ? "模型未就绪"
                                                                                 : "录入失败");
            enrolled = true;
            break;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (fb)
        {
            if (fb->format == PIXFORMAT_JPEG &&
                jpg2gray565_quarter(fb->buf, fb->len, half, HW, HH))
            {
                /* 80x60 -> 320x240 最近邻 4x 放大（和本地识别预览一致） */
                for (int y = 0; y < HH; y++)
                    for (int x = 0; x < HW; x++)
                    {
                        uint16_t c = half[y * HW + x];
                        for (int dy = 0; dy < 4; dy++)
                        {
                            uint16_t *d = &fbuf[cur][(4 * y + dy) * FW];
                            d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = d[4 * x + 3] = c;
                        }
                    }

                draw_rect(fbuf[cur], FW, FH, 60, 26, 200, 160, COLOR_GREEN, 2); /* 取景框 */
                char txt[40];
                snprintf(txt, sizeof(txt), "录入编号: %d", num);
                draw_badge(fbuf[cur], FW, FH, 2, 2, txt, 0xFFFF);
                draw_badge(fbuf[cur], FW, FH, 2, FH - 22, "短按录入 长按退出", 0xFFFF);

                bswap16_buf(fbuf[cur], FW * FH);
                esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, FW, FH, fbuf[cur]);
                cur ^= 1;
            }
            esp_camera_fb_return(fb);
        }
        prev = now;
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    wait_btn_release();
    lvgl_port_set_preview(false);
    if (sensor)
        sensor->set_quality(sensor, 20); /* 恢复标准画质 */
    lvgl_port_lock();
    if (enrolled)
        show_result(result); /* 3 秒后自动回主菜单 */
    else
        show_main_menu();
    lvgl_port_unlock();

    free(half);
    free(fbuf[0]);
    free(fbuf[1]);
    vTaskDelete(NULL);
}

static StackType_t *s_enroll_stack = NULL;
static StaticTask_t s_enroll_tcb;

static void start_local_enroll(void)
{
    uint32_t free_ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (!s_enroll_store_done)
        s_enroll_store_done = xSemaphoreCreateBinary();
    /* 预览任务栈放 PSRAM（预览+提特征不碰 NVS，安全），把紧俏的内部 RAM 留给 6KB 写库任务 */
    if (!s_enroll_stack)
        s_enroll_stack = (StackType_t *)heap_caps_aligned_alloc(16, 12 * 1024, MALLOC_CAP_SPIRAM);

    BaseType_t r;
    if (s_enroll_stack)
        r = (xTaskCreateStaticPinnedToCore(local_enroll_task, "local_enroll", 12 * 1024,
                                           NULL, 5, s_enroll_stack, &s_enroll_tcb, 0) != NULL)
                ? pdPASS
                : pdFAIL;
    else
        r = xTaskCreatePinnedToCore(local_enroll_task, "local_enroll", 12 * 1024, NULL, 5, NULL, 0);
    ESP_LOGW(TAG, "本地录入任务创建: 结果=%d 空闲内部RAM=%u 最大连续块=%u",
             (int)r, (unsigned)free_ram, (unsigned)largest);
}

/* ---- 人脸库页：列出已录入的人 + 头像 + 长按删除 ---- */

#define FACE_LIB_MAX 16
#define AVATAR_SZ 48 /* 电脑端录入时裁成 48x48 头像 */
#define LIB_TITLE_H 24
#define LIB_ROW_H 52

typedef struct
{
    char name[32];
    uint16_t *avatar; /* AVATAR_SZ*AVATAR_SZ RGB565，无头像为 NULL */
} lib_entry_t;

/* 解码 JPEG → RGB565 彩色（头像用，不改灰度）。scale 0 = 原尺寸。 */
static bool jpg2rgb565_color(const uint8_t *src, size_t src_len, uint16_t *out, int w, int h)
{
    static uint8_t work[3100];
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)src,
        .indata_size = src_len,
        .outbuf = (uint8_t *)out,
        .outbuf_size = (uint32_t)w * h * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags.swap_color_bytes = 0,
        .advanced.working_buffer = work,
        .advanced.working_buffer_size = sizeof(work),
    };
    esp_jpeg_image_output_t info = {0};
    return esp_jpeg_decode(&cfg, &info) == ESP_OK;
}

/* 把一块 sz*sz 的 RGB565 小图贴到大缓冲 (x,y) 处（越界裁剪） */
static void blit_avatar(uint16_t *dst, int dw, int dh, int x, int y,
                        const uint16_t *src, int sz)
{
    for (int r = 0; r < sz; r++)
        for (int c = 0; c < sz; c++)
        {
            int dx = x + c, dy = y + r;
            if (dx >= 0 && dx < dw && dy >= 0 && dy < dh)
                dst[dy * dw + dx] = src[r * sz + c];
        }
}

/* 画一行：头像 + 姓名；选中白框，确认删除时红框 + 红字提示 */
static void draw_lib_row(uint16_t *buf, int w, int h, int x, int y, int rw, int rh,
                         const char *name, const uint16_t *avatar, bool selected, bool confirming)
{
    for (int yy = y; yy < y + rh && yy < h; yy++)
        for (int xx = x; xx < x + rw && xx < w; xx++)
            buf[yy * w + xx] = 0x0000;

    if (avatar)
    {
        blit_avatar(buf, w, h, x + 4, y + 2, avatar, AVATAR_SZ);
    }
    else
    {
        draw_rect(buf, w, h, x + 4, y + 2, AVATAR_SZ, AVATAR_SZ, COLOR_GRAY, 2);
        draw_text_rgb565(buf, w, h, x + 4 + 19, y + 2 + 16, "?", 0x7BEF);
    }

    if (confirming)
    {
        draw_text_rgb565(buf, w, h, x + 4 + AVATAR_SZ + 10, y + 18, "长按确认删除", COLOR_RED);
    }
    else
    {
        draw_text_rgb565(buf, w, h, x + 4 + AVATAR_SZ + 10, y + 18, name, 0xFFFF);
    }

    if (selected)
    {
        draw_rect(buf, w, h, x, y, rw, rh, confirming ? COLOR_RED : 0xFFFF, 2);
    }
}

/* 画整个人脸库界面；返回时更新 *scroll 让选中行可见 */
static void render_library(uint16_t *buf, int w, int h, const lib_entry_t *lib, int count,
                           int sel, int *scroll, bool confirming, bool connect_fail)
{
    memset(buf, 0, w * h * 2);

    char title[48];
    if (connect_fail)
        strcpy(title, "人脸库连接失败");
    else
        snprintf(title, sizeof(title), "人脸库 %d 人", count);
    draw_text_rgb565(buf, w, h, 8, 4, title, 0xFFFF);
    draw_text_rgb565(buf, w, h, w - 8 - text_width_px("长按删除"), 4, "长按删除", 0x7BEF);

    const int back_h = LIB_ROW_H;
    const int area_h = h - LIB_TITLE_H - back_h;
    const int visible = area_h / LIB_ROW_H;

    if (sel < count)
    {
        if (sel < *scroll)
            *scroll = sel;
        if (sel >= *scroll + visible)
            *scroll = sel - visible + 1;
    }
    if (*scroll < 0)
        *scroll = 0;

    for (int k = 0; k < visible; k++)
    {
        int i = *scroll + k;
        if (i >= count)
            break;
        int y = LIB_TITLE_H + k * LIB_ROW_H;
        draw_lib_row(buf, w, h, 0, y, w, LIB_ROW_H, lib[i].name, lib[i].avatar,
                     (i == sel), (i == sel) && confirming);
    }

    if (count == 0)
    {
        draw_text_rgb565(buf, w, h, 16, LIB_TITLE_H + 30,
                         connect_fail ? "请确认电脑端服务已启动" : "尚未录入人脸", 0x7BEF);
    }

    int by = h - back_h;
    for (int yy = by; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            buf[yy * w + xx] = 0x0000;
    draw_text_rgb565(buf, w, h, 16, by + (back_h - 16) / 2, "返回", 0xFFFF);
    if (sel == count)
        draw_rect(buf, w, h, 0, by, w, back_h, 0xFFFF, 2);
}

/* 人脸库任务：拉名单+头像，编码器滚选，短按返回/取消，长按删除 */
static void library_task(void *arg)
{
    const char *host = face_client_get_cached_host();
    const int FW = LCD_WIDTH, FH = LCD_HEIGHT;

    lvgl_port_set_preview(true);

    uint16_t *fbuf = heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
    if (!fbuf)
    {
        ESP_LOGE(TAG, "人脸库缓冲分配失败");
        lvgl_port_set_preview(false);
        lvgl_port_lock();
        show_main_menu();
        lvgl_port_unlock();
        vTaskDelete(NULL);
        return;
    }

    lib_entry_t lib[FACE_LIB_MAX];
    memset(lib, 0, sizeof(lib));
    char names[FACE_LIB_MAX][32];
    int count = 0;
    bool connect_fail = (face_client_list_faces(host, names, FACE_LIB_MAX, &count) != ESP_OK);
    if (connect_fail)
        count = 0;
    ESP_LOGI(TAG, "人脸库: host=%s count=%d connect_fail=%d", host, count, connect_fail);

    static uint8_t avjpg[4096]; /* 头像 JPEG 临时缓冲（48x48 通常 1~2KB） */
    for (int i = 0; i < count; i++)
    {
        strncpy(lib[i].name, names[i], sizeof(lib[i].name) - 1);
        lib[i].avatar = heap_caps_malloc(AVATAR_SZ * AVATAR_SZ * 2, MALLOC_CAP_SPIRAM);
        if (lib[i].avatar)
        {
            size_t alen = 0;
            if (face_client_get_avatar(host, names[i], avjpg, sizeof(avjpg), &alen) == ESP_OK &&
                alen > 0 && jpg2rgb565_color(avjpg, alen, lib[i].avatar, AVATAR_SZ, AVATAR_SZ))
            {
                ESP_LOGI(TAG, "头像就位: %s (%u bytes)", names[i], (unsigned)alen);
            }
            else
            {
                ESP_LOGW(TAG, "头像缺失: %s", names[i]);
                free(lib[i].avatar);
                lib[i].avatar = NULL;
            }
        }
    }

    int sel = 0, scroll = 0;
    bool confirming = false;
    bool prev_pressed = encoder_is_pressed();
    bool long_handled = false;
    bool exit = false;

    while (!exit)
    {
        int delta = encoder_read_delta();
        bool now = encoder_is_pressed();

        if (now && !prev_pressed)
            long_handled = false;

        /* 长按 1 秒：选中人 -> 进入/执行删除确认；返回行 -> 退出 */
        if (now && encoder_press_duration_ms() >= 1000 && !long_handled)
        {
            long_handled = true;
            if (sel < count)
            {
                if (!confirming)
                {
                    confirming = true;
                }
                else if (face_client_delete_face(host, lib[sel].name) == ESP_OK)
                {
                    free(lib[sel].avatar);
                    for (int i = sel; i < count - 1; i++)
                        lib[i] = lib[i + 1];
                    memset(&lib[count - 1], 0, sizeof(lib[count - 1]));
                    count--;
                    confirming = false;
                    if (sel > count)
                        sel = count;
                }
                else
                {
                    confirming = false; /* 删除失败，退出确认态 */
                }
            }
            else
            {
                exit = true;
            }
        }

        /* 短按（松手）：取消删除确认；或返回行 -> 退出 */
        if (!now && prev_pressed && !long_handled)
        {
            if (confirming)
                confirming = false;
            else if (sel == count)
                exit = true;
        }

        if (delta)
        {
            confirming = false;
            sel += delta;
            if (sel < 0)
                sel = 0;
            if (sel > count)
                sel = count;
        }

        render_library(fbuf, FW, FH, lib, count, sel, &scroll, confirming, connect_fail);
        bswap16_buf(fbuf, FW * FH);
        esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, FW, FH, fbuf);

        prev_pressed = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    for (int i = 0; i < count; i++)
    {
        if (lib[i].avatar)
            free(lib[i].avatar);
    }
    wait_btn_release();
    lvgl_port_set_preview(false);
    lvgl_port_lock();
    show_main_menu();
    lvgl_port_unlock();
    free(fbuf);
    vTaskDelete(NULL);
}

static void start_library(void)
{
    xTaskCreatePinnedToCore(library_task, "face_lib", 12 * 1024, NULL, 5, NULL, 0);
}

/* ---- 本地库：列出本地特征库里的姓名，长按删除（无头像，纯文字行） ---- */

static void render_local_library(uint16_t *buf, int w, int h, char names[][32],
                                 int count, int sel, int *scroll, bool confirming)
{
    memset(buf, 0, w * h * 2);

    char title[48];
    snprintf(title, sizeof(title), "本地库 %d 人", count);
    draw_text_rgb565(buf, w, h, 8, 4, title, 0xFFFF);
    draw_text_rgb565(buf, w, h, w - 8 - text_width_px("长按删除"), 4, "长按删除", 0x7BEF);

    const int back_h = LIB_ROW_H;
    const int area_h = h - LIB_TITLE_H - back_h;
    const int visible = area_h / LIB_ROW_H;

    if (sel < count)
    {
        if (sel < *scroll)
            *scroll = sel;
        if (sel >= *scroll + visible)
            *scroll = sel - visible + 1;
    }
    if (*scroll < 0)
        *scroll = 0;

    for (int k = 0; k < visible; k++)
    {
        int i = *scroll + k;
        if (i >= count)
            break;
        int y = LIB_TITLE_H + k * LIB_ROW_H;
        draw_lib_row(buf, w, h, 0, y, w, LIB_ROW_H, names[i], NULL,
                     (i == sel), (i == sel) && confirming);
    }

    if (count == 0)
    {
        draw_text_rgb565(buf, w, h, 16, LIB_TITLE_H + 30, "尚未录入本地人脸", 0x7BEF);
    }

    int by = h - back_h;
    for (int yy = by; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            buf[yy * w + xx] = 0x0000;
    draw_text_rgb565(buf, w, h, 16, by + (back_h - 16) / 2, "返回", 0xFFFF);
    if (sel == count)
        draw_rect(buf, w, h, 0, by, w, back_h, 0xFFFF, 2);
}

static void local_library_task(void *arg)
{
    const int FW = LCD_WIDTH, FH = LCD_HEIGHT;
    lvgl_port_set_preview(true);

    uint16_t *fbuf = heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
    if (!fbuf)
    {
        ESP_LOGE(TAG, "本地库缓冲分配失败");
        lvgl_port_set_preview(false);
        lvgl_port_lock();
        show_main_menu();
        lvgl_port_unlock();
        vTaskDelete(NULL);
        return;
    }

    char names[32][32];
    memset(names, 0, sizeof(names));
    int count = 0;
    local_face_list(names, 32, &count);

    int sel = 0, scroll = 0;
    bool confirming = false;
    bool prev_pressed = encoder_is_pressed();
    bool long_handled = false;
    bool exit = false;

    while (!exit)
    {
        int delta = encoder_read_delta();
        bool now = encoder_is_pressed();

        if (now && !prev_pressed)
            long_handled = false;

        if (now && encoder_press_duration_ms() >= 1000 && !long_handled)
        {
            long_handled = true;
            if (sel < count)
            {
                if (!confirming)
                {
                    confirming = true;
                }
                else if (local_face_delete(names[sel]) == ESP_OK)
                {
                    for (int i = sel; i < count - 1; i++)
                        strcpy(names[i], names[i + 1]);
                    names[count - 1][0] = 0;
                    count--;
                    confirming = false;
                    if (sel > count)
                        sel = count;
                }
                else
                {
                    confirming = false;
                }
            }
            else
            {
                exit = true;
            }
        }

        if (!now && prev_pressed && !long_handled)
        {
            if (confirming)
                confirming = false;
            else if (sel == count)
                exit = true;
        }

        if (delta)
        {
            confirming = false;
            sel += delta;
            if (sel < 0)
                sel = 0;
            if (sel > count)
                sel = count;
        }

        render_local_library(fbuf, FW, FH, names, count, sel, &scroll, confirming);
        bswap16_buf(fbuf, FW * FH);
        esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, FW, FH, fbuf);

        prev_pressed = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    wait_btn_release();
    lvgl_port_set_preview(false);
    lvgl_port_lock();
    show_main_menu();
    lvgl_port_unlock();
    free(fbuf);
    vTaskDelete(NULL);
}

static void start_local_library(void)
{
    xTaskCreatePinnedToCore(local_library_task, "local_lib", 8 * 1024, NULL, 5, NULL, 0);
}

/* ---- 联动模式：舵机回中 + 提示在电脑端启动联动程序，长按退出 ---- */
static void link_task(void *arg)
{
    const int FW = LCD_WIDTH, FH = LCD_HEIGHT;

    lvgl_port_set_preview(true);
    uint16_t *fbuf = heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
    if (!fbuf)
    {
        ESP_LOGE(TAG, "联动页缓冲分配失败");
        lvgl_port_set_preview(false);
        lvgl_port_lock();
        show_main_menu();
        lvgl_port_unlock();
        vTaskDelete(NULL);
        return;
    }

    servo_center(); /* 云台回中，等待电脑端发 pan/tilt 指令 */

    memset(fbuf, 0, FW * FH * 2);
    draw_text_rgb565(fbuf, FW, FH, 8, 20, "联动模式", 0xFFFF);
    draw_text_rgb565(fbuf, FW, FH, 8, 60, "请在电脑端运行", 0x7BEF);
    draw_text_rgb565(fbuf, FW, FH, 8, 84, "gesture_track.py", 0x7BEF);
    draw_text_rgb565(fbuf, FW, FH, 8, 116, "舵机追踪 + 手势识别", 0x7BEF);
    draw_text_rgb565(fbuf, FW, FH, 8, FH - 30, "长按退出", 0xFFFF);
    bswap16_buf(fbuf, FW * FH);
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, FW, FH, fbuf);

    bool prev = encoder_is_pressed(), lh = false;
    while (1)
    {
        bool now = encoder_is_pressed();
        if (now && !prev)
            lh = false;
        if (now && encoder_press_duration_ms() >= 1000 && !lh)
        {
            lh = true;
            break;
        }
        prev = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    wait_btn_release();
    lvgl_port_set_preview(false);
    lvgl_port_lock();
    show_main_menu();
    lvgl_port_unlock();
    free(fbuf);
    vTaskDelete(NULL);
}

static void start_link(void)
{
    xTaskCreatePinnedToCore(link_task, "face_link", 6 * 1024, NULL, 5, NULL, 0);
}

/* ---- 采集流程：小窗预览 + 倒计时 + 拍照上传 ---- */

/* 预览时的静态提示画面（LVGL 渲染一帧后冻结；小窗由 capture_task 直接刷 LCD） */
static void show_preview_screen(cap_mode_t mode, const char *name)
{
    ui_clean();
    char title[64];
    const char *hint;
    if (mode == CAP_ENROLL)
    {
        snprintf(title, sizeof(title), "录入: %s", name);
        hint = "请把脸放进框内";
    }
    else if (mode == CAP_LOCAL_ENROLL)
    {
        snprintf(title, sizeof(title), "本地录入: %s", name);
        hint = "请把脸放进框内";
    }
    else if (mode == CAP_LOCAL_RECOGNIZE)
    {
        snprintf(title, sizeof(title), "本地识别中");
        hint = "对准脸 短按拍照识别";
    }
    else
    {
        snprintf(title, sizeof(title), "识别中");
        hint = "绿框=已录入 红框=未录入";
    }
    lv_obj_t *t = ui_label(s_scr, title);
    lv_obj_set_width(t, 130);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, THUMB_OFF_X + THUMB_W + 12, 16);

    lv_obj_t *h = ui_label(s_scr, hint);
    lv_obj_set_width(h, 130);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, THUMB_OFF_X + THUMB_W + 12, 70);

    lv_obj_t *h1 = ui_label(s_scr, "短按拍照 长按退出");
    lv_obj_align(h1, LV_ALIGN_BOTTOM_LEFT, THUMB_OFF_X, -10);

    lv_obj_t *h2 = ui_label(s_scr, "10秒后自动拍");
    lv_obj_align(h2, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
}

/* 采集一帧 JPEG（本地识别用）。摄像头可能只有 2 个帧缓冲，重试几次避开推流争抢。 */
static esp_err_t capture_jpeg_local(uint8_t **jpeg_out, size_t *len_out)
{
    for (int i = 0; i < 10; i++)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (fb->format == PIXFORMAT_JPEG)
        {
            uint8_t *copy = malloc(fb->len);
            if (copy)
            {
                memcpy(copy, fb->buf, fb->len);
                *jpeg_out = copy;
                *len_out = fb->len;
                esp_camera_fb_return(fb);
                return ESP_OK;
            }
            esp_camera_fb_return(fb);
            return ESP_ERR_NO_MEM;
        }
        esp_camera_fb_return(fb);
        return ESP_FAIL; /* 非 JPEG，兜底不做转码 */
    }
    return ESP_FAIL;
}

static void capture_task(void *arg)
{
    capture_job_t *job = (capture_job_t *)arg;
    const char *host = face_client_get_cached_host();

    /* 1) 先让 LVGL 渲染一帧静态提示画面，再冻结（小窗直接写 LCD，避免和 LVGL 抢屏）。
     *    关键：必须在 set_preview(true) 之前同步刷新。否则这些标签只是被标记为无效，
     *    还没来得及画就被冻结，预览期间看到的仍是旧菜单，观感就是"一闪而过"。 */
    lvgl_port_lock();
    show_preview_screen(job->mode, job->name);
    lv_refr_now(NULL); // 立刻把提示文字同步刷到屏上
    lvgl_port_unlock();
    lvgl_port_set_preview(true);
    vTaskDelay(pdMS_TO_TICKS(50)); // 等 SPI 把这帧发完，再开始画小窗

    /* 2) 两块 160x120 小缓冲交替用，资源远小于之前的全屏 320x240 预览 */
    uint16_t *thumb[2];
    thumb[0] = heap_caps_malloc(THUMB_W * THUMB_H * 2, MALLOC_CAP_SPIRAM);
    thumb[1] = heap_caps_malloc(THUMB_W * THUMB_H * 2, MALLOC_CAP_SPIRAM);
    if (!thumb[0] || !thumb[1])
    {
        ESP_LOGE(TAG, "缩略图缓冲分配失败");
        lvgl_port_set_preview(false);
        free(thumb[0]);
        free(thumb[1]);
        free(job);
        vTaskDelete(NULL);
        return;
    }
    int cur = 0;
    int frame_cnt = 0; // 诊断用：只打印前 3 帧，确认取帧/解码是否正常

    /* 3) 实时预览 + 倒计时（按墙钟计时，识别模式的网络往返不会拖慢秒数），
     *    按编码器立即拍照，到 0 自动拍照。 */
    const TickType_t tick_per_sec = pdMS_TO_TICKS(1000);
    TickType_t deadline = xTaskGetTickCount() + tick_per_sec * FACE_COUNTDOWN_SEC;
    bool pc_online = true;                // 识别模式：电脑端是否可达
    bool canceled = false;                // 长按取消标记（未拍照直接回菜单）
    bool last_btn = encoder_is_pressed(); // 记录按键电平，用于检测"松手=短按拍照"

    while (1)
    {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline)
            break; // 到点自动拍
        int sec = (int)((deadline - now + tick_per_sec - 1) / tick_per_sec);
        if (sec > FACE_COUNTDOWN_SEC)
            sec = FACE_COUNTDOWN_SEC;

        /* 按键：长按 1 秒 = 取消退出；松手 = 短按立即拍照。
         * 放在取帧前检查，识别模式网络往返阻塞时也能及时响应退出。 */
        bool bnow = encoder_is_pressed();
        if (bnow && encoder_press_duration_ms() >= 1000)
        {
            canceled = true;
            break;
        }
        if (!bnow && last_btn)
            break; // 松手 → 立即拍照
        last_btn = bnow;

        camera_fb_t *fb = esp_camera_fb_get();
        if (fb)
        {
            bool ok = (fb->format == PIXFORMAT_JPEG) &&
                      jpg2gray565(fb->buf, fb->len, thumb[cur], THUMB_W, THUMB_H);
            if (frame_cnt < 3)
            {
                ESP_LOGI(TAG, "预览帧%d: fb_len=%d fmt=%d 解码=%d", frame_cnt,
                         (int)fb->len, (int)fb->format, (int)ok);
            }
            if (ok)
            {

                if (job->mode == CAP_ENROLL || job->mode == CAP_LOCAL_ENROLL)
                {
                    /* 录入：固定绿色取景框引导对准 */
                    draw_rect(thumb[cur], THUMB_W, THUMB_H, 25, 15, 110, 90, COLOR_GREEN, 2);
                }
                else if (job->mode == CAP_RECOGNIZE)
                {
                    /* 识别：把这一帧发电脑端，实时拿框 + 是否已录入 */
                    bool drew = false;
                    if (pc_online)
                    {
                        face_result_t r;
                        if (face_client_recognize_frame(host, fb->buf, fb->len, &r) == ESP_OK)
                        {
                            if (r.detected && r.has_box)
                            {
                                int x = r.box[0] / 2, y = r.box[1] / 2;
                                int w = r.box[2] / 2, h = r.box[3] / 2;
                                draw_rect(thumb[cur], THUMB_W, THUMB_H, x, y, w, h,
                                          r.matched ? COLOR_GREEN : COLOR_RED, 2);
                                drew = true;
                            }
                        }
                        else
                        {
                            pc_online = false; // HTTP 失败：电脑端不可达，退本地灰框
                        }
                    }
                    if (!drew)
                    {
                        draw_rect(thumb[cur], THUMB_W, THUMB_H, 25, 15, 110, 90, COLOR_GRAY, 2);
                    }
                }
                else
                {
                    /* 本地识别：灰框预览（不做每帧本地检测，节省 CPU） */
                    draw_rect(thumb[cur], THUMB_W, THUMB_H, 25, 15, 110, 90, COLOR_GRAY, 2);
                }

                draw_countdown(thumb[cur], THUMB_W, THUMB_H, sec, 3);
                bswap16_buf(thumb[cur], THUMB_W * THUMB_H); // 小端 -> 大端，ST7789 才不花屏
                esp_lcd_panel_draw_bitmap(panel_handle, THUMB_OFF_X, THUMB_OFF_Y,
                                          THUMB_OFF_X + THUMB_W, THUMB_OFF_Y + THUMB_H,
                                          thumb[cur]);
                cur ^= 1;
            }
            esp_camera_fb_return(fb);
            frame_cnt++;
        }

        vTaskDelay(pdMS_TO_TICKS(40)); // 预览刷新节流：40ms ≈ 最高 25 FPS（录入预览）
    }

    /* 4) 结束预览：长按取消则直接回菜单；否则正式采集 + 上传 + 显示结果 */
    wait_btn_release();
    lvgl_port_set_preview(false);

    if (canceled)
    {
        lvgl_port_lock();
        show_main_menu();
        lvgl_port_unlock();
        free(thumb[0]);
        free(thumb[1]);
        free(job);
        vTaskDelete(NULL);
        return;
    }

    /* 本地/上传前先提示，否则阻塞期间屏幕静止，看起来像卡死 */
    bool is_local = (job->mode == CAP_LOCAL_ENROLL || job->mode == CAP_LOCAL_RECOGNIZE);
    lvgl_port_lock();
    show_message(is_local ? "本地识别中..." : "正在连接电脑...");
    lvgl_port_unlock();

    char result[128];
    if (job->mode == CAP_ENROLL)
    {
        esp_err_t err = face_client_enroll(host, job->name);
        const char *m = face_client_last_msg();
        snprintf(result, sizeof(result), "%s",
                 (m && m[0]) ? m : (err == ESP_OK ? "录入成功" : "录入失败"));
    }
    else if (job->mode == CAP_RECOGNIZE)
    {
        face_result_t r;
        esp_err_t err = face_client_recognize(host, &r);
        if (err != ESP_OK)
            snprintf(result, sizeof(result), "识别失败");
        else if (!r.detected)
            snprintf(result, sizeof(result), "未检测到人脸");
        else if (r.matched)
            snprintf(result, sizeof(result), "%s 已录入 %.3f", r.name, r.similarity);
        else
            snprintf(result, sizeof(result), "未录入 %.3f", r.similarity);
    }
    else if (job->mode == CAP_LOCAL_ENROLL)
    {
        uint8_t *jpeg = NULL;
        size_t jlen = 0;
        esp_err_t err = capture_jpeg_local(&jpeg, &jlen);
        if (err == ESP_OK)
        {
            err = local_face_enroll_jpeg(jpeg, jlen, job->name);
            free(jpeg);
        }
        snprintf(result, sizeof(result), "%s",
                 (err == ESP_OK) ? "本地录入成功" : (err == ESP_ERR_NOT_FOUND) ? "未检测到人脸"
                                                : (err == ESP_ERR_NO_MEM)      ? "特征库已满"
                                                                               : "本地录入失败");
    }
    else
    { /* CAP_LOCAL_RECOGNIZE */
        uint8_t *jpeg = NULL;
        size_t jlen = 0;
        esp_err_t err = capture_jpeg_local(&jpeg, &jlen);
        if (err == ESP_OK)
        {
            char name[32];
            float sim = 0.0f;
            esp_err_t r = local_face_recognize_jpeg(jpeg, jlen, name, sizeof(name), &sim);
            free(jpeg);
            if (r == ESP_OK)
                snprintf(result, sizeof(result), "%s %.3f", name, sim);
            else if (r == ESP_ERR_NOT_FOUND)
                snprintf(result, sizeof(result), "未录入 %.3f", sim);
            else
                snprintf(result, sizeof(result), "未检测到人脸");
        }
        else
        {
            snprintf(result, sizeof(result), "采集失败");
        }
    }

    lvgl_port_lock();
    show_result(result);
    lvgl_port_unlock();

    free(thumb[0]);
    free(thumb[1]);
    free(job);
    vTaskDelete(NULL);
}

/* ---- 菜单回调 ---- */
static void menu_enroll_cb(lv_event_t *e) { show_name_select(); }
static void menu_recognize_cb(lv_event_t *e) { start_capture(CAP_RECOGNIZE, ""); }
static void menu_multi_cb(lv_event_t *e) { start_multi(); }
static void menu_library_cb(lv_event_t *e) { start_library(); }
static void menu_link_cb(lv_event_t *e) { start_link(); }
static void menu_local_cb(lv_event_t *e) { start_local_live(); }
static void menu_local_enroll_cb(lv_event_t *e) { start_local_enroll(); }
static void menu_local_lib_cb(lv_event_t *e) { start_local_library(); }
static void menu_more_cb(lv_event_t *e) { show_more_menu(); }
static void menu_sleep_cb(lv_event_t *e) { enter_sleep(); }
static void name_click_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    start_capture(CAP_ENROLL, name);
}
static void back_menu_cb(lv_event_t *e) { show_main_menu(); }

/* 低功耗休眠：用编码器按键（GPIO42，低电平=按下）唤醒，模拟雷达/红外探测到人来唤醒整机。
 * 注意：ESP32-S3 深度睡眠(deep sleep)只能靠 RTC GPIO(0~21) 唤醒；编码器按键在 GPIO42 不是
 * RTC 脚，硬用 EXT0 唤醒会一睡不醒。这里改用浅睡(light sleep) + 任意 GPIO 唤醒，效果等价
 * （按下即醒、整机回到菜单），只是省电深度略浅。若要真深度睡眠，把按键改接到空闲 RTC 脚
 * （如 GPIO19/20），再把 esp_sleep_enable_gpio_wakeup 换成 esp_sleep_enable_ext0_wakeup 即可。 */
static void enter_sleep(void)
{
    wait_btn_release(); // 确保按键已松开（高电平），否则配成低电平唤醒会立刻醒

    /* 冻结 LVGL，手绘"休眠中"黑屏提示（保证入睡前屏幕已刷出） */
    lvgl_port_set_preview(true);
    uint16_t *black = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (black)
    {
        memset(black, 0, LCD_WIDTH * LCD_HEIGHT * 2);
        draw_text_rgb565(black, LCD_WIDTH, LCD_HEIGHT,
                         (LCD_WIDTH - text_width_px("休眠中")) / 2, LCD_HEIGHT / 2 - 10,
                         "休眠中", 0xFFFF);
        draw_text_rgb565(black, LCD_WIDTH, LCD_HEIGHT,
                         (LCD_WIDTH - text_width_px("按下按键唤醒")) / 2, LCD_HEIGHT / 2 + 16,
                         "按下按键唤醒", 0x7BEF);
        bswap16_buf(black, LCD_WIDTH * LCD_HEIGHT);
        esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_WIDTH, LCD_HEIGHT, black);
        free(black);
    }
    vTaskDelay(pdMS_TO_TICKS(200)); // 让屏刷完再睡，避免唤醒时残留

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);                   // 清掉可能残留的旧唤醒源
    esp_err_t we = gpio_wakeup_enable(ENCODER_SW_GPIO, GPIO_INTR_LOW_LEVEL); // 按下(低电平)即唤醒
    esp_err_t se = esp_sleep_enable_gpio_wakeup();
    ESP_LOGW(TAG, "配唤醒 gpio_wakeup_enable=%d enable_gpio_wakeup=%d (GPIO%d 低电平)", (int)we, (int)se, ENCODER_SW_GPIO);
    esp_wifi_stop(); // 关射频：AP 的周期信标会挡着 CPU 进不了浅睡

    ESP_LOGW(TAG, "进入浅睡，等 GPIO%d 按下唤醒 ...", ENCODER_SW_GPIO);
    esp_err_t slp = esp_light_sleep_start();
    ESP_LOGW(TAG, "浅睡返回 ret=%d 唤醒原因=%d", (int)slp, (int)esp_sleep_get_wakeup_cause());

    esp_wifi_start(); // 唤醒后恢复热点（模式/SSID/密码仍保留在驱动里）

    /* 唤醒后：按键通常还按着，先等它松开，避免"松手"被 LVGL 当成一次点击 */
    wait_btn_release();
    lvgl_port_set_preview(false);
    /* 这里不能 lvgl_port_lock()：enter_sleep() 由 menu_sleep_cb 触发，而 menu_sleep_cb 是在
     * lvgl_task 的 lv_timer_handler() 里被调用的，那时 lvgl_mux 已被 lvgl_port_lock() 持有。
     * 再拿同一把非递归锁会死锁（这正是之前唤醒后卡在"休眠中"界面的原因），直接刷界面即可。 */
    show_main_menu();
    ESP_LOGI(TAG, "已唤醒，回到主菜单");
}

/* ---- 界面：主菜单 ---- */
static void show_main_menu(void)
{
    ui_clean();
    lv_obj_t *title = ui_label(s_scr, "人脸识别");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* 本地识别 = 主功能，放最大最醒目；下面依次 本地库 / PC功能 / 休眠 */
    lv_obj_t *b1 = lv_btn_create(s_scr);
    lv_obj_set_size(b1, 220, 54);
    lv_obj_align(b1, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_center(ui_label(b1, "本地识别"));
    lv_obj_add_event_cb(b1, menu_local_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b2 = lv_btn_create(s_scr);
    lv_obj_set_size(b2, 220, 40);
    lv_obj_align(b2, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_center(ui_label(b2, "本地库"));
    lv_obj_add_event_cb(b2, menu_local_lib_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b3 = lv_btn_create(s_scr);
    lv_obj_set_size(b3, 220, 40);
    lv_obj_align(b3, LV_ALIGN_TOP_MID, 0, 144);
    lv_obj_center(ui_label(b3, "PC功能"));
    lv_obj_add_event_cb(b3, menu_more_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b4 = lv_btn_create(s_scr);
    lv_obj_set_size(b4, 220, 40);
    lv_obj_align(b4, LV_ALIGN_TOP_MID, 0, 192);
    lv_obj_center(ui_label(b4, "休眠"));
    lv_obj_add_event_cb(b4, menu_sleep_cb, LV_EVENT_CLICKED, NULL);

    lv_group_add_obj(s_group, b1);
    lv_group_add_obj(s_group, b2);
    lv_group_add_obj(s_group, b3);
    lv_group_add_obj(s_group, b4);
    lv_group_focus_obj(b1);
}

/* ---- 界面：选择姓名（录入用，cb 决定录入到 PC 还是本地） ---- */
static void show_name_select_with_cb(lv_event_cb_t cb)
{
    ui_clean();
    lv_obj_t *title = ui_label(s_scr, "选择姓名");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *first = NULL;
    int y = 45;
    for (int i = 0; i < PRESET_NAME_COUNT; i++)
    {
        lv_obj_t *b = lv_btn_create(s_scr);
        lv_obj_set_size(b, 240, 40);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_center(ui_label(b, PRESET_NAMES[i]));
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)PRESET_NAMES[i]);
        lv_group_add_obj(s_group, b);
        if (!first)
            first = b;
        y += 48;
    }
    lv_group_focus_obj(first);
}

static void show_name_select(void) { show_name_select_with_cb(name_click_cb); }

/* ---- 界面：更多功能子菜单（PC 相关功能收起在这里） ---- */
static void show_more_menu(void)
{
    ui_clean();
    lv_obj_t *title = ui_label(s_scr, "PC功能");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    /* 现在 PC 功能只保留「本地录入」；其余 PC 入口（识别人脸/录入人脸/多人识别/人脸库/联动）
     * 先移除菜单、代码未删。想加回来照下面格式补一行即可：
     *   {"识别人脸", menu_recognize_cb}, {"录入人脸", menu_enroll_cb},
     *   {"多人识别", menu_multi_cb}, {"人脸库", menu_library_cb}, {"联动", menu_link_cb}, */
    struct
    {
        const char *txt;
        lv_event_cb_t cb;
    } items[] = {
        {"本地录入", menu_local_enroll_cb},
        {"返回", back_menu_cb},
    };
    int n = sizeof(items) / sizeof(items[0]);
    lv_obj_t *first = NULL;
    int y = 28;
    for (int i = 0; i < n; i++)
    {
        lv_obj_t *b = lv_btn_create(s_scr);
        lv_obj_set_size(b, 220, 32);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_center(ui_label(b, items[i].txt));
        lv_obj_add_event_cb(b, items[i].cb, LV_EVENT_CLICKED, NULL);
        lv_group_add_obj(s_group, b);
        if (!first)
            first = b;
        y += 34;
    }
    lv_group_focus_obj(first);
}

/* ---- 界面：结果（3 秒后自动回主菜单） ---- */
static void return_to_menu_cb(lv_timer_t *t)
{
    s_return_timer = NULL;
    lv_timer_del(t);
    show_main_menu();
}

static void show_message(const char *msg)
{
    ui_clean();
    lv_obj_t *lbl = ui_label(s_scr, msg);
    lv_obj_set_width(lbl, 290);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
}

static void show_result(const char *msg)
{
    show_message(msg);
    s_return_timer = lv_timer_create(return_to_menu_cb, 3000, NULL);
}

static void start_capture(cap_mode_t mode, const char *name)
{
    capture_job_t *job = malloc(sizeof(*job));
    if (!job)
        return;
    job->mode = mode;
    memset(job->name, 0, sizeof(job->name));
    if (name)
        strncpy(job->name, name, sizeof(job->name) - 1);
    xTaskCreatePinnedToCore(capture_task, "face_cap", 16 * 1024, job, 5, NULL, 0);
}

/* 在独立大栈任务里加载本地人脸模型。主任务栈只有 3.5KB，不足以承载 ESP-DL 模型解析。
 * 任务栈放 PSRAM（模型只读 flash、不擦写，PSRAM 栈安全），把紧俏的内部 RAM 留给驱动。 */
static void local_face_init_task(void *arg)
{
    local_face_init();
    vTaskDeleteWithCaps(NULL);
}

void face_ui_start(void)
{
    /* featdb 分区初始化在内部栈上先做：首次使用会擦写 flash（禁用 cache，PSRAM 栈会崩），
     * 之后模型加载任务才能安全地用 PSRAM 栈。 */
    local_face_init_nvs();

    /* 菜单不阻塞等模型：先画菜单，模型在后台加载。
     * 以前这里是 xSemaphoreTake(portMAX_DELAY) 阻塞等模型加载完，一旦模型加载
     * 卡住/任务创建失败，菜单就永远画不出来（TFT 停在复位白屏）。改成后台加载后，
     * 进「本地识别」时若模型还没就绪，local_face_* 会返回 ESP_ERR_INVALID_STATE。 */
    uint32_t free_ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(local_face_init_task, "lf_init", 16 * 1024,
                                                   NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM);
    ESP_LOGW(TAG, "模型加载任务创建=%d 空闲内部RAM=%u", (int)r, (unsigned)free_ram);
    if (r != pdPASS)
    {
        ESP_LOGE(TAG, "模型加载任务创建失败（内部RAM不足），本地识别将不可用");
    }

    // 此时 lvgl 任务已在运行，跨线程操作 LVGL 必须加锁
    lvgl_port_lock();
    s_indev = lvgl_port_get_encoder_indev();
    s_group = lv_group_create();
    lv_indev_set_group(s_indev, s_group);
    s_scr = lv_scr_act();
    show_main_menu();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "人脸菜单已启动（编码器滚选，按键确认）");
}
