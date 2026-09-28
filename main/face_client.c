#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_camera.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "face_client.h"

static const char *TAG = "face_client";

/* multipart/form-data 分隔符。与 Content-Type 里的 boundary 保持一致。 */
#define FACE_BOUNDARY "----ESP32FaceBoundary"
#define MAX_RESP 4096  /* 多人识别 /recognize_multi 最多返回 FACE_MULTI_MAX 张脸，JSON 可能超 1KB，需放大避免截断 */

/* 电脑端返回的响应体缓存在这里（JSON 很小，静态缓冲够用）。 */
static char g_resp_buf[MAX_RESP];
static int  g_resp_len = 0;

/* 缓存最近一次从 HTTP 请求里学到的电脑端 IP。TFT/编码器流程没有"发起方"，就靠它上报。 */
static char g_cached_host[64] = FACE_PC_IP;

const char *face_client_get_cached_host(void)
{
    return g_cached_host;
}

void face_client_set_cached_host(const char *host)
{
    if (host && host[0]) {
        strncpy(g_cached_host, host, sizeof(g_cached_host) - 1);
        g_cached_host[sizeof(g_cached_host) - 1] = 0;
    }
}

/* 返回最近一次电脑端响应的 msg 字段（如"未检测到人脸"/"录入成功: 张三"），无则 ""。 */
const char *face_client_last_msg(void)
{
    static char msg[64];
    msg[0] = 0;
    cJSON *root = cJSON_Parse(g_resp_buf);
    if (root) {
        cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "msg");
        if (cJSON_IsString(m)) {
            strncpy(msg, m->valuestring, sizeof(msg) - 1);
        }
        cJSON_Delete(root);
    }
    return msg;
}

/* 人脸录入/识别网页：内嵌 /stream 实时预览 + 10 秒倒计时，到点才拍照。
 * 这样录入/识别前能先看镜头、调整姿势，不会再"一闪而过"。
 * 页面零外部依赖（ESP32 热点上没有公网），全部内联。 */
static const char FACE_PAGE_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang='zh'><head><meta charset='utf-8'>\n"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
    "<title>人脸录入 / 识别</title>\n"
    "<style>\n"
    "body{margin:0;background:#111;color:#eee;font-family:system-ui,sans-serif}\n"
    "img{width:100%;display:block;background:#000}\n"
    ".wrap{padding:12px}\n"
    "input{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border-radius:6px;border:1px solid #555;background:#222;color:#eee;margin:8px 0}\n"
    ".row{display:flex;gap:8px}\n"
    "button{flex:1;padding:12px;font-size:16px;border-radius:6px;border:0;background:#2e7d32;color:#fff}\n"
    "button.rec{background:#1565c0}\n"
    "#cd{display:none;position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);font-size:140px;font-weight:bold;color:#fff;text-shadow:0 0 24px #f00;pointer-events:none}\n"
    "#result{margin:8px 0;padding:10px;border-radius:6px;background:#222;min-height:20px;white-space:pre-wrap;word-break:break-all}\n"
    "</style></head><body>\n"
    "<img src='/stream'>\n"
    "<div id='cd'></div>\n"
    "<div class='wrap'>\n"
    "<input id='name' placeholder='输入姓名，如 张三'>\n"
    "<div class='row'>\n"
    "<button onclick=\"run('enroll')\">录入（10秒倒计时）</button>\n"
    "<button class='rec' onclick=\"run('recognize')\">识别（10秒倒计时）</button>\n"
    "</div>\n"
    "<div id='result'>点上方按钮开始。倒计时期间请正对镜头调整姿势，到 1 结束自动拍照。</div>\n"
    "</div>\n"
    "<script>\n"
    "function sleep(ms){return new Promise(function(r){setTimeout(r,ms)})}\n"
    "async function run(kind){\n"
    "  var name=document.getElementById('name').value.trim()\n"
    "  if(kind==='enroll' && !name){alert('请先输入姓名');return}\n"
    "  var cd=document.getElementById('cd'),res=document.getElementById('result')\n"
    "  cd.style.display='block';res.textContent='准备拍照，请看镜头…'\n"
    "  for(var i=10;i>=1;i--){cd.textContent=i;await sleep(1000)}\n"
    "  cd.style.display='none';res.textContent='拍照中…'\n"
    "  try{\n"
    "    var url=kind==='enroll'?('/enroll?name='+encodeURIComponent(name)):'/recognize'\n"
    "    var r=await fetch(url);res.textContent=await r.text()\n"
    "  }catch(e){res.textContent='请求失败: '+e}\n"
    "}\n"
    "</script></body></html>\n";

/* ===== 采集一帧 JPEG：拷贝后立即归还帧缓冲，避免长时间占用影响 /stream 推流 ===== */
static esp_err_t face_capture_jpeg(uint8_t **jpeg_out, size_t *len_out)
{
    camera_fb_t *fb = NULL;
    // 摄像头只有 2 个帧缓冲，推流任务会占用；这里重试几次避开争抢
    for (int i = 0; i < 10; i++) {
        fb = esp_camera_fb_get();
        if (fb) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!fb) {
        ESP_LOGE(TAG, "采集失败：拿不到帧缓冲（可能被推流持续占用）");
        return ESP_FAIL;
    }

    if (fb->format != PIXFORMAT_JPEG) {
        // 兜底：个别配置下摄像头输出非 JPEG，现场转一次
        bool ok = frame2jpg(fb, 80, jpeg_out, len_out);
        esp_camera_fb_return(fb);
        if (!ok) {
            ESP_LOGE(TAG, "RGB->JPEG 转换失败");
        }
        return ok ? ESP_OK : ESP_FAIL;
    }

    uint8_t *copy = malloc(fb->len);
    if (!copy) {
        esp_camera_fb_return(fb);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, fb->buf, fb->len);
    *jpeg_out = copy;
    *len_out = fb->len;
    esp_camera_fb_return(fb);
    return ESP_OK;
}

/* ===== 收集 HTTP 响应体到 g_resp_buf ===== */
static esp_err_t face_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (g_resp_len + evt->data_len < MAX_RESP - 1) {
            memcpy(g_resp_buf + g_resp_len, evt->data, evt->data_len);
            g_resp_len += evt->data_len;
            g_resp_buf[g_resp_len] = 0;
        }
    }
    return ESP_OK;
}

/* ===== 构造 multipart/form-data 请求体（name 为空则只带 image 字段） ===== */
static esp_err_t face_build_body(const char *name, const uint8_t *jpeg, size_t jpeg_len,
                                 uint8_t **body_out, size_t *body_len_out)
{
    char head[512];
    int head_len;
    const char *footer = "\r\n--" FACE_BOUNDARY "--\r\n";
    size_t footer_len = strlen(footer);

    if (name && name[0]) {
        head_len = snprintf(head, sizeof(head),
                            "--" FACE_BOUNDARY "\r\n"
                            "Content-Disposition: form-data; name=\"name\"\r\n"
                            "\r\n"
                            "%s"
                            "\r\n--" FACE_BOUNDARY "\r\n"
                            "Content-Disposition: form-data; name=\"image\"; filename=\"face.jpg\"\r\n"
                            "Content-Type: image/jpeg\r\n"
                            "\r\n",
                            name);
    } else {
        head_len = snprintf(head, sizeof(head),
                            "--" FACE_BOUNDARY "\r\n"
                            "Content-Disposition: form-data; name=\"image\"; filename=\"face.jpg\"\r\n"
                            "Content-Type: image/jpeg\r\n"
                            "\r\n");
    }

    size_t total = (size_t)head_len + jpeg_len + footer_len;
    uint8_t *body = malloc(total);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(body, head, head_len);
    memcpy(body + head_len, jpeg, jpeg_len);
    memcpy(body + head_len + jpeg_len, footer, footer_len);
    *body_out = body;
    *body_len_out = total;
    return ESP_OK;
}

/* ===== 把 JPEG POST 到电脑端，返回 HTTP 状态码；响应体在 g_resp_buf ===== */
static esp_err_t face_post(const char *host, const char *path, const char *name,
                           const uint8_t *jpeg, size_t jpeg_len, int *status_out, int timeout_ms)
{
    uint8_t *body = NULL;
    size_t body_len = 0;
    esp_err_t ret = face_build_body(name, jpeg, jpeg_len, &body, &body_len);
    if (ret != ESP_OK) {
        return ret;
    }

    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, FACE_PC_PORT, path);

    g_resp_len = 0;
    g_resp_buf[0] = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms,
        .event_handler = face_http_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        return ESP_FAIL;
    }

    char ctype[64];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=" FACE_BOUNDARY);
    esp_http_client_set_header(client, "Content-Type", ctype);
    esp_http_client_set_post_field(client, (const char *)body, (int)body_len);

    ret = esp_http_client_perform(client);
    if (status_out) {
        *status_out = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    free(body);
    return ret;
}

/* ===== 解析电脑端 /recognize 返回的 JSON =====
 *   {"code":0,"matched":true,"name":"张三","similarity":1.0}
 *   {"code":-1,"msg":"未检测到人脸"}                     */
static esp_err_t face_parse_recognize(face_result_t *out)
{
    cJSON *root = cJSON_Parse(g_resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "解析响应 JSON 失败: %s", g_resp_buf);
        return ESP_FAIL;
    }

    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsNumber(code) && code->valueint == 0) {
        cJSON *matched = cJSON_GetObjectItemCaseSensitive(root, "matched");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *sim = cJSON_GetObjectItemCaseSensitive(root, "similarity");
        out->detected = true;
        out->matched = cJSON_IsTrue(matched);
        out->name[0] = 0;
        if (cJSON_IsString(name)) {
            strncpy(out->name, name->valuestring, sizeof(out->name) - 1);
        }
        out->similarity = cJSON_IsNumber(sim) ? (float)sim->valuedouble : 0.0f;

        /* 人脸框 [x, y, w, h]（摄像头输出坐标系，QVGA 320x240），供实时画绿/红框 */
        out->has_box = false;
        cJSON *box = cJSON_GetObjectItemCaseSensitive(root, "box");
        if (cJSON_IsArray(box)) {
            for (int i = 0; i < 4 && i < cJSON_GetArraySize(box); i++) {
                cJSON *n = cJSON_GetArrayItem(box, i);
                if (cJSON_IsNumber(n)) out->box[i] = n->valueint;
            }
            out->has_box = true;
        }
    } else {
        // code == -1：通常是"未检测到人脸"或"缺少 image"
        out->detected = false;
        out->matched = false;
        out->name[0] = 0;
        out->similarity = 0.0f;
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        if (cJSON_IsString(msg)) {
            strncpy(out->msg, msg->valuestring, sizeof(out->msg) - 1);
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t face_client_recognize_frame(const char *host, const uint8_t *jpeg,
                                      size_t jpeg_len, face_result_t *out)
{
    memset(out, 0, sizeof(*out));

    int status = 0;
    /* 预览画框用短超时：PC 不可达时快速失败，回退本地取景框，别卡住预览循环 */
    esp_err_t err = face_post(host, "/recognize", NULL, jpeg, jpeg_len, &status, 1500);
    if (err != ESP_OK) {
        return err;
    }
    return face_parse_recognize(out);
}

/* ===== 解析 /recognize_multi 返回:
 *   {"code":0,"faces":[{"box":[x,y,w,h],"matched":true,"name":"张三","similarity":0.9}, ...]} ===== */
static esp_err_t face_parse_recognize_multi(face_multi_result_t *out)
{
    cJSON *root = cJSON_Parse(g_resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "解析 /recognize_multi 响应失败: %s", g_resp_buf);
        return ESP_FAIL;
    }
    cJSON *faces = cJSON_GetObjectItemCaseSensitive(root, "faces");
    if (cJSON_IsArray(faces)) {
        int n = cJSON_GetArraySize(faces);
        if (n > FACE_MULTI_MAX) n = FACE_MULTI_MAX;
        for (int i = 0; i < n; i++) {
            cJSON *f = cJSON_GetArrayItem(faces, i);
            if (!cJSON_IsObject(f)) continue;
            face_multi_item_t *it = &out->items[out->count];

            cJSON *box = cJSON_GetObjectItemCaseSensitive(f, "box");
            if (cJSON_IsArray(box)) {
                for (int k = 0; k < 4 && k < cJSON_GetArraySize(box); k++) {
                    cJSON *v = cJSON_GetArrayItem(box, k);
                    if (cJSON_IsNumber(v)) it->box[k] = v->valueint;
                }
            }
            cJSON *m = cJSON_GetObjectItemCaseSensitive(f, "matched");
            it->matched = cJSON_IsTrue(m);
            it->name[0] = 0;
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(f, "name");
            if (cJSON_IsString(nm)) strncpy(it->name, nm->valuestring, sizeof(it->name) - 1);
            cJSON *sim = cJSON_GetObjectItemCaseSensitive(f, "similarity");
            it->similarity = cJSON_IsNumber(sim) ? (float)sim->valuedouble : 0.0f;
            out->count++;
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t face_client_recognize_multi_frame(const char *host, const uint8_t *jpeg,
                                            size_t jpeg_len, face_multi_result_t *out)
{
    memset(out, 0, sizeof(*out));
    int status = 0;
    /* 多人实时识别：短超时快速失败，PC 不可达时不阻塞整帧循环 */
    esp_err_t err = face_post(host, "/recognize_multi", NULL, jpeg, jpeg_len, &status, 1200);
    if (err != ESP_OK) {
        return err;
    }
    return face_parse_recognize_multi(out);
}

esp_err_t face_client_recognize(const char *host, face_result_t *out)
{
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    esp_err_t err = face_capture_jpeg(&jpeg, &jpeg_len);
    if (err != ESP_OK) {
        return err;
    }

    int status = 0;
    err = face_post(host, "/recognize", NULL, jpeg, jpeg_len, &status, 5000);
    free(jpeg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST %s/recognize 失败: %s（请确认电脑端 face_server.py 已启动、"
                 "防火墙放行 5000 端口、且电脑连着 ESP32 热点）", host, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "电脑端 /recognize 返回 HTTP %d: %s", status, g_resp_buf);
    return face_parse_recognize(out);
}

esp_err_t face_client_enroll(const char *host, const char *name)
{
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    esp_err_t err = face_capture_jpeg(&jpeg, &jpeg_len);
    if (err != ESP_OK) {
        return err;
    }

    int status = 0;
    err = face_post(host, "/enroll", name, jpeg, jpeg_len, &status, 5000);
    free(jpeg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST %s/enroll 失败: %s", host, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "电脑端 /enroll 返回 HTTP %d: %s", status, g_resp_buf);
    return (status == 200) ? ESP_OK : ESP_FAIL;
}

/* ===== 从 HTTP 请求里取出发起请求的客户端（电脑）IP ===== */
esp_err_t face_client_get_ip(httpd_req_t *req, char *ip, size_t ip_len)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return ESP_FAIL;
    }
    struct sockaddr_storage ss;
    socklen_t ss_len = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &ss_len) != 0) {
        return ESP_FAIL;
    }
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        const char *s = inet_ntoa(a->sin_addr);
        if (!s) {
            return ESP_FAIL;
        }
        strncpy(ip, s, ip_len - 1);
        ip[ip_len - 1] = 0;
        strncpy(g_cached_host, s, sizeof(g_cached_host) - 1);   // 记住电脑 IP
        g_cached_host[sizeof(g_cached_host) - 1] = 0;
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* ===== URL 百分号解码（浏览器会把中文 name 编码成 %E5%BC%A0...） ===== */
static void url_decode(const char *src, char *dst, size_t dst_len)
{
    char *d = dst;
    const char *s = src;
    while (*s && (size_t)(d - dst) < dst_len - 1) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *d++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *d++ = *s++;
        }
    }
    *d = 0;
}

/* ===== URL 百分号编码（把中文姓名编码成 %E5%BC%A0...，拼进 GET/DELETE 查询串） ===== */
static void url_encode(const char *src, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789ABCDEF";
    char *d = dst;
    const char *s = src;
    while (*s && (size_t)(d - dst) < dst_len - 4) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *d++ = (char)c;
        } else {
            *d++ = '%';
            *d++ = hex[c >> 4];
            *d++ = hex[c & 0x0F];
        }
        s++;
    }
    *d = 0;
}

/* ===== 通用 GET/DELETE 请求：响应体原样收进调用方缓冲（头像 JPEG 是二进制） ===== */
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t max;
} face_get_buf_t;

static esp_err_t face_get_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        face_get_buf_t *b = (face_get_buf_t *)evt->user_data;
        if (b && b->buf && b->len + evt->data_len < b->max) {
            memcpy(b->buf + b->len, evt->data, evt->data_len);
            b->len += evt->data_len;
            b->buf[b->len] = 0;
        }
    }
    return ESP_OK;
}

static esp_err_t face_http_get(const char *host, const char *path,
                               esp_http_client_method_t method,
                               uint8_t *buf, size_t max, size_t *len_out,
                               int *status_out, int timeout_ms)
{
    char url[192];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, FACE_PC_PORT, path);

    face_get_buf_t b = { .buf = buf, .len = 0, .max = max };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .event_handler = face_get_event,
        .user_data = &b,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }
    esp_err_t ret = esp_http_client_perform(client);
    if (status_out) {
        *status_out = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    if (len_out) {
        *len_out = b.len;
    }
    return ret;
}

esp_err_t face_client_list_faces(const char *host, char names[][32], int max, int *count)
{
    *count = 0;
    static uint8_t buf[2048];   // 姓名列表 JSON（中文会被 \uXXXX 转义，2KB 足够十几个人）
    size_t len = 0;
    int status = 0;
    esp_err_t err = face_http_get(host, "/faces", HTTP_METHOD_GET,
                                  buf, sizeof(buf), &len, &status, 3000);
    if (err != ESP_OK || status != 200) {
        return (err == ESP_OK) ? ESP_FAIL : err;
    }
    cJSON *root = cJSON_Parse((const char *)buf);
    if (!root) {
        return ESP_FAIL;
    }
    cJSON *faces = cJSON_GetObjectItemCaseSensitive(root, "faces");
    int n = 0;
    if (cJSON_IsArray(faces)) {
        n = cJSON_GetArraySize(faces);
        if (n > max) n = max;
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(faces, i);
            if (cJSON_IsString(it)) {
                strncpy(names[i], it->valuestring, 31);
                names[i][31] = 0;
            }
        }
    }
    cJSON_Delete(root);
    *count = n;
    return ESP_OK;
}

esp_err_t face_client_get_avatar(const char *host, const char *name,
                                 uint8_t *buf, size_t max, size_t *len_out)
{
    char enc[128];
    url_encode(name, enc, sizeof(enc));
    char path[160];
    snprintf(path, sizeof(path), "/avatar?name=%s", enc);

    int status = 0;
    esp_err_t err = face_http_get(host, path, HTTP_METHOD_GET,
                                  buf, max, len_out, &status, 3000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "头像拉取失败 name=%.24s err=%s", name, esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        /* 404 = 电脑端没有这个人的头像文件（多为改动前录入、或服务未重启） */
        ESP_LOGW(TAG, "头像 HTTP %d name=%.24s（需重新录入一次生成头像）", status, name);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t face_client_delete_face(const char *host, const char *name)
{
    char enc[128];
    url_encode(name, enc, sizeof(enc));
    char path[160];
    snprintf(path, sizeof(path), "/faces?name=%s", enc);

    uint8_t buf[128];
    size_t len = 0;
    int status = 0;
    esp_err_t err = face_http_get(host, path, HTTP_METHOD_DELETE,
                                  buf, sizeof(buf), &len, &status, 3000);
    if (err != ESP_OK) {
        return err;
    }
    return (status == 200) ? ESP_OK : ESP_FAIL;
}

/* ===== GET /recognize：触发一次识别 ===== */
static esp_err_t face_recognize_handler(httpd_req_t *req)
{
    // 直接回 POST 给发起请求的这台电脑，省得猜 IP
    char host[64] = FACE_PC_IP;
    face_client_get_ip(req, host, sizeof(host));

    face_result_t r;
    esp_err_t err = face_client_recognize(host, &r);

    char resp[256];
    if (err != ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"code\":-1,\"msg\":\"识别失败: %s\"}", esp_err_to_name(err));
    } else if (!r.detected) {
        snprintf(resp, sizeof(resp),
                 "{\"code\":0,\"matched\":false,\"name\":\"\",\"similarity\":0,\"msg\":\"%s\"}",
                 r.msg[0] ? r.msg : "未检测到人脸");
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"code\":0,\"matched\":%s,\"name\":\"%s\",\"similarity\":%.3f}",
                 r.matched ? "true" : "false",
                 r.matched ? r.name : "",
                 r.similarity);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* ===== GET /enroll?name=张三：触发一次录入 ===== */
static esp_err_t face_enroll_handler(httpd_req_t *req)
{
    char host[64] = FACE_PC_IP;
    face_client_get_ip(req, host, sizeof(host));

    char query[256];
    char param[64];
    char name[64] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "name", param, sizeof(param)) == ESP_OK) {
            url_decode(param, name, sizeof(name));
        }
    }

    char resp[256];
    if (!name[0]) {
        snprintf(resp, sizeof(resp), "{\"code\":-1,\"msg\":\"缺少 name 参数，示例 /enroll?name=张三\"}");
    } else {
        esp_err_t err = face_client_enroll(host, name);
        if (err == ESP_OK) {
            snprintf(resp, sizeof(resp), "{\"code\":0,\"msg\":\"录入成功: %s\"}", name);
        } else {
            snprintf(resp, sizeof(resp), "{\"code\":-1,\"msg\":\"录入失败: %s\"}", esp_err_to_name(err));
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* ===== GET /snapshot：返回当前抓到的 JPEG，方便肉眼确认图像是否正常 ===== */
static esp_err_t face_snapshot_handler(httpd_req_t *req)
{
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    esp_err_t err = face_capture_jpeg(&jpeg, &jpeg_len);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "采集失败");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t r = httpd_resp_send(req, (const char *)jpeg, jpeg_len);
    free(jpeg);
    return r;
}

/* ===== GET /face：人脸录入/识别网页（实时预览 + 倒计时） ===== */
static esp_err_t face_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, FACE_PAGE_HTML, strlen(FACE_PAGE_HTML));
}

void face_http_register(httpd_handle_t server)
{
    httpd_uri_t uri_recognize = {
        .uri = "/recognize",
        .method = HTTP_GET,
        .handler = face_recognize_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &uri_recognize);

    httpd_uri_t uri_enroll = {
        .uri = "/enroll",
        .method = HTTP_GET,
        .handler = face_enroll_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &uri_enroll);

    httpd_uri_t uri_snapshot = {
        .uri = "/snapshot",
        .method = HTTP_GET,
        .handler = face_snapshot_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &uri_snapshot);

    httpd_uri_t uri_page = {
        .uri = "/face",
        .method = HTTP_GET,
        .handler = face_page_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &uri_page);

    ESP_LOGI(TAG, "已注册人脸接口: /face(录入页), GET /recognize, GET /enroll?name=xxx, GET /snapshot");
}
