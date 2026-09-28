import cv2

# ESP32 AP 模式下固定地址（必须先连上 ESP32-CAM 这个 WiFi）
URL = "http://192.168.4.1/stream"

cap = cv2.VideoCapture(URL)
if not cap.isOpened():
    print("连不上摄像头流，请确认电脑已连接 ESP32-CAM 热点")
    exit(1)

print("按 q 退出")
while True:
    ret, frame = cap.read()
    if not ret:
        # 断流了，尝试重连
        print("读取失败，重连中...")
        cap.release()
        cap = cv2.VideoCapture(URL)
        continue

    cv2.imshow("ESP32-CAM", frame)   # 本地窗口显示
    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

cap.release()
cv2.destroyAllWindows()
