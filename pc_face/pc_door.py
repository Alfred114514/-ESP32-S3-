#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pc_door.py —— 门禁决策端（跑在 PC 上）

轮询 ESP32 的 /door_status：当「陌生人(stranger) + 大幅挥手(waving)」同时为 true 时，
弹出实时摄像头画面，由 PC 端的人按键决定是否开门。

用法：
    python pc_door.py [esp_ip]        # 默认 192.168.4.1（ESP32 热点网关）

按键（在弹出的画面窗口里）：
    O  开门   → GET /door?action=open
    X  拒绝   → GET /door?action=close
    Q/ESC 关闭 → 不开门

依赖：OpenCV（cv2）+ numpy + Python 标准库。

低延迟优化：
  - 手动解析 MJPEG（不走 cv2.VideoCapture 的 ffmpeg 缓冲，后者会囤帧导致画面滞后）。
  - 只显示最新一帧，丢弃积压旧帧。
  - 开门/关门命令异步发送（后台线程），主线程不阻塞，按键秒响应。
"""
import sys
import time
import json
import socket
import threading
import urllib.request
import numpy as np
import cv2

ESP_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.4.1"


def self_ip_for(dst):
    """本机发往 dst 的出口网卡 IP（UDP connect 不真正发包，只查路由）。"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((dst, 80))
            return s.getsockname()[0]
        finally:
            s.close()
    except Exception:
        return ""


PC_IP = self_ip_for(ESP_IP)
STREAM_URL = "http://%s/stream" % ESP_IP
# 轮询时把电脑 IP 带在 query 上，让 ESP32 缓存，供它日志上报 POST 回本机
STATUS_URL = "http://%s/door_status?pc=%s" % (ESP_IP, PC_IP)
DOOR_URL = "http://%s/door" % ESP_IP
SNAPSHOT_URL = "http://%s/snapshot" % ESP_IP


def http_json(url):
    try:
        with urllib.request.urlopen(url, timeout=2) as r:
            return json.loads(r.read().decode("utf-8"))
    except Exception:
        return None


def http_get(url):
    try:
        urllib.request.urlopen(url, timeout=2).read()
    except Exception as e:
        print("请求失败:", url, e)


def http_get_bytes(url, timeout=2):
    """抓原始字节，失败返回 None（超时/断连不抛异常）。"""
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read()
    except Exception:
        return None


def _do_set_door(open_):
    http_get("%s?action=%s" % (DOOR_URL, "open" if open_ else "close"))
    print("=> 已%s" % ("开门" if open_ else "关门/拒绝"))


def set_door(open_):
    """异步发送开门/关门命令，不阻塞主线程（否则按键后窗口会卡到命令发完）。"""
    threading.Thread(target=_do_set_door, args=(open_,), daemon=True).start()


def extract_frames(buf):
    """从字节缓冲里切出完整 JPEG 帧，返回 (帧列表, 剩余缓冲)。"""
    frames = []
    while True:
        a = buf.find(b"\xff\xd8")       # JPEG SOI
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


def decision_loop():
    """弹出实时画面（定时拉单帧快照，低延迟、断线自动恢复），等待人来按键决定。"""
    print("实时画面已弹出：O=开门  X=拒绝  Q/ESC=关闭(不开门)")
    while True:
        # 拉单帧快照（而非持续 MJPEG 推流）：省带宽、断线自动恢复，不会卡死/超时
        jpg = http_get_bytes(SNAPSHOT_URL, timeout=2)
        if jpg:
            frame = cv2.imdecode(np.frombuffer(jpg, dtype=np.uint8), cv2.IMREAD_COLOR)
            if frame is not None:
                cv2.putText(frame, "STRANGER WAVING", (10, 30),
                            cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 0, 255), 2)
                cv2.putText(frame, "O=open  X=reject  Q=close", (10, 62),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
                cv2.imshow("Door decision", frame)

        k = cv2.waitKey(1) & 0xFF
        if k == ord('o'):
            set_door(True)
            break
        elif k == ord('x'):
            set_door(False)
            break
        elif k == ord('q') or k == 27:      # ESC
            set_door(False)
            break

        time.sleep(0.15)   # ~5fps 拉取节奏（即便快照失败也不退出）
    cv2.destroyAllWindows()


def main():
    print("门禁决策端已启动，本机 IP=%s，轮询 %s" % (PC_IP, STATUS_URL))
    print("在 ESP32 进「本地识别」后，让陌生人出现在镜头前并大幅挥手，本程序会自动弹窗。")

    last = None
    while True:
        st = http_json(STATUS_URL)
        if st is not None:
            key = (st.get("door"), st.get("stranger"), st.get("waving"))
            if key != last:
                print("[status] door=%s stranger=%s waving=%s" %
                      (st.get("door"), st.get("stranger"), st.get("waving")))
                last = key
        else:
            if last is not None:
                print("[status] 连不上 ESP32（%s）" % STATUS_URL)
                last = None

        if st and st.get("stranger") and st.get("waving"):
            print("检测到 陌生人 + 大幅挥手，弹出实时画面，请决定是否开门")
            decision_loop()
            # 冷却：等告警解除（或最多 15 秒）再重新监听，避免同一波反复弹窗
            t0 = time.time()
            while time.time() - t0 < 15:
                st = http_json(STATUS_URL)
                if st and not (st.get("stranger") and st.get("waving")):
                    break
                time.sleep(0.5)
        time.sleep(1.0)


if __name__ == "__main__":
    main()
