"""纯 PC 摄像头演示：不依赖 ESP32，先验证识别算法和人脸库。

用法：
  python webcam_demo.py                # 实时识别：窗口显示"已录入/未录入"
  python webcam_demo.py --enroll 张三  # 录入：把当前摄像头里的人脸录入为"张三"
按 q 退出。
"""
import argparse
import cv2
import face_db


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--enroll", metavar="姓名", help="录入模式：录入该姓名后退出")
    args = p.parse_args()

    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        print("无法打开摄像头")
        return

    if args.enroll:
        print(f"[录入模式] 请正对摄像头，录入姓名: {args.enroll}，按 q 放弃")
        while True:
            ok, frame = cap.read()
            if not ok:
                continue
            success, msg = face_db.enroll(args.enroll, frame)
            print(msg)
            for (x, y, w, h) in face_db.detect_boxes(frame):
                color = (0, 255, 0) if success else (0, 0, 255)
                cv2.rectangle(frame, (x, y), (x + w, y + h), color, 2)
            cv2.imshow("enroll", frame)
            if success or (cv2.waitKey(1) & 0xFF == ord("q")):
                break
    else:
        print("[识别模式] 按 q 退出")
        while True:
            ok, frame = cap.read()
            if not ok:
                continue
            matched, name, score = face_db.recognize(frame)
            for (x, y, w, h) in face_db.detect_boxes(frame):
                color = (0, 255, 0) if matched else (0, 0, 255)
                cv2.rectangle(frame, (x, y), (x + w, y + h), color, 2)

            if matched is None:
                label = "未检测到人脸"
            elif matched:
                label = f"{name} (已录入, sim={score:.3f})"
            else:
                label = f"未录入 (sim={score:.3f})"
            cv2.putText(frame, label, (10, 30), cv2.FONT_HERSHEY_SIMPLEX,
                        1, (0, 255, 0), 2)
            cv2.imshow("recognize", frame)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break

    cap.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
