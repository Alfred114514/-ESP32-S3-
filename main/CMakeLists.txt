idf_component_register(SRCS "log_report.c" "face_ui.c" "lvgl_port.c" "encoder.c" "face_client.c" "local_face_recog.cpp" "local_face_http.c" "door_ctrl.c" "mqtt.c" "wifi_http.c" "main.c" "led/led.c" "camera/camera.c" "lvgl_demo.c" "lcd.c" "servo.c" "fonts/lv_font_face_cjk_16.c"
                    INCLUDE_DIRS "." "led" "camera"
                    REQUIRES mqtt esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_http_client json esp_psram human_face_detect human_face_recognition)
