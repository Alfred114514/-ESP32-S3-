@echo off
setlocal
set MSYSTEM=
set IDF_TOOLS_PATH=E:\esp32ssif\new\Espressif
set IDF_PATH=E:\esp32ssif\new\Espressif\frameworks\esp-idf-v5.5.5
set "PATH=E:\esp32ssif\new\Espressif\python_env\idf5.5_py3.11_env\Scripts;E:\esp32ssif\new\Espressif\tools\idf-git\2.44.0\cmd;%PATH%"
call "%IDF_PATH%\export.bat" >nul 2>&1
cd /d D:\cubemx\esp32-s3\esp32_video\esp32_cam_video
python "%IDF_PATH%\tools\idf.py" build
