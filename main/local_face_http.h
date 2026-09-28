#ifndef LOCAL_FACE_HTTP_H
#define LOCAL_FACE_HTTP_H

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在现有 HTTP 服务器上注册本地人脸识别的下发/查询接口：
 *        POST   /local_enroll       multipart(name + image) 录入
 *        GET    /local_faces        列出已录入姓名
 *        DELETE /local_faces?name=x 删除
 */
void local_face_http_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif // LOCAL_FACE_HTTP_H
