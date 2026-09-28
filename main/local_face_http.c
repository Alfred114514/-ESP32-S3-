#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "local_face_recog.h"
#include "local_face_http.h"

static const char *TAG = "local_face_http";

#define LOCAL_HTTP_MAX_FACES 32
#define LOCAL_BODY_MAX       (128 * 1024)   /* QVGA JPEG 通常 20~40KB，128KB 足够 */

/* 接收整个请求体到 SPIRAM 缓冲，返回实际长度（0 = 失败）。 */
static size_t recv_body(httpd_req_t *req, uint8_t *buf, size_t max)
{
    size_t total = 0;
    int remaining = req->content_len;
    while (remaining > 0 && total < max) {
        int want = (remaining < (int)(max - total)) ? remaining : (int)(max - total);
        int r = httpd_req_recv(req, (char *)buf + total, want);
        if (r <= 0) {
            break;
        }
        total += r;
        remaining -= r;
    }
    return total;
}

/* 极简 multipart/form-data 解析：抽出 name 与 image 两个字段。
 * 返回的指针指向 body 内部（body 存活期间有效）。我们和 local_db.py 两端自控，格式固定。 */
static void multipart_parse(const char *boundary, const char *body, size_t len,
                            const char **name_out, size_t *name_len,
                            const char **img_out, size_t *img_len)
{
    char delim[96];
    snprintf(delim, sizeof(delim), "--%s", boundary);
    size_t dlen = strlen(delim);

    *name_out = NULL; *name_len = 0;
    *img_out = NULL;  *img_len = 0;

    const char *end = body + len;
    const char *p = body;
    while (p + dlen <= end) {
        if (memcmp(p, delim, dlen) != 0) { p++; continue; }

        /* 找到下一个 boundary（作为本 part 的结束） */
        const char *next = end;
        for (const char *q = p + dlen; q + dlen + 2 <= end; q++) {
            if (q[0] == '\r' && q[1] == '\n' && memcmp(q + 2, delim, dlen) == 0) {
                next = q + 2;
                break;
            }
        }

        /* 找到头部与内容的交界 "\r\n\r\n" */
        const char *hdr_start = p + dlen;
        const char *hdr_end = NULL;
        for (const char *q = hdr_start; q + 3 <= next; q++) {
            if (q[0] == '\r' && q[1] == '\n' && q[2] == '\r' && q[3] == '\n') {
                hdr_end = q;
                break;
            }
        }
        if (!hdr_end) { p = next; continue; }

        char hdr[256];
        size_t hlen = (size_t)(hdr_end - hdr_start);
        if (hlen > sizeof(hdr) - 1) hlen = sizeof(hdr) - 1;
        memcpy(hdr, hdr_start, hlen);
        hdr[hlen] = 0;

        char field[32] = {0};
        const char *np = strstr(hdr, "name=\"");
        if (np) {
            np += 6;
            const char *nq = strchr(np, '"');
            if (nq) {
                size_t fl = (size_t)(nq - np);
                if (fl < sizeof(field) - 1) { memcpy(field, np, fl); field[fl] = 0; }
            }
        }

        const char *bstart = hdr_end + 4;
        const char *bend = next;
        if (bend - bstart >= 2 && bend[-2] == '\r' && bend[-1] == '\n') bend -= 2;  /* 去掉 boundary 前的 CRLF */

        if (strcmp(field, "name") == 0) {
            *name_out = bstart; *name_len = (size_t)(bend - bstart);
        } else if (strcmp(field, "image") == 0) {
            *img_out = bstart; *img_len = (size_t)(bend - bstart);
        }

        p = next;
    }
}

/* 从 Content-Type 里抠出 boundary（若无则回退到默认值）。 */
static void get_boundary(httpd_req_t *req, char *out, size_t cap)
{
    strcpy(out, "----ESP32LocalFaceBoundary");   /* 与 local_db.py 的默认 boundary 一致 */
    char ctype[160] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK) return;
    const char *bp = strstr(ctype, "boundary=");
    if (!bp) return;
    bp += 9;
    const char *e = bp + strlen(bp);
    while (e > bp && (e[-1] == ';' || e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n')) e--;
    size_t n = (size_t)(e - bp);
    if (n > 0 && bp[0] == '"' && e[-1] == '"') { bp++; n -= 2; }  /* 去引号 */
    if (n > 0 && n < cap) { memcpy(out, bp, n); out[n] = 0; }
}

/* POST /local_enroll：multipart(name + image) → 本地录入 */
static esp_err_t local_enroll_handler(httpd_req_t *req)
{
    uint8_t *body = heap_caps_malloc(LOCAL_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }

    size_t len = recv_body(req, body, LOCAL_BODY_MAX);

    char boundary[64];
    get_boundary(req, boundary, sizeof(boundary));

    const char *name = NULL, *img = NULL;
    size_t name_len = 0, img_len = 0;
    multipart_parse(boundary, (const char *)body, len, &name, &name_len, &img, &img_len);

    char resp[256];
    if (!img || img_len == 0) {
        snprintf(resp, sizeof(resp), "{\"code\":-1,\"msg\":\"缺少 image 字段\"}");
    } else if (!name || name_len == 0) {
        snprintf(resp, sizeof(resp), "{\"code\":-1,\"msg\":\"缺少 name 字段\"}");
    } else {
        char nm[64];
        size_t nl = name_len < sizeof(nm) - 1 ? name_len : sizeof(nm) - 1;
        memcpy(nm, name, nl);
        nm[nl] = 0;

        esp_err_t err = local_face_enroll_jpeg((const uint8_t *)img, img_len, nm);
        if (err == ESP_OK) {
            snprintf(resp, sizeof(resp), "{\"code\":0,\"msg\":\"录入成功: %s\",\"count\":%d}",
                     nm, local_face_count());
        } else {
            const char *why = (err == ESP_ERR_NOT_FOUND) ? "未检测到人脸" :
                              (err == ESP_ERR_NO_MEM) ? "特征库已满" : "录入失败";
            snprintf(resp, sizeof(resp), "{\"code\":-1,\"msg\":\"%s\"}", why);
        }
    }

    free(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* GET /local_faces → {"code":0,"faces":["张三", ...]} */
static esp_err_t local_faces_handler(httpd_req_t *req)
{
    char names[LOCAL_HTTP_MAX_FACES][32];
    int count = 0;
    local_face_list(names, LOCAL_HTTP_MAX_FACES, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", 0);
    cJSON *arr = cJSON_AddArrayToObject(root, "faces");
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "构建响应失败");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, txt, strlen(txt));
    free(txt);
    return ESP_OK;
}

/* DELETE /local_faces?name=xxx */
static esp_err_t local_faces_delete_handler(httpd_req_t *req)
{
    char query[256];
    char param[128];
    char name[64] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "name", param, sizeof(param)) == ESP_OK) {
            /* 简单百分号解码（浏览器/脚本会把中文 name 编码成 %E5%BC%A0...） */
            char *d = name;
            const char *s = param;
            while (*s && (size_t)(d - name) < sizeof(name) - 1) {
                if (*s == '%' && s[1] && s[2]) {
                    char hex[3] = {s[1], s[2], 0};
                    *d++ = (char)strtol(hex, NULL, 16);
                    s += 3;
                } else {
                    *d++ = *s++;
                }
            }
            *d = 0;
        }
    }

    char resp[256];
    if (!name[0]) {
        snprintf(resp, sizeof(resp), "{\"code\":-1,\"msg\":\"缺少 name 参数\"}");
    } else if (local_face_delete(name) == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"code\":0,\"msg\":\"已删除: %s\"}", name);
    } else {
        snprintf(resp, sizeof(resp), "{\"code\":-1,\"msg\":\"未找到: %s\"}", name);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

void local_face_http_register(httpd_handle_t server)
{
    httpd_uri_t enroll = {
        .uri = "/local_enroll",
        .method = HTTP_POST,
        .handler = local_enroll_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &enroll);

    httpd_uri_t faces = {
        .uri = "/local_faces",
        .method = HTTP_GET,
        .handler = local_faces_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &faces);

    httpd_uri_t del = {
        .uri = "/local_faces",
        .method = HTTP_DELETE,
        .handler = local_faces_delete_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &del);

    ESP_LOGI(TAG, "已注册本地人脸接口: POST /local_enroll, GET /local_faces, DELETE /local_faces");
}
