# 电脑端人脸识别（OpenCV YuNet + SFace）

电脑端负责：**人脸录入**（存 128 维特征到 `faces.json`）+ **人脸识别**（比对判断是否已录入、是谁）。
ESP32 拍照后走 HTTP 直连本服务。

> 已从 `face_recognition(dlib)` 换成 OpenCV 官方深度学习模型。原因：dlib 在 PyPI 上
> **没有 Windows 预编译 wheel**（只有源码包），pip 安装会去编译 C++ 源码，在中文 GBK
> 环境下直接报错。改用纯 OpenCV 方案后，只需 `opencv-python`（有 Windows wheel），零编译。

## 模型文件（已下载好，放在本目录）

| 文件 | 用途 | 大小 |
|------|------|------|
| `face_detection_yunet_2023mar.onnx` | 人脸检测 YuNet | ~227KB |
| `face_recognition_sface_2021dec.onnx` | 人脸识别 SFace（128 维特征） | ~37MB |

模型来自 [OpenCV 官方模型库 opencv_zoo](https://github.com/opencv/opencv_zoo)。
如果本机删了需要重下，GitHub 直连不通时可用镜像前缀（本机已实测可用）：

```bash
curl -L -o face_detection_yunet_2023mar.onnx \
  https://ghfast.top/https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx
curl -L -o face_recognition_sface_2021dec.onnx \
  https://ghfast.top/https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx
```

## 环境要求

- Python 3.9+（`opencv-python` 全版本都有 Windows wheel，无 dlib 版本限制）

```bash
cd pc_face
pip install -r requirements.txt      # flask + opencv-python + numpy
```

## 第一步：本地验证（不依赖 ESP32）

用电脑摄像头先跑通识别逻辑：

```bash
python webcam_demo.py --enroll 张三     # 正对摄像头录入一张脸
python webcam_demo.py                   # 实时识别：窗口显示"张三(已录入)"或"未录入"
```

## 第二步：启动 HTTP 服务（给 ESP32 用）

```bash
python face_server.py
```

接口（`image` 为 JPEG 文件字段）：

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/enroll` | 表单 `name=张三` + `image=<JPEG>`，录入人脸 |
| POST | `/recognize` | 表单 `image=<JPEG>`，返回 `{matched, name, similarity}` |
| GET  | `/faces` | 列出已录入的人 |

命令行自测：

```bash
curl -F "name=张三" -F "image=@face.jpg" http://127.0.0.1:5000/enroll
curl -F "image=@face.jpg" http://127.0.0.1:5000/recognize
```

## 参数说明

- `THRESHOLD = 0.363`（`face_db.py`）：**余弦相似度**阈值（0~1，越大越像）。
  同一个人通常 > 0.4，不同人通常 < 0.2，中间地带按需微调。此值为 OpenCV 官方示例推荐值。
- `DETECT_SCORE = 0.7`（`face_db.py`）：人脸检测置信度阈值（0.9 对 ESP32 的 VGA 图偏严、易漏检）。

## 下一步

- ESP32 端用 `esp_http_client` 把 OV5640 的 JPEG 帧 POST 到本服务（Phase 2）
- ESP32 TFT/LVGL 录入界面（Phase 3）
- 识别结果用现有 MQTT 上报 OneNet（Phase 4）
