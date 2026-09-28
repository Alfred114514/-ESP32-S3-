"""人脸库核心：录入 / 识别，供 HTTP 服务和本地演示共用。

不依赖 dlib，改用 OpenCV 官方深度学习模型（Windows 有预编译 wheel，直接 pip 装 opencv-python）：
  - 人脸检测：YuNet  (face_detection_yunet_2023mar.onnx,  ~227KB)
  - 人脸识别：SFace (face_recognition_sface_2021dec.onnx, ~37MB)
两个模型文件需与本文件放在同一目录。

识别流程：YuNet 检出人脸 -> SFace 提取 128 维特征 -> 与库中每个人两两算余弦相似度，
取最高分，超过阈值(0.363)即判为"同一人"。阈值是 OpenCV 官方示例推荐值。
"""
import os
import json
import re
import numpy as np
import cv2

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DETECTOR_MODEL = os.path.join(BASE_DIR, "face_detection_yunet_2023mar.onnx")
RECOGNIZER_MODEL = os.path.join(BASE_DIR, "face_recognition_sface_2021dec.onnx")
DB_FILE = os.path.join(BASE_DIR, "faces.json")
AVATAR_DIR = os.path.join(BASE_DIR, "avatars")
AVATAR_SIZE = 48  # 小头像边长（像素），越小越省下位机内存

# 余弦相似度阈值：同一个人通常 > 0.4，不同人通常 < 0.2；OpenCV 官方示例取 0.363。
THRESHOLD = 0.363
# YuNet 检测置信度阈值（0~1，越高越严格）。0.9 对 ESP32 摄像头这种
# 640x480 的图偏严、容易漏检，降到 0.7 更稳（后面还有余弦相似度兜底，不会乱认人）。
DETECT_SCORE = 0.7

_detector = None
_recognizer = None


def _get_detector(w, h):
    global _detector
    if _detector is None:
        _detector = cv2.FaceDetectorYN.create(
            DETECTOR_MODEL, "", (w, h), DETECT_SCORE, 0.3, 5000)
    _detector.setInputSize((w, h))
    return _detector


def _get_recognizer():
    global _recognizer
    if _recognizer is None:
        _recognizer = cv2.FaceRecognizerSF.create(RECOGNIZER_MODEL, "")
    return _recognizer


def load_db():
    """读取人脸库。返回 [{"name": str, "feature": np.ndarray(1,128)}, ...]。"""
    if not os.path.exists(DB_FILE):
        return []
    with open(DB_FILE, "r", encoding="utf-8") as f:
        db = json.load(f)
    for item in db:
        item["feature"] = np.array(item["feature"], dtype=np.float32).reshape(1, -1)
    return db


def save_db(db):
    out = [{"name": item["name"], "feature": item["feature"].reshape(-1).tolist()}
           for item in db]
    with open(DB_FILE, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)


def image_from_bytes(data):
    """把 HTTP 上传的 JPEG/PNG 字节流解码成 BGR 图（OpenCV 内部统一用 BGR）。"""
    arr = np.frombuffer(data, np.uint8)
    return cv2.imdecode(arr, cv2.IMREAD_COLOR)


def _detect_faces(image):
    """YuNet 检出所有人脸，返回 (N,15) 数组；没人脸返回 None。"""
    h, w = image.shape[:2]
    ret, faces = _get_detector(w, h).detect(image)
    if not ret or faces is None:
        return None
    return faces


def _largest_face(image):
    """返回面积最大的一张脸（15 元素行）；没人脸返回 None。"""
    faces = _detect_faces(image)
    if faces is None:
        return None
    areas = faces[:, 2] * faces[:, 3]  # 宽 * 高
    return faces[int(np.argmax(areas))]


def detect_boxes(image):
    """返回所有人脸框 [(x, y, w, h), ...]，供界面画框显示。"""
    faces = _detect_faces(image)
    if faces is None:
        return []
    return [(int(f[0]), int(f[1]), int(f[2]), int(f[3])) for f in faces]


def _feature(image, face):
    rec = _get_recognizer()
    aligned = rec.alignCrop(image, face)
    return rec.feature(aligned)  # (1, 128)


def enroll(name, image):
    """录入一张人脸（同名覆盖）。返回 (成功?, 消息)。"""
    if image is None:
        return False, "图片解码失败"
    face = _largest_face(image)
    if face is None:
        return False, "未检测到人脸"
    feat = _feature(image, face)
    db = load_db()
    db = [item for item in db if item["name"] != name]
    db.append({"name": name, "feature": feat})
    save_db(db)
    _save_avatar(name, image, face)
    return True, f"录入成功: {name}"


def _recognize_core(image):
    """识别一张人脸，返回 (matched, name, similarity, box)。
    matched=None 表示没检测到人脸；box 为 (x, y, w, h) 或 None。"""
    if image is None:
        return None, None, None, None
    face = _largest_face(image)
    if face is None:
        return None, None, None, None
    box = (int(face[0]), int(face[1]), int(face[2]), int(face[3]))
    feat = _feature(image, face)
    db = load_db()
    if not db:
        return False, None, 0.0, box

    rec = _get_recognizer()
    best_name, best_score = None, -1.0
    for item in db:
        score = rec.match(feat, item["feature"], cv2.FaceRecognizerSF_FR_COSINE)
        score = float(score)
        if score > best_score:
            best_score = score
            best_name = item["name"]

    if best_name is not None and best_score >= THRESHOLD:
        return True, best_name, best_score, box
    return False, None, best_score, box


def recognize(image):
    """识别一张人脸。返回 (matched, name, similarity)；
    matched=None 表示没检测到人脸；similarity 为余弦相似度(0~1，越大越像)。"""
    matched, name, similarity, _ = _recognize_core(image)
    return matched, name, similarity


def recognize_boxed(image):
    """识别并返回 (matched, name, similarity, (x, y, w, h))，供实时画框显示。"""
    return _recognize_core(image)


def recognize_all(image):
    """识别画面里所有人脸，返回
    [{"box":(x,y,w,h),"matched":bool,"name":str,"similarity":float}, ...]。
    没人脸返回 []。供 /recognize_multi 画多人绿/红框。"""
    if image is None:
        return []
    faces = _detect_faces(image)
    if faces is None:
        return []
    rec = _get_recognizer()
    db = load_db()
    results = []
    for face in faces:
        box = (int(face[0]), int(face[1]), int(face[2]), int(face[3]))
        feat = _feature(image, face)
        matched, name, sim = False, None, 0.0
        if db:
            best_name, best_score = None, -1.0
            for item in db:
                score = rec.match(feat, item["feature"], cv2.FaceRecognizerSF_FR_COSINE)
                score = float(score)
                if score > best_score:
                    best_score = score
                    best_name = item["name"]
            if best_name is not None and best_score >= THRESHOLD:
                matched, name, sim = True, best_name, best_score
            else:
                sim = best_score
        results.append({"box": list(box), "matched": matched,
                        "name": name or "", "similarity": sim})
    return results


def list_names():
    return [item["name"] for item in load_db()]


def _avatar_filename(name):
    """把姓名转成安全文件名（保留中文/字母数字，其余替换成下划线）"""
    return re.sub(r'[^\w一-鿿-]', '_', name) + ".jpg"


def _save_avatar(name, image, face):
    """把人脸区域裁剪缩成 48x48 小头像存成 JPEG，供 ESP32 显示。"""
    os.makedirs(AVATAR_DIR, exist_ok=True)
    x, y, w, h = int(face[0]), int(face[1]), int(face[2]), int(face[3])
    ih, iw = image.shape[:2]
    m = int(0.25 * max(w, h))  # 外扩一点边，别只裁到脸皮
    x0 = max(0, x - m)
    y0 = max(0, y - m)
    x1 = min(iw, x + w + m)
    y1 = min(ih, y + h + m)
    crop = image[y0:y1, x0:x1]
    if crop.size == 0:
        return
    crop = cv2.resize(crop, (AVATAR_SIZE, AVATAR_SIZE), interpolation=cv2.INTER_AREA)
    ok, buf = cv2.imencode(".jpg", crop, [cv2.IMWRITE_JPEG_QUALITY, 70])
    if ok:
        with open(os.path.join(AVATAR_DIR, _avatar_filename(name)), "wb") as f:
            f.write(buf.tobytes())


def get_avatar_bytes(name):
    """读取某人的头像 JPEG 字节；无头像返回 None。"""
    p = os.path.join(AVATAR_DIR, _avatar_filename(name))
    if not os.path.exists(p):
        return None
    with open(p, "rb") as f:
        return f.read()


def delete_person(name):
    """从人脸库删除某人（同时删头像文件）。返回 (成功?, 消息)。"""
    db = load_db()
    new = [item for item in db if item["name"] != name]
    if len(new) == len(db):
        return False, f"未找到: {name}"
    save_db(new)
    p = os.path.join(AVATAR_DIR, _avatar_filename(name))
    if os.path.exists(p):
        os.remove(p)
    return True, f"已删除: {name}"
