"""ESP32-S3 两轴舵机人脸追踪（PC 端）。

- 直接读取 ESP32 的 MJPEG 流并手动解析 JPEG（不走 cv2.VideoCapture）
- OpenCV YuNet (FaceDetectorYN) 检测人脸
- P 控制器把偏差映射成 pan/tilt，后台线程经 HTTP GET /servo 发给 ESP32

诊断：控制台会打印 [status]（是否检测到脸 + 当前角度）和 [servo]（实际发出的命令），
据此判断卡在「检测」还是「发送」。

依赖模型文件 face_detection_yunet_2023mar.onnx（与脚本同目录）。
"""

import os
import time
import socket
import threading
import urllib.request

import numpy as np
import cv2

STREAM_URL = "http://192.168.4.1/stream"
SERVO_HOST = "192.168.4.1"
SERVO_UDP_PORT = 8080
MODEL_NAME = "face_detection_yunet_2023mar.onnx"

# 舵机角度范围（与 ESP32 端 servo.h 的安全行程保持一致）
PAN_MIN, PAN_MAX = 10, 180
TILT_MIN, TILT_MAX = 10, 180

# P 控制器增益（只启用 P 控制器）
KP_PAN = 0.05
KP_TILT = 0.05

# 死区
DEADBAND = 12

# 控制周期（秒）：舵机响应慢，每帧都调会来回振荡，放慢到 ~8Hz
CONTROL_INTERVAL = 0.12

# 人脸中心 EMA 平滑系数（0~1，越小越平滑、越抗检测框抖动）
EMA_ALPHA = 0.3


# ===== 后台发送线程：目标角度变化时才发，每次独立请求（不依赖 keep-alive） =====
_lock = threading.Lock()
_target = (90, 90)
_sent = None


def set_target(p, t):
    global _target
    with _lock:
        _target = (int(p), int(t))


def _servo_sender():
    global _sent
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    while True:
        with _lock:
            target = _target
        if target != _sent:
            msg = f"{target[0]} {target[1]}"
            try:
                sock.sendto(msg.encode(), (SERVO_HOST, SERVO_UDP_PORT))
                _sent = target
                print(f"  [servo] pan={target[0]} tilt={target[1]}")
            except Exception as e:
                print(f"  [servo 发送失败] {e}")
                time.sleep(0.2)
        else:
            time.sleep(0.02)


def extract_frames(buf):
    """从字节缓冲里切出完整 JPEG 帧，返回 (帧列表, 剩余缓冲)。"""
    frames = []
    while True:
        a = buf.find(b"\xff\xd8")  # JPEG SOI
        if a == -1:
            break
        b = buf.find(b"\xff\xd9", a + 2)  # JPEG EOI
        if b == -1:
            break
        jpg = buf[a:b + 2]
        buf = buf[b + 2:]
        frame = cv2.imdecode(np.frombuffer(jpg, dtype=np.uint8), cv2.IMREAD_COLOR)
        if frame is not None:
            frames.append(frame)
    return frames, buf


def main():
    pan, tilt = 90, 90
    smooth_cx = None
    smooth_cy = None
    last_control = 0.0

    model_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), MODEL_NAME)
    if not os.path.exists(model_path):
        print(f"缺少模型文件：{model_path}")
        return

    detector = cv2.FaceDetectorYN.create(
        model_path, "", (320, 240), score_threshold=0.6, nms_threshold=0.3, top_k=5000
    )

    threading.Thread(target=_servo_sender, daemon=True).start()

    print(f"正在连接视频流 {STREAM_URL} ...")
    print("提示：连不上时确认电脑已连接热点 ESP32-CAM（密码 12345678）")

    stop = False
    last_status = 0.0
    frames_total = 0

    while not stop:
        try:
            stream = urllib.request.urlopen(STREAM_URL, timeout=5)
        except Exception as e:
            print(f"连接失败：{e}，2 秒后重试...")
            time.sleep(2)
            continue

        buf = b""
        while not stop:
            try:
                chunk = stream.read(4096)
            except Exception:
                chunk = None
            if not chunk:
                break

            buf += chunk
            frames, buf = extract_frames(buf)

            for frame in frames:
                frames_total += 1
                h, w = frame.shape[:2]
                detector.setInputSize((w, h))
                _, faces = detector.detect(frame)

                cx_frame, cy_frame = w // 2, h // 2
                face_seen = False

                if faces is not None and len(faces) > 0:
                    face_seen = True
                    best = max(faces, key=lambda f: f[14])
                    x, y, fw, fh = int(best[0]), int(best[1]), int(best[2]), int(best[3])
                    cx = x + fw // 2
                    cy = y + fh // 2

                    # EMA 平滑人脸中心，消除检测框抖动
                    if smooth_cx is None:
                        smooth_cx, smooth_cy = float(cx), float(cy)
                    else:
                        smooth_cx += EMA_ALPHA * (cx - smooth_cx)
                        smooth_cy += EMA_ALPHA * (cy - smooth_cy)

                    cv2.rectangle(frame, (x, y), (x + fw, y + fh), (0, 255, 0), 2)

                # 控制限频：舵机响应慢，每帧都调会来回振荡
                tick = time.time()
                if tick - last_control >= CONTROL_INTERVAL:
                    last_control = tick
                    if smooth_cx is not None:
                        err_x = smooth_cx - cx_frame
                        err_y = smooth_cy - cy_frame
                        if abs(err_x) > DEADBAND:
                            pan = min(PAN_MAX, max(PAN_MIN, pan + KP_PAN * err_x))
                        if abs(err_y) > DEADBAND:
                            tilt = min(TILT_MAX, max(TILT_MIN, tilt + KP_TILT * err_y))

                set_target(pan, tilt)

                cv2.line(frame, (cx_frame - 10, cy_frame), (cx_frame + 10, cy_frame), (255, 0, 0), 2)
                cv2.line(frame, (cx_frame, cy_frame - 10), (cx_frame, cy_frame + 10), (255, 0, 0), 2)
                cv2.putText(frame, f"pan={pan:.0f} tilt={tilt:.0f}",
                            (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 0), 2)

                cv2.imshow("Face Tracking", frame)
                if cv2.waitKey(1) & 0xFF == 27:
                    stop = True
                    break

                now = time.time()
                if now - last_status >= 1.0:
                    print(f"[status] frame={frames_total} face={'Y' if face_seen else 'N'} "
                          f"pan={pan:.1f} tilt={tilt:.1f}")
                    last_status = now

        if not stop:
            print("视频流断开，重连...")
            time.sleep(1)

    set_target(90, 90)
    time.sleep(0.3)
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
