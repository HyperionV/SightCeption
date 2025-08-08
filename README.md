# SightCeption - AI Assistive Object Detection (MVP)

An assistive system that lets a user trigger a camera capture, run on-device/edge object detection on a server, and announce results via text-to-speech. This repo contains two ESP32 firmwares and a Python backend with both a Flask dashboard and a Streamlit UI.

## Project Structure

```
SightCeption/
├── circuit/
│   ├── SightCeption/                 # ESP32 DevKit/WROOM (wake word + signal + logs) - PlatformIO
│   └── sightception-cam/             # ESP32-CAM AI Thinker (capture + image publish + logs) - PlatformIO
└── flask/                            # Python backend
    ├── app.py                        # Flask app: REST API + HTML dashboard (/dashboard)
    ├── mqtt_handler.py               # MQTT client: image/command/logs wiring
    ├── yolo_api.py                   # YOLO11 inference helper
    ├── streamlit_app.py              # Streamlit dashboard (optional UI)
    ├── received_images/current_image.jpg
    └── requirements.txt
```

## System Architecture (current)

- ESP32 WROOM (wake word): publishes a wake-word signal.
- ESP32-CAM: subscribes to wake-word and to a server command; captures a JPEG and publishes raw bytes.
- Flask server: subscribes to image bytes, saves to `flask/received_images/current_image.jpg`, runs YOLO, generates TTS with gTTS, and provides a dashboard.

### Dashboard features

- Adjust Camera Angle: triggers a fresh capture and shows the new frame.
- Test Object Detection: triggers capture → runs YOLO → displays detected classes and speaks them.
- Activity Log: aggregates live MQTT logs from devices and server.

## MQTT Topic Map

- Broker: `broker.hivemq.com:1883`
- Wakeword signal (ESP32 WROOM → all):
  - `sightception/device/sightception-esp32-001/signal`
- Server command to ESP32-CAM (capture-on-demand):
  - `sightception/camera/command` (JSON: `{ "action": "capture_once" }`)
- ESP32-CAM image publish (unchanged, raw JPEG bytes):
  - `hydroshiba/esp32/cam_image`
- Activity logs (live feed shown on dashboard):
  - `sightception/logs/esp32wroom`
  - `sightception/logs/esp32cam`
  - `sightception/logs/server`

## Backend REST API (Flask)

- `POST /api/capture` — Sends capture command and waits briefly for a fresh image.
- `POST /api/detect` — Capture → YOLO detect → returns `{ detected: string[], latest_image_url }` and plays TTS locally.
- `GET /api/status` — Returns `{ latest_image_url, activity, broker, device }`.
- `GET /images/current_image.jpg` — Serves last received frame (cache-busted by the UIs).

## Getting Started

### 1) Install backend deps

```bash
pip install -r flask/requirements.txt
```

### 2) Run the Flask backend

```bash
cd flask
python app.py
```

### 3) Streamlit dashboard

```bash
streamlit run flask/streamlit_app.py
# Set the backend URL in the sidebar (default http://127.0.0.1:5000/)
```

### 4) Flash the devices (PlatformIO)

- Open `circuit/SightCeption/` and `circuit/sightception-cam/` in VSCode with PlatformIO.
- Configure Wi‑Fi and broker if needed.
- Build & upload each firmware to the respective board.

## How it works (end-to-end)

1. Wake word on ESP32 WROOM publishes to `sightception/device/sightception-esp32-001/signal`.
2. ESP32-CAM listens to the signal; it also listens to server command `sightception/camera/command`.
3. When the dashboard sends “Capture Image” or “Run Detection”, the server publishes `{action:"capture_once"}` to the command topic.
4. ESP32-CAM captures a frame and publishes raw JPEG bytes to `hydroshiba/esp32/cam_image`.
5. Flask receives, writes `flask/received_images/current_image.jpg`, updates the dashboard.
6. For detection, Flask runs YOLO11 and announces results via gTTS + pygame.
7. All components push logs to `sightception/logs/#`, shown on the dashboard.

## Notes

- Images are written to `flask/received_images/current_image.jpg` and served via `/images/current_image.jpg`.
- YOLO model is loaded by `flask/yolo_api.py` (Ultralytics YOLO11).
- TTS output is saved to `flask/received_images/detection_audio.mp3` and played locally by the server.
