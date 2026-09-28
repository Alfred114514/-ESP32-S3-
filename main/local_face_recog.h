#ifndef LOCAL_FACE_RECOG_H
#define LOCAL_FACE_RECOG_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MFN 特征维度（L2 归一化后的 float 向量长度），供调用方分配 feat 缓冲。 */
#define LOCAL_FACE_FEAT_LEN 512

/**
 * @brief 仅初始化 featdb 分区（幂等，可重复调用）。
 *        首次使用若分区未格式化会擦除 flash，而擦除 flash 会禁用 cache（PSRAM 栈会崩），
 *        所以这一部必须在内部 RAM 栈上做（由主任务先调）。之后模型加载任务可用 PSRAM 栈。
 */
esp_err_t local_face_init_nvs(void);

/**
 * @brief 初始化本地人脸识别：加载 ESP-DL 检测(MSRMNP)+特征(MFN) 模型，并初始化 featdb 分区。
 *        在 wifi_http_start() 之后、进入人脸菜单之前调用一次即可。
 *        模型加载任务栈已搬到 PSRAM，故本函数只做 flash 读，不做擦写。
 */
esp_err_t local_face_init(void);

/** 录入：从一张 JPEG 里检测最大人脸，提取 512 维特征（L2 归一化）写入 featdb。 */
esp_err_t local_face_enroll_jpeg(const uint8_t *jpeg, size_t len, const char *name);

/**
 * 录入拆两步（供「PSRAM 栈上提特征 + 内部栈上写 NVS」使用，绕开 PSRAM 栈碰 NVS 崩溃）：
 * 1) 提取特征：解码+检测+MFN，只做纯计算不碰 NVS，PSRAM 栈安全；feat_out 需 LOCAL_FACE_FEAT_LEN 个 float。
 * 2) 写库：把已提取特征写入 featdb 并刷新缓存，内部会碰 NVS，必须在内部 RAM 栈上调用。
 */
esp_err_t local_face_extract_feat_jpeg(const uint8_t *jpeg, size_t len, float *feat_out);
esp_err_t local_face_store_feat(const float *feat, const char *name);

/**
 * 识别：提取特征后与库内逐条比对（余弦相似度）。
 * 命中(>0.42)时返回 ESP_OK 并把姓名写入 name_out、相似度写入 sim_out；
 * 未命中返回 ESP_ERR_NOT_FOUND（sim_out 仍会填最佳相似度，供"未录入 x.xx"显示）。
 */
esp_err_t local_face_recognize_jpeg(const uint8_t *jpeg, size_t len,
                                    char *name_out, size_t cap, float *sim_out);

/** 实时识别版：额外回填最大人脸框 box[4]=[x1,y1,x2,y2]（输入图坐标系），供预览画框。
 *  返回值语义同 local_face_recognize_jpeg；未检测到人脸时 box 保持 0。 */
esp_err_t local_face_recognize_jpeg_box(const uint8_t *jpeg, size_t len,
                                        int box[4], char *name_out, size_t cap,
                                        float *sim_out);

/** 预览：检测 JPEG 里的人脸框（[x1,y1,x2,y2]，QVGA 320x240 坐标系）。 */
esp_err_t local_face_detect_jpeg(const uint8_t *jpeg, size_t len,
                                 int boxes[][4], int max, int *n);

/** 列出已录入姓名（names 每项最多 31 字节）。 */
esp_err_t local_face_list(char names[][32], int max, int *count);

/** 按姓名删除。 */
esp_err_t local_face_delete(const char *name);

/** 清空特征库。 */
esp_err_t local_face_clear(void);

/** 当前已录入人数。 */
int local_face_count(void);

#ifdef __cplusplus
}
#endif

#endif // LOCAL_FACE_RECOG_H
