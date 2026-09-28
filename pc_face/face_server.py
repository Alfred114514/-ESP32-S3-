"""电脑端人脸识别 HTTP 服务（Flask）。

ESP32 拍照后通过 esp_http_client 把 JPEG POST 过来：
  - POST /enroll    表单: name=张三, image=<JPEG文件>  -> 录入人脸
  - POST /recognize 表单: image=<JPEG文件>             -> 识别是否已录入
  - GET  /faces     -> 列出已录入的人

启动：python face_server.py  （监听 0.0.0.0:5000）
"""
from flask import Flask, request, jsonify, Response
import time
import face_db

app = Flask(__name__)

# 门禁日志事件名 -> 中文（ESP32 上报 /log 时用英文事件名，这里转成人话打印）
EVENT_CN = {"open": "开门", "close": "关门", "dwell": "陌生人逗留"}


@app.route("/enroll", methods=["POST"])
def enroll():
    name = request.form.get("name", "").strip()
    img = request.files.get("image")
    if not name or img is None:
        return jsonify({"code": -1, "msg": "缺少 name 或 image"}), 400
    image = face_db.image_from_bytes(img.read())
    ok, msg = face_db.enroll(name, image)
    return jsonify({"code": 0 if ok else -1, "msg": msg}), (200 if ok else 400)


@app.route("/recognize", methods=["POST"])
def recognize():
    img = request.files.get("image")
    if img is None:
        return jsonify({"code": -1, "msg": "缺少 image"}), 400
    image = face_db.image_from_bytes(img.read())
    matched, name, similarity, box = face_db.recognize_boxed(image)
    if matched is None:
        return jsonify({"code": -1, "msg": "未检测到人脸"}), 400
    resp = {
        "code": 0,
        "matched": matched,
        "name": name,
        "similarity": similarity,  # 余弦相似度 0~1，越大越像
    }
    if box is not None:
        resp["box"] = list(box)  # [x, y, w, h]，摄像头 VGA 坐标系，供下位机画绿/红框
    return jsonify(resp)


@app.route("/recognize_multi", methods=["POST"])
def recognize_multi():
    img = request.files.get("image")
    if img is None:
        return jsonify({"code": -1, "msg": "缺少 image"}), 400
    image = face_db.image_from_bytes(img.read())
    faces = face_db.recognize_all(image)
    return jsonify({"code": 0, "faces": faces})


@app.route("/faces", methods=["GET"])
def faces():
    return jsonify({"code": 0, "faces": face_db.list_names()})


@app.route("/faces", methods=["DELETE"])
def delete_face():
    name = request.args.get("name", "").strip()
    if not name:
        return jsonify({"code": -1, "msg": "缺少 name"}), 400
    ok, msg = face_db.delete_person(name)
    return jsonify({"code": 0 if ok else -1, "msg": msg}), (200 if ok else 404)


@app.route("/avatar", methods=["GET"])
def avatar():
    name = request.args.get("name", "").strip()
    if not name:
        return jsonify({"code": -1, "msg": "缺少 name"}), 400
    data = face_db.get_avatar_bytes(name)
    if data is None:
        return jsonify({"code": -1, "msg": "无头像"}), 404
    return Response(data, mimetype="image/jpeg")


@app.route("/log", methods=["POST"])
def log():
    """接收 ESP32 门禁日志：{"name":"张三","event":"open","time":"12:03:21"}。
    打印到命令行窗口，并追加到 door.log（相对本脚本目录）。"""
    data = request.get_json(silent=True)
    if not data:
        data = request.form.to_dict()
    name = (data.get("name") or "").strip()
    event = (data.get("event") or "").strip()
    t = (data.get("time") or "").strip()
    # ESP32 尚未 SNTP 授时时 time 为 "--:--:--"，此时退回 PC 本机时间
    if not t or t == "--:--:--":
        t = time.strftime("%H:%M:%S")
    ev = EVENT_CN.get(event, event or "?")
    line = "[%s] %s" % (t, ev)
    if name:
        line += " " + name
    print(line, flush=True)
    try:
        with open("door.log", "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception as e:
        print("写日志文件失败:", e)
    return jsonify({"code": 0}), 200


if __name__ == "__main__":
    print("人脸识别服务已启动: http://0.0.0.0:5000")
    app.run(host="0.0.0.0", port=5000)
