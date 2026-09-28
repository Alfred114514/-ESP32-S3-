"""电脑端「本地人脸库」下发工具：把图片 + 姓名下发给 ESP32，由 ESP32 自己提取特征存 flash。

用法（先连上 ESP32 热点，IP 默认 192.168.4.1）：
    python local_db.py enroll 张三 face.jpg              # 录入（ESP32 本地算特征并存储）
    python local_db.py list                              # 列出已录入姓名
    python local_db.py delete 张三                       # 按姓名删除
    python local_db.py enroll 张三 face.jpg 192.168.4.1  # 指定 IP

说明：
    - 任意常见图片格式均可（PNG/JPG/BMP…），本脚本用 OpenCV 统一压成长边 640 的 JPEG 再下发。
    - 录入/识别都在 ESP32 本地（ESP-DL MFN 模型），电脑端只是"下发"入口，不参与识别。
"""
import os
import sys
import urllib.request
import urllib.parse

DEFAULT_IP = "192.168.4.1"
BOUNDARY = "----ESP32LocalFaceBoundary"


def load_jpeg(path):
    """统一解码 -> 长边压到 640px -> 重编码 JPEG。

    手机上/相机拍的原图常是几百万像素，ESP32 软件解码整张会内存爆炸（8MB PSRAM 不够），
    这里先压小再下发，保证 ESP32 能解、且 JPEG 体积落在 128KB 缓冲内。
    """
    import cv2
    import numpy as np
    with open(path, "rb") as f:
        data = f.read()
    img = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        raise RuntimeError(f"无法读取图片: {path}（请确认是图片文件，且本机已装 opencv-python）")
    h, w = img.shape[:2]
    max_side = max(h, w)
    if max_side > 640:
        scale = 640.0 / max_side
        img = cv2.resize(img, (int(w * scale), int(h * scale)), interpolation=cv2.INTER_AREA)
    ok, buf = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 85])
    if not ok:
        raise RuntimeError("JPEG 编码失败")
    return buf.tobytes()


def make_multipart(name, jpeg):
    body = b""
    body += f"--{BOUNDARY}\r\n".encode()
    body += f'Content-Disposition: form-data; name="name"\r\n\r\n'.encode()
    body += name.encode("utf-8") + b"\r\n"
    body += f"--{BOUNDARY}\r\n".encode()
    body += f'Content-Disposition: form-data; name="image"; filename="face.jpg"\r\n'.encode()
    body += b"Content-Type: image/jpeg\r\n\r\n"
    body += jpeg + b"\r\n"
    body += f"--{BOUNDARY}--\r\n".encode()
    return body


def enroll(name, img_path, ip):
    jpeg = load_jpeg(img_path)
    body = make_multipart(name, jpeg)
    url = f"http://{ip}/local_enroll"
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", f"multipart/form-data; boundary={BOUNDARY}")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            print(r.read().decode("utf-8", "replace"))
    except Exception as e:
        print(f"请求失败: {e}")


def list_faces(ip):
    url = f"http://{ip}/local_faces"
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            print(r.read().decode("utf-8", "replace"))
    except Exception as e:
        print(f"请求失败: {e}")


def delete(name, ip):
    url = f"http://{ip}/local_faces?name={urllib.parse.quote(name)}"
    req = urllib.request.Request(url, method="DELETE")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            print(r.read().decode("utf-8", "replace"))
    except Exception as e:
        print(f"请求失败: {e}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]

    if cmd == "enroll":
        if len(sys.argv) < 4:
            print("用法: python local_db.py enroll <姓名> <图片.jpg> [ip]")
            return
        name, img = sys.argv[2], sys.argv[3]
        ip = sys.argv[4] if len(sys.argv) >= 5 else DEFAULT_IP
        enroll(name, img, ip)
    elif cmd == "list":
        ip = sys.argv[2] if len(sys.argv) >= 3 else DEFAULT_IP
        list_faces(ip)
    elif cmd == "delete":
        if len(sys.argv) < 3:
            print("用法: python local_db.py delete <姓名> [ip]")
            return
        name = sys.argv[2]
        ip = sys.argv[3] if len(sys.argv) >= 4 else DEFAULT_IP
        delete(name, ip)
    else:
        print("未知子命令，支持: enroll / list / delete")


if __name__ == "__main__":
    main()
