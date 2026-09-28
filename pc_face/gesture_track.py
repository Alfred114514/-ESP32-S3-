"""电脑端「联动」程序：人脸追踪（舵机）+ 手势识别（1~5 / OK）。

人脸追踪：增量式 P 控制器
  - 定时拉 ESP32 单帧快照  http://<ESP32_IP>/snapshot_full  （全彩全分辨率）
  - YuNet 检测人脸，对人脸中心做 EMA 平滑
  - err = 平滑中心 - 画面中心，超过死区才用 P 增益累加角度（不是绝对映射）
  - 后台线程只在新目标变化时经 UDP 发给 ESP32 舵机服务器（端口 8080，协议 "pan tilt"）

为什么用「拉单帧」而不是「连续 MJPEG /stream」：
  - 手机热点下长连接 MJPEG 流极易被断开（几分钟就 timed out、频繁重连、画面卡顿）；
  - 单帧快照每次是独立的短 HTTP 请求，断了下一帧自动重来，抗抖动，帧率稳定在 5~10fps。

手势识别用 MediaPipe Hands：
  - 实时画出 21 个手部关键点（节点）与连线
  - 识别 1 / 2 / 3 / 4 / 5 根手指，以及「OK」（拇指尖与食指尖捏合 + 其余三指伸直）

依赖：
    pip install opencv-python mediapipe
  mediapipe 可选：没装的话仍可只做人脸追踪。
  手部模型 hand_landmarker.task 需放在本脚本同目录（pc_face/）下。

启动：
    python gesture_track.py                 # 用下面 ESP32_IP 的兜底值
    python gesture_track.py 192.168.210.179 # 现在 ESP32 是纯 STA，务必传串口里的 STA IP
退出：按 q 或 Esc。
"""
import os
import sys
import math
import time
import socket
import threading
import urllib.request

import numpy as np
import cv2

import face_db  # 复用 YuNet 模型路径（face_db.DETECTOR_MODEL）

ESP32_IP = "192.168.210.179"   # 兜底 IP；现在 ESP32 是纯 STA，热点 DHCP 会变，记得和串口里的 IP 对齐
SERVO_PORT = 8080

# 舵机角度范围（与 servo.h 的安全行程一致）
PAN_MIN, PAN_MAX = 10, 180
TILT_MIN, TILT_MAX = 10, 180

# P 控制器增益（和 track.py 一致，只启用 P）
KP_PAN = 0.12
KP_TILT = 0.12

# 死区：偏差小于该像素就不动，避免舵机来回抖动
DEADBAND = 12

# 控制周期（秒）：舵机响应慢，每帧都调会来回振荡，放慢到 ~8Hz
CONTROL_INTERVAL = 0.12

# 人脸中心 EMA 平滑系数（越小越平滑、越抗检测框抖动）
EMA_ALPHA = 0.3

# 若发现舵机转反了，把对应项改成 -1（云台安装方向不同会导致正负相反）
PAN_FLIP = 1
TILT_FLIP = 1

# 显示窗口放大倍数：ESP32 快照 320x240，放大后更清楚，也更容易让 MediaPipe 识别到手
DISPLAY_SCALE = 2.0


# ===== 后台发送线程：目标角度变化时才发（与 track.py 一致） =====
_lock = threading.Lock()
_target = (90, 90)
_sent = None
SERVO_HOST = ESP32_IP   # 由 main() 根据命令行参数更新


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
                sock.sendto(msg.encode(), (SERVO_HOST, SERVO_PORT))
                _sent = target
                print(f"  [servo] pan={target[0]} tilt={target[1]}")
            except Exception as e:
                print(f"  [servo 发送失败] {e}")
                time.sleep(0.2)
        else:
            time.sleep(0.02)


# ===== 拉单帧（抗断线：独立短请求，失败返回 None，下一帧自动重来） =====
def http_get_bytes(url, timeout=3):
    """抓一帧 JPEG 原始字节，失败返回 None（超时/断连不抛异常）。"""
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read()
    except Exception:
        return None


# ===== 手势分类 =====
def _dist(a, b):
    return math.hypot(a.x - b.x, a.y - b.y)


def _is_thumb_up(lm):
    # 拇指：指尖(4) 到食指根 MCP(5) 的距离，明显大于拇指 IP(3) 到食指根的距离 → 拇指张开
    return _dist(lm[4], lm[5]) > 1.4 * _dist(lm[3], lm[5])


def count_fingers(lm):
    """数伸出的手指数（1~5）。四指看指尖是否高于 PIP，拇指看距离。"""
    n = 0
    for tip, pip in [(8, 6), (12, 10), (16, 14), (20, 18)]:
        if lm[tip].y < lm[pip].y:
            n += 1
    if _is_thumb_up(lm):
        n += 1
    return n


def classify_gesture(lm):
    """返回手势字符串：'OK' 或 '1'~'5'（含 '0' 握拳）。"""
    # OK：拇指尖(4) 与食指尖(8) 捏合，且中/无名/小指伸直
    if _dist(lm[4], lm[8]) < 0.06:
        if lm[12].y < lm[10].y and lm[16].y < lm[14].y and lm[20].y < lm[18].y:
            return "OK"
    return str(count_fingers(lm))


def main():
    global SERVO_HOST
    ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    SERVO_HOST = ip
    snap_url = f"http://{ip}/snapshot_full"
    print(f"联动程序启动：snapshot={snap_url}  servo={ip}:{SERVO_PORT}")

    # ---- YuNet 人脸检测（复用 face_db 的模型路径） ----
    model_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              os.path.basename(face_db.DETECTOR_MODEL))
    if not os.path.exists(model_path):
        model_path = face_db.DETECTOR_MODEL
    detector = cv2.FaceDetectorYN.create(
        model_path, "", (320, 240), score_threshold=0.6, nms_threshold=0.3, top_k=5000)

    # ---- MediaPipe 手部（可选，Tasks API，需要 hand_landmarker.task 模型） ----
    # 注意：Python 3.14 下可装的 mediapipe（0.10.30+/1.x）已移除旧的 mp.solutions 接口，
    # 只能用 tasks 的 HandLandmarker，所以额外需要下载一个 .task 模型文件。
    landmarker = None
    mp_module = None
    mp_python = None
    mp_vision = None
    HAND_CONNECTIONS = None
    try:
        import mediapipe as mp
        from mediapipe.tasks import python as _mp_python
        from mediapipe.tasks.python import vision as _mp_vision

        model_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "hand_landmarker.task")
        if not os.path.exists(model_path):
            raise RuntimeError(f"缺少手部模型文件：{model_path}")

        options = _mp_vision.HandLandmarkerOptions(
            base_options=_mp_python.BaseOptions(model_asset_path=model_path),
            running_mode=_mp_vision.RunningMode.VIDEO,
            num_hands=2,
            min_hand_detection_confidence=0.3,
            min_hand_presence_confidence=0.3,
            min_tracking_confidence=0.3)
        landmarker = _mp_vision.HandLandmarker.create_from_options(options)
        mp_module = mp
        mp_python = _mp_python
        mp_vision = _mp_vision
        HAND_CONNECTIONS = _mp_vision.HandLandmarksConnections.HAND_CONNECTIONS
        print(f"[info] 手势识别已启用（mediapipe {mp.__version__}）")
    except Exception as e:
        print("=" * 60)
        print("[!!] 手势识别未启用：没装 mediapipe 或缺少 hand_landmarker.task。")
        print(f"     错误：{e}")
        print("     请执行：pip install mediapipe")
        print("     并确认本目录（pc_face/）有 hand_landmarker.task 模型文件。")
        print("     就绪后重启本脚本即可（人脸追踪不受影响）。")
        print("=" * 60)

    # ---- P 控制器状态 ----
    pan, tilt = 90, 90
    smooth_cx, smooth_cy = None, None
    last_control = 0.0
    frames_total = 0
    last_status = 0.0
    stop = False
    t0 = time.time()   # 手势 VIDEO 模式时间戳起点（毫秒）

    threading.Thread(target=_servo_sender, daemon=True).start()

    while not stop:
        # 拉单帧全彩快照（独立短请求，断了下一帧自动重来，不卡死）
        jpg = http_get_bytes(snap_url, timeout=3)
        if jpg is None:
            time.sleep(0.05)
            continue
        frame = cv2.imdecode(np.frombuffer(jpg, dtype=np.uint8), cv2.IMREAD_COLOR)
        if frame is None:
            time.sleep(0.05)
            continue

        frames_total += 1
        h, w = frame.shape[:2]
        detector.setInputSize((w, h))
        _, faces = detector.detect(frame)

        cx_frame, cy_frame = w // 2, h // 2
        face_seen = False
        x = y = fw = fh = 0

        # 1) 人脸追踪：增量 P 控制
        if faces is not None and len(faces) > 0:
            face_seen = True
            best = max(faces, key=lambda f: f[14])
            x, y, fw, fh = int(best[0]), int(best[1]), int(best[2]), int(best[3])
            cx, cy = x + fw // 2, y + fh // 2

            # EMA 平滑人脸中心，消除检测框抖动
            if smooth_cx is None:
                smooth_cx, smooth_cy = float(cx), float(cy)
            else:
                smooth_cx += EMA_ALPHA * (cx - smooth_cx)
                smooth_cy += EMA_ALPHA * (cy - smooth_cy)

        # 控制限频 + 死区
        tick = time.time()
        if tick - last_control >= CONTROL_INTERVAL:
            last_control = tick
            if smooth_cx is not None:
                err_x = PAN_FLIP * (smooth_cx - cx_frame)
                err_y = TILT_FLIP * (smooth_cy - cy_frame)
                if abs(err_x) > DEADBAND:
                    pan = min(PAN_MAX, max(PAN_MIN, pan + KP_PAN * err_x))
                if abs(err_y) > DEADBAND:
                    tilt = min(TILT_MAX, max(TILT_MIN, tilt + KP_TILT * err_y))

        set_target(pan, tilt)

        # 2) 放大画面再画框/文字/手
        big = cv2.resize(frame, None, fx=DISPLAY_SCALE, fy=DISPLAY_SCALE,
                         interpolation=cv2.INTER_LINEAR)
        bh, bw = big.shape[:2]
        s = DISPLAY_SCALE

        if face_seen:
            cv2.rectangle(big, (int(x * s), int(y * s)),
                          (int((x + fw) * s), int((y + fh) * s)), (0, 255, 0), 2)

        # 画面中心十字线
        ccx, ccy = int(cx_frame * s), int(cy_frame * s)
        cv2.line(big, (ccx - 16, ccy), (ccx + 16, ccy), (255, 0, 0), 2)
        cv2.line(big, (ccx, ccy - 16), (ccx, ccy + 16), (255, 0, 0), 2)
        cv2.putText(big, f"pan={pan:.0f} tilt={tilt:.0f}",
                    (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 0), 2)

        # 3) 手势识别
        if landmarker is not None:
            rgb = cv2.cvtColor(big, cv2.COLOR_BGR2RGB)
            mp_img = mp_module.Image(
                image_format=mp_module.ImageFormat.SRGB, data=rgb)
            ts_ms = int((time.time() - t0) * 1000)
            res = landmarker.detect_for_video(mp_img, ts_ms)
            if res.hand_landmarks:
                for lms in res.hand_landmarks:
                    for conn in HAND_CONNECTIONS:
                        a, b = lms[conn.start], lms[conn.end]
                        cv2.line(big,
                                 (int(a.x * bw), int(a.y * bh)),
                                 (int(b.x * bw), int(b.y * bh)),
                                 (0, 255, 0), 2)
                    for lm in lms:
                        cv2.circle(big, (int(lm.x * bw), int(lm.y * bh)),
                                   4, (0, 0, 255), -1)
                    label = classify_gesture(lms)
                    hx = int(lms[9].x * bw)
                    hy = int(lms[9].y * bh)
                    cv2.putText(big, label, (hx - 16, hy - 16),
                                cv2.FONT_HERSHEY_SIMPLEX, 1.4, (0, 0, 255), 3)

        cv2.imshow("gesture_track (q=quit)", big)
        if cv2.waitKey(1) & 0xFF in (ord('q'), 27):
            stop = True
            break

        now = time.time()
        if now - last_status >= 1.0:
            print(f"[status] frame={frames_total} face={'Y' if face_seen else 'N'} "
                  f"pan={pan:.1f} tilt={tilt:.1f}")
            last_status = now

        time.sleep(0.05)   # 限速到 ~10fps 上限，别把 ESP32 的 HTTP 打满

    set_target(90, 90)
    time.sleep(0.3)
    cv2.destroyAllWindows()
    print("联动程序退出")


if __name__ == "__main__":
    main()
