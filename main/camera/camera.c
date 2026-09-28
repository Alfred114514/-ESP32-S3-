#include <camera.h>
#include <stdio.h>
#include <led/led.h>
#include <esp_log.h>
#include <freertos/queue.h>
static const char *TAG = "example";

static QueueHandle_t xQueuePhotoFrame = NULL;
int if_camer_show = 1;
int to_photo_flag = 0;

void bsp_camera_init(void)
{
    camera_config_t cam_config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_sccb_sda = CAM_PIN_SIOD,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VYSNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .ledc_timer = LEDC_TIMER_1,
        .ledc_channel = LEDC_CHANNEL_1,
        .xclk_freq_hz = 10000000,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,  // 320x240（提帧率用；要更清晰可改回 VGA 640x480）
        .jpeg_quality = 20,            // 0~63，越小画质越好但文件越大、帧率越低；20 兼顾帧率与识别精度
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .sccb_i2c_port = 1,

    };
    // sensor_t * s = esp_camera_sensor_get();
    // s->set_vflip(s, 1);      // 视需要翻转
    // s->set_hmirror(s, 1);    // 水平镜像
    // s->set_brightness(s, 1); // 调整亮度
    esp_err_t err = esp_camera_init(&cam_config);
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "Camera init failed with error 0x%x", err);
    }
    // 详细错误处理
}

// // 在esp_camera_init()后添加
// static uint32_t get_max_file_number(void)
// {
//     DIR *dir = opendir("/sdcard");
//     if (!dir)
//     {
//         ESP_LOGE(TAG, "Failed to open directory");
//         return 0;
//     }

//     uint32_t max_number = 0;
//     struct dirent *entry;
//     while ((entry = readdir(dir)) != NULL)
//     {
//         ESP_LOGI(TAG, "Processing file: %s", entry->d_name);

//         // 检查文件名长度
//         size_t name_len = strlen(entry->d_name);
//         if (name_len != 12)
//         { // 改为12，因为PHOTO000.JPG是12个字符
//             ESP_LOGD(TAG, "Skipping file with wrong length: %s (%d)", entry->d_name, name_len);
//             continue;
//         }

//         // 检查前缀
//         if (memcmp(entry->d_name, "PHOTO", 5) != 0)
//         {
//             ESP_LOGD(TAG, "Skipping file without PHOTO prefix: %s", entry->d_name);
//             continue;
//         }

//         // 检查扩展名
//         if (memcmp(entry->d_name + 8, ".JPG", 4) != 0)
//         {
//             ESP_LOGD(TAG, "Skipping non-JPG file: %s", entry->d_name);
//             continue;
//         }

//         // 提取数字部分
//         char num_str[4] = {0};
//         memcpy(num_str, entry->d_name + 5, 3);

//         uint32_t current_number;
//         if (sscanf(num_str, "%lu", &current_number) == 1)
//         {
//             ESP_LOGI(TAG, "Found valid photo number: %lu from file %s", current_number, entry->d_name);
//             if (current_number > max_number)
//             {
//                 max_number = current_number;
//                 ESP_LOGI(TAG, "New maximum number: %lu", max_number);
//             }
//         }
//         else
//         {
//             ESP_LOGW(TAG, "Failed to parse number from: %s", num_str);
//         }
//     }

//     closedir(dir);
//     ESP_LOGI(TAG, "Final maximum file number found: %lu", max_number);
//     return max_number;
// }

// void camera_capture()
// {
//      char filename[32];               // 文件名缓冲区
//   camera_fb_t *fb = esp_camera_fb_get();
//       static uint32_t file_number = 0; // 文件编号
//     // 获取SD卡中最大的文件编号
//     file_number = get_max_file_number() + 1;
//     ESP_LOGI(TAG, "Starting file number: %lu", file_number);

// if (!fb) {
//     ESP_LOGE("CAM", "Failed to capture image");
// } else {
//     ESP_LOGI("CAM", "Image captured: %d bytes", fb->len);
//     if(fb)
//     {
//  snprintf(filename, sizeof(filename), "/sdcard/PHOTO%03lu.JPG",file_number++);

//                  {
//   // 分配JPEG缓冲区
//                 uint8_t *jpeg_buf = NULL;
//                 size_t jpeg_len = 0;

//                 // 将frame转换为JPEG
//                 bool converted = frame2jpg(fb,      // 输入frame
//                                            92,         // JPEG质量(0-100)
//                                            &jpeg_buf,  // 输出JPEG缓冲区
//                                            &jpeg_len); // 输出JPEG长度

//                 if (!converted)
//                 {
//                     ESP_LOGE(TAG, "JPEG conversion failed");
//                     esp_camera_fb_return(fb);
//                 }
//                 else
//                 {
//                      FILE *file = fopen(filename, "wb");
//                      if (file)
//                  {
//                     size_t written = fwrite(jpeg_buf, 1, jpeg_len, file);
//                     fclose(file);

//                     if (written == jpeg_len)
//                     {
//                         ESP_LOGI(TAG, "Saved %s (%d bytes)", filename, jpeg_len);
//                     }
//                     else
//                     {
//                         ESP_LOGE(TAG, "File write failed: %d/%d bytes", written, jpeg_len);
//                     }
//                     // 清理资源
//                    free(jpeg_buf);              // 释放JPEG缓冲区
//                    esp_camera_fb_return(fb); // 返回帧缓冲区
//                  }

//                 }
//     }
// }
//     esp_camera_fb_return(fb);
// }
// }
// static void task_save_photo(void *arg)
// {
//     camera_fb_t *frame = NULL;
//     char filename[32];               // 文件名缓冲区
//     static uint32_t file_number = 0; // 文件编号
//     // 获取SD卡中最大的文件编号
//     file_number =1 + 1;
//     ESP_LOGI(TAG, "Starting file number: %lu", file_number);
//     // // 在task_save_photo开始时添加
//     // ESP_LOGI(TAG, "Listing all files in /sdcard:");
//     // DIR *dir = opendir("/sdcard");
//     // if (dir)
//     // {
//     //     struct dirent *entry;
//     //     while ((entry = readdir(dir)) != NULL)
//     //     {
//     //         ESP_LOGI(TAG, "File: %s", entry->d_name);
//     //     }
//     //     closedir(dir);
//     // }

//     while (true)
//     {
//         if (xQueueReceive(xQueuePhotoFrame, &frame, portMAX_DELAY))
//         {
//             if (frame)
//             {
//                 // 修正格式化字符串，使用%03lu来匹配uint32_t类型
//                 snprintf(filename, sizeof(filename), "/sdcard/PHOTO%03lu.JPG", file_number++);

//                 if (access(filename, F_OK) == 0)
//                 {
//                     ESP_LOGW(TAG, "File %s already exists, skipping to next number", filename);
//                     continue;
//                 }

//                 // 分配JPEG缓冲区
//                 uint8_t *jpeg_buf = NULL;
//                 size_t jpeg_len = 0;

//                 // 将frame转换为JPEG
//                 bool converted = frame2jpg(frame,      // 输入frame
//                                            92,         // JPEG质量(0-100)
//                                            &jpeg_buf,  // 输出JPEG缓冲区
//                                            &jpeg_len); // 输出JPEG长度

//                 if (!converted)
//                 {
//                     ESP_LOGE(TAG, "JPEG conversion failed");
//                     esp_camera_fb_return(frame);
//                     continue;
//                 }

//                 // 保存JPEG文件
//                 FILE *file = fopen(filename, "wb");
//                 if (file)
//                 {
//                     size_t written = fwrite(jpeg_buf, 1, jpeg_len, file);
//                     fclose(file);

//                     if (written == jpeg_len)
//                     {
//                         ESP_LOGI(TAG, "Saved %s (%d bytes)", filename, jpeg_len);
//                     }
//                     else
//                     {
//                         ESP_LOGE(TAG, "File write failed: %d/%d bytes", written, jpeg_len);
//                     }
//                 }
//                 else
//                 {
//                     ESP_LOGE(TAG, "Failed to open file: %s", filename);
//                 }

//                 // 清理资源
//                 free(jpeg_buf);              // 释放JPEG缓冲区
//                 esp_camera_fb_return(frame); // 返回帧缓冲区
//             }
//         }
//     }
// }

// // 摄像头处理任务
// static void task_process_camera(void *arg)
// {
//     while (true)
//     {
//         camera_fb_t *frame = esp_camera_fb_get();
//         if (frame)
//             xQueueSend(xQueueLCDFrame, &frame, portMAX_DELAY);
//     }
// }