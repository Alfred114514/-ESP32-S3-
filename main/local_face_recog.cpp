#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "dl_image_jpeg.hpp"
#include "dl_tensor_base.hpp"
#include "local_face_recog.h"

static const char *TAG = "local_face";

/* 特征库专用 NVS 分区（partitions.csv 里新增的 featdb，与 wifi_http.c 的默认 nvs 分区互不干扰） */
static const char *NVS_PART = "featdb";
static const char *NVS_NS   = "featdb";

/* MFN 输出 512 维 float；NVS 单条 blob 上限 4000B，2048B 一条放得下 */
#define FEAT_LEN 512
#define MAX_LOCAL_FACES 32
#define MATCH_THR 0.42f   /* 阈值略放宽：现场 QVGA 抓拍分辨率/噪声比录入照片低，0.42 更容易命中，仍能避开不同人(<0.3) */

static HumanFaceDetect *s_detect = nullptr;
static HumanFaceFeat    *s_feat   = nullptr;
static bool s_ready = false;

/* 全局互斥锁：串行化 ESP-DL 模型推理 + featdb(NVS) 多步读写。
 * HumanFaceDetect / HumanFaceFeat 非线程安全；识别是"推理→比对"连续流程，
 * 与 HTTP 录入并发会互相踩。各公开接口独立、互不嵌套，一把锁整体串行化即可。 */
static SemaphoreHandle_t s_mutex = NULL;

/* ---- 特征库缓存（PSRAM）----
 * 识别跑在 PSRAM 栈任务里，任何 NVS 读（esp_flash_read 会禁 cache）都会触发
 * esp_task_stack_is_sane_cache_disabled() 断言崩掉。故把 featdb 全部特征一次性载入
 * PSRAM 缓存，识别只读缓存；录入/删除/清空时同步更新 NVS + 缓存。缓存受 s_mutex 保护。 */
static float     *s_db_feats = NULL;   /* MAX_LOCAL_FACES x FEAT_LEN 个 float，PSRAM */
static char      (*s_db_names)[32] = NULL;  /* MAX_LOCAL_FACES x 32 字节姓名，PSRAM */
static int        s_db_count = 0;

struct ScopedLock {
    SemaphoreHandle_t m;
    explicit ScopedLock(SemaphoreHandle_t x) : m(x) { if (m) xSemaphoreTake(m, portMAX_DELAY); }
    ~ScopedLock() { if (m) xSemaphoreGive(m); }
};

/* ---- NVS 存取 ---- */

static esp_err_t nvs_open_db(nvs_handle_t *h)
{
    return nvs_open_from_partition(NVS_PART, NVS_NS, NVS_READWRITE, h);
}

/* 从 NVS 重建 PSRAM 缓存。内部会读 NVS（禁 cache），只能在内部 RAM 栈上调用。 */
static esp_err_t cache_reload(void)
{
    if (!s_db_feats) {
        s_db_feats = (float *)heap_caps_malloc(MAX_LOCAL_FACES * FEAT_LEN * sizeof(float), MALLOC_CAP_SPIRAM);
        s_db_names = (char (*)[32])heap_caps_malloc(MAX_LOCAL_FACES * 32, MALLOC_CAP_SPIRAM);
        if (!s_db_feats || !s_db_names) {
            ESP_LOGE(TAG, "特征库缓存分配失败");
            return ESP_ERR_NO_MEM;
        }
    }
    s_db_count = 0;
    memset(s_db_feats, 0, MAX_LOCAL_FACES * FEAT_LEN * sizeof(float));
    memset(s_db_names, 0, MAX_LOCAL_FACES * 32);

    nvs_handle_t h;
    if (nvs_open_db(&h) != ESP_OK) return ESP_ERR_INVALID_STATE;

    uint16_t cnt = 0;
    nvs_get_u16(h, "cnt", &cnt);
    if (cnt > MAX_LOCAL_FACES) cnt = MAX_LOCAL_FACES;

    char key[16];
    for (uint16_t i = 0; i < cnt; i++) {
        snprintf(key, sizeof(key), "f%u", i);
        size_t len = FEAT_LEN * sizeof(float);
        if (nvs_get_blob(h, key, &s_db_feats[i * FEAT_LEN], &len) != ESP_OK || len != FEAT_LEN * sizeof(float)) {
            memset(&s_db_feats[i * FEAT_LEN], 0, FEAT_LEN * sizeof(float));  /* 损坏条目置零 */
        }
        snprintf(key, sizeof(key), "n%u", i);
        size_t nl = 32;
        if (nvs_get_str(h, key, s_db_names[i], &nl) != ESP_OK) s_db_names[i][0] = 0;
    }
    s_db_count = cnt;
    nvs_close(h);
    ESP_LOGI(TAG, "特征库缓存已载入 %d 人", s_db_count);
    return ESP_OK;
}

/* ---- 解码 + 检测 ---- */

/* JPEG -> RGB888 img_t；成功时调用方负责 heap_caps_free(img->data) */
static esp_err_t decode_jpeg(const uint8_t *jpeg, size_t len, dl::image::img_t *img)
{
    dl::image::jpeg_img_t jp{(void *)jpeg, len};
    *img = dl::image::sw_decode_jpeg(jp, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (!img->data || img->width == 0 || img->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* 提取最大脸的 512 维特征（未归一化原始输出） */
static esp_err_t extract_feat(const dl::image::img_t &img,
                              const dl::detect::result_t &det, float *feat_out)
{
    dl::TensorBase *t = s_feat->run(img, det.keypoint);
    if (!t) {
        return ESP_FAIL;
    }
    int len = s_feat->get_feat_len();
    if (len != FEAT_LEN) {
        ESP_LOGE(TAG, "特征维度异常：%d（期望 %d）", len, FEAT_LEN);
        return ESP_FAIL;
    }
    const float *src = t->get_element_ptr<float>();
    memcpy(feat_out, src, FEAT_LEN * sizeof(float));
    return ESP_OK;
}

/* L2 归一化，使点积 == 余弦相似度（模型输出通常已归一，再归一一次无副作用） */
static void normalize(float *v, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; i++) {
        s += v[i] * v[i];
    }
    s = sqrtf(s);
    if (s > 1e-6f) {
        for (int i = 0; i < n; i++) {
            v[i] /= s;
        }
    }
}

/* ---- 对外接口 ---- */

esp_err_t local_face_init_nvs(void)
{
    esp_err_t err = nvs_flash_init_partition(NVS_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "featdb 分区需擦除重初始化");
        nvs_flash_erase_partition(NVS_PART);
        err = nvs_flash_init_partition(NVS_PART);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "featdb 分区初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "featdb 分区就绪");

    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();

    /* 载入特征库缓存（读 NVS，内部栈上安全）。之后识别只读缓存、不碰 NVS。 */
    return cache_reload();
}

esp_err_t local_face_init(void)
{
    ESP_LOGI(TAG, "开始本地人脸模型初始化（检测 MSRMNP + 特征 MFN）");

    /* NVS 初始化 + 特征库缓存载入已在主任务内部栈上完成（见 local_face_init_nvs）；
     * 这里只做纯 flash 读（模型权重走 XIP mmap，不经过 esp_flash_read，不禁 cache），PSRAM 栈安全。 */

    /* 模型一次加载进内存（lazy_load=false），后续推理复用 */
    ESP_LOGI(TAG, "加载检测模型 MSRMNP ...");
    s_detect = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
    ESP_LOGI(TAG, "检测模型 MSRMNP 加载完成");

    ESP_LOGI(TAG, "加载特征模型 MFN ...");
    s_feat   = new HumanFaceFeat(HumanFaceFeat::MFN_S8_V1, false);
    ESP_LOGI(TAG, "特征模型 MFN 加载完成");

    s_ready  = true;
    ESP_LOGI(TAG, "本地人脸模型加载完成（检测 MSRMNP + 特征 MFN，%d 维）", s_feat->get_feat_len());
    return ESP_OK;
}

/* 提取核心：解码 → 检测最大脸 → 提特征 → L2 归一化。不碰 NVS，PSRAM 栈安全。
 * 调用方须已持锁，feat_out 至少 FEAT_LEN 个 float。 */
static esp_err_t extract_feat_core(const uint8_t *jpeg, size_t len, float *feat_out)
{
    dl::image::img_t img;
    esp_err_t err = decode_jpeg(jpeg, len, &img);
    if (err != ESP_OK) return err;

    auto &res = s_detect->run(img);
    if (res.empty()) {
        heap_caps_free(img.data);
        ESP_LOGW(TAG, "录入未检测到人脸");
        return ESP_ERR_NOT_FOUND;
    }
    auto it = std::max_element(res.begin(), res.end(),
        [](const dl::detect::result_t &a, const dl::detect::result_t &b) {
            return a.box_area() < b.box_area();
        });

    err = extract_feat(img, *it, feat_out);
    heap_caps_free(img.data);
    if (err != ESP_OK) return err;
    normalize(feat_out, FEAT_LEN);
    return ESP_OK;
}

/* 存储核心：把特征写入 NVS 并刷新 PSRAM 缓存。碰 NVS，必须内部 RAM 栈调用。
 * 调用方须已持锁。 */
static esp_err_t store_feat_core(const float *feat, const char *name)
{
    nvs_handle_t h;
    if (nvs_open_db(&h) != ESP_OK) return ESP_ERR_INVALID_STATE;

    uint16_t count = 0;
    nvs_get_u16(h, "cnt", &count);
    if (count >= MAX_LOCAL_FACES) {
        ESP_LOGW(TAG, "特征库已满（%d 人上限）", MAX_LOCAL_FACES);
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }

    char key[16];
    snprintf(key, sizeof(key), "f%u", count);
    nvs_set_blob(h, key, feat, FEAT_LEN * sizeof(float));
    snprintf(key, sizeof(key), "n%u", count);
    nvs_set_str(h, key, name);
    nvs_set_u16(h, "cnt", (uint16_t)(count + 1));
    nvs_commit(h);
    nvs_close(h);

    cache_reload();   /* 同步 PSRAM 缓存 */

    ESP_LOGI(TAG, "录入成功: %s (id=%u, 共 %u 人)", name, count, (unsigned)(count + 1));
    return ESP_OK;
}

esp_err_t local_face_enroll_jpeg(const uint8_t *jpeg, size_t len, const char *name)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    ScopedLock lock(s_mutex);

    float feat[FEAT_LEN];
    esp_err_t err = extract_feat_core(jpeg, len, feat);
    if (err != ESP_OK) return err;
    return store_feat_core(feat, name);
}

esp_err_t local_face_extract_feat_jpeg(const uint8_t *jpeg, size_t len, float *feat_out)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    ScopedLock lock(s_mutex);
    return extract_feat_core(jpeg, len, feat_out);
}

esp_err_t local_face_store_feat(const float *feat, const char *name)
{
    ScopedLock lock(s_mutex);
    return store_feat_core(feat, name);
}

/* 识别核心：解码 → 检测最大脸 → 提特征 → 与库比对。box_out 非空时回填人脸框 [x1,y1,x2,y2]。 */
static esp_err_t recognize_impl(const uint8_t *jpeg, size_t len, int *box_out,
                                char *name_out, size_t cap, float *sim_out)
{
    if (name_out && cap) name_out[0] = 0;
    if (sim_out) *sim_out = 0.0f;
    if (box_out) memset(box_out, 0, 4 * sizeof(int));
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    ScopedLock lock(s_mutex);

    dl::image::img_t img;
    esp_err_t err = decode_jpeg(jpeg, len, &img);
    if (err != ESP_OK) return err;

    auto &res = s_detect->run(img);
    if (res.empty()) {
        heap_caps_free(img.data);
        return ESP_ERR_INVALID_ARG;   /* 未检测到人脸（区别于下方的"未录入"） */
    }
    auto it = std::max_element(res.begin(), res.end(),
        [](const dl::detect::result_t &a, const dl::detect::result_t &b) {
            return a.box_area() < b.box_area();
        });
    if (box_out) {
        box_out[0] = it->box[0]; box_out[1] = it->box[1];
        box_out[2] = it->box[2]; box_out[3] = it->box[3];
    }

    float feat[FEAT_LEN];
    err = extract_feat(img, *it, feat);
    heap_caps_free(img.data);
    if (err != ESP_OK) return err;
    normalize(feat, FEAT_LEN);

    /* 与 PSRAM 缓存比对（不碰 NVS，PSRAM 栈上安全） */
    float best = -2.0f;   /* 归一化点积 ∈ [-1,1] */
    int best_id = -1;
    for (int i = 0; i < s_db_count; i++) {
        const float *sv = &s_db_feats[i * FEAT_LEN];
        float dot = 0.0f;
        for (int k = 0; k < FEAT_LEN; k++) {
            dot += feat[k] * sv[k];
        }
        if (dot > best) {
            best = dot;
            best_id = i;
        }
    }

    if (sim_out) *sim_out = best;

    if (best_id >= 0 && best >= MATCH_THR) {
        if (name_out && cap) {
            strncpy(name_out, s_db_names[best_id], cap - 1);
            name_out[cap - 1] = 0;
        }
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t local_face_recognize_jpeg(const uint8_t *jpeg, size_t len,
                                    char *name_out, size_t cap, float *sim_out)
{
    return recognize_impl(jpeg, len, NULL, name_out, cap, sim_out);
}

esp_err_t local_face_recognize_jpeg_box(const uint8_t *jpeg, size_t len,
                                        int box[4], char *name_out, size_t cap,
                                        float *sim_out)
{
    return recognize_impl(jpeg, len, box, name_out, cap, sim_out);
}

esp_err_t local_face_detect_jpeg(const uint8_t *jpeg, size_t len,
                                 int boxes[][4], int max, int *n)
{
    *n = 0;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    ScopedLock lock(s_mutex);

    dl::image::img_t img;
    esp_err_t err = decode_jpeg(jpeg, len, &img);
    if (err != ESP_OK) return err;

    auto &res = s_detect->run(img);
    int i = 0;
    for (const auto &d : res) {
        if (i >= max) break;
        boxes[i][0] = d.box[0];
        boxes[i][1] = d.box[1];
        boxes[i][2] = d.box[2];
        boxes[i][3] = d.box[3];
        i++;
    }
    *n = i;
    heap_caps_free(img.data);
    return ESP_OK;
}

esp_err_t local_face_list(char names[][32], int max, int *count)
{
    *count = 0;
    ScopedLock lock(s_mutex);
    for (int i = 0; i < s_db_count && *count < max; i++) {
        strncpy(names[*count], s_db_names[i], 31);
        names[*count][31] = 0;
        (*count)++;
    }
    return ESP_OK;
}

esp_err_t local_face_delete(const char *name)
{
    ScopedLock lock(s_mutex);
    nvs_handle_t h;
    if (nvs_open_db(&h) != ESP_OK) return ESP_ERR_INVALID_STATE;

    uint16_t cnt = 0;
    nvs_get_u16(h, "cnt", &cnt);

    int target = -1;
    char key[16];
    for (uint16_t i = 0; i < cnt; i++) {
        snprintf(key, sizeof(key), "n%u", i);
        size_t nl = 0;
        if (nvs_get_str(h, key, NULL, &nl) == ESP_OK && nl > 0) {
            char *nm = (char *)malloc(nl);
            if (nm && nvs_get_str(h, key, nm, &nl) == ESP_OK && strcmp(nm, name) == 0) {
                target = (int)i;
                free(nm);
                break;
            }
            free(nm);
        }
    }
    if (target < 0) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }

    /* 交换-删除：把最后一条搬到 target 位置，再删掉最后一条，避免留空洞 */
    uint16_t last = (uint16_t)(cnt - 1);
    char kl[16], kt[16];
    if (target == last) {
        snprintf(kl, sizeof(kl), "f%u", last); nvs_erase_key(h, kl);
        snprintf(kl, sizeof(kl), "n%u", last); nvs_erase_key(h, kl);
    } else {
        uint8_t stored[FEAT_LEN * sizeof(float)];
        size_t stored_len = sizeof(stored);
        snprintf(kl, sizeof(kl), "f%u", last);
        snprintf(kt, sizeof(kt), "f%u", target);
        if (nvs_get_blob(h, kl, stored, &stored_len) == ESP_OK) {
            nvs_set_blob(h, kt, stored, stored_len);
        }

        snprintf(kl, sizeof(kl), "n%u", last);
        size_t nl = 0;
        if (nvs_get_str(h, kl, NULL, &nl) == ESP_OK && nl > 0) {
            char *nm = (char *)malloc(nl);
            if (nm && nvs_get_str(h, kl, nm, &nl) == ESP_OK) {
                snprintf(kt, sizeof(kt), "n%u", target);
                nvs_set_str(h, kt, nm);
            }
            free(nm);
        }

        snprintf(kl, sizeof(kl), "f%u", last); nvs_erase_key(h, kl);
        snprintf(kl, sizeof(kl), "n%u", last); nvs_erase_key(h, kl);
    }

    nvs_set_u16(h, "cnt", last);
    nvs_commit(h);
    nvs_close(h);
    cache_reload();   /* 同步 PSRAM 缓存 */
    ESP_LOGI(TAG, "已删除: %s", name);
    return ESP_OK;
}

esp_err_t local_face_clear(void)
{
    ScopedLock lock(s_mutex);
    nvs_handle_t h;
    if (nvs_open_db(&h) != ESP_OK) return ESP_ERR_INVALID_STATE;

    uint16_t cnt = 0;
    nvs_get_u16(h, "cnt", &cnt);

    char key[16];
    for (uint16_t i = 0; i < cnt; i++) {
        snprintf(key, sizeof(key), "f%u", i); nvs_erase_key(h, key);
        snprintf(key, sizeof(key), "n%u", i); nvs_erase_key(h, key);
    }
    nvs_set_u16(h, "cnt", 0);
    nvs_commit(h);
    nvs_close(h);
    cache_reload();   /* 同步 PSRAM 缓存 */
    ESP_LOGI(TAG, "特征库已清空");
    return ESP_OK;
}

int local_face_count(void)
{
    ScopedLock lock(s_mutex);
    return s_db_count;
}
