import logging
import time
import os
import base64
import glob
from io import BytesIO
from datetime import datetime
from gtts import gTTS
import pygame

from mqtt_handler import SightCeptionMQTTHandler
from yolo_api import YOLODetector
from flask import Flask, jsonify, send_from_directory, request, render_template_string, url_for

# --- Configuration ---
BROKER_HOST = "broker.hivemq.com"
BROKER_PORT = 1883
USERNAME = None  
PASSWORD = None

# Global variables for flow control
current_device_id = None
image_fetching_active = False


def empty_image_folder():
    """
    Empty the received_images folder to ensure no old images are retained.
    """
    image_folder = os.path.join(os.path.dirname(__file__), "received_images")
    if os.path.exists(image_folder):
        for file in glob.glob(os.path.join(image_folder, "*.jpg")):
            try:
                os.remove(file)
            except:
                pass


def on_image_received():
    """
    Called when an image is received. Stop fetching and do object detection.
    """
    global image_fetching_active, current_device_id
    
    if not image_fetching_active:
        return
    
    logging.info("Image received, stopping fetch and starting detection")
    image_fetching_active = False
    mqtt_handler.stop_image_polling()
    
    # Do object detection
    current_image_path = os.path.join(os.path.dirname(__file__), "received_images", "current_image.jpg")
    if os.path.exists(current_image_path):
        logging.info(f"Processing image: {current_image_path}")
        
        # Process the image with YOLO
        detected_objects = yolo_detector.predict(current_image_path)
        logging.info(f"Detection complete. Found: {detected_objects}")
        
        # # Delete image after detection
        # try:
        #     os.remove(current_image_path)
        #     logging.info("Image deleted after detection")
        # except:
        #     pass
        
        # Handle detection result
        if detected_objects:
            # Objects detected - generate audio and play
            generate_and_play_audio(current_device_id, detected_objects)
        else:
            # No objects detected - continue fetching
            logging.info("No objects detected, continuing to fetch images")
            start_image_fetching()


def generate_and_play_audio(device_id, detected_objects):
    """
    Generate TTS audio, save it, and play it.
    """
    text_to_speak = f"I detected {', and '.join(detected_objects)}."
    logging.info(f"Generating TTS audio: '{text_to_speak}'")
    
    try:
        # Generate TTS audio
        tts = gTTS(text=text_to_speak, lang='en')
        audio_buffer = BytesIO()
        tts.write_to_fp(audio_buffer)
        audio_buffer.seek(0)
        
        # Save audio to file
        audio_path = os.path.join(os.path.dirname(__file__), "received_images", "detection_audio.mp3")
        with open(audio_path, 'wb') as f:
            f.write(audio_buffer.getvalue())
        logging.info(f"Audio saved to: {audio_path}")
        
        # Play the audio
        try:
            pygame.mixer.init()
            pygame.mixer.music.load(audio_path)
            pygame.mixer.music.play()
            logging.info("Playing detection audio...")
            
            # Wait for audio to finish playing
            while pygame.mixer.music.get_busy():
                pygame.time.Clock().tick(10)
            
            pygame.mixer.quit()
            logging.info("Audio playback completed")
            
            # # Clean up audio file after playing
            # try:
            #     os.remove(audio_path)
            #     logging.info("Audio file cleaned up")
            # except Exception as e:
            #     logging.warning(f"Failed to clean up audio file: {e}")
                
        except Exception as e:
            logging.error(f"Failed to play audio: {e}")
            pygame.mixer.quit()
            
    except Exception as e:
        logging.error(f"Failed to generate audio: {e}")


def start_image_fetching():
    """
    Start fetching images every 5 seconds.
    """
    global image_fetching_active
    
    if image_fetching_active:
        return  # Already fetching
    
    logging.info("Starting image fetching (every 5 seconds)")
    image_fetching_active = True
    mqtt_handler.set_image_received_callback(on_image_received)
    mqtt_handler.start_image_polling()


def on_signal_received(device_id, signal_data):
    """
    Callback function to handle wake word signals from ESP32.
    Starts the image fetching flow.
    """
    global current_device_id
    
    logging.info(f"Received wake word signal from device {device_id}")
    logging.info(f"Signal data: {signal_data}")

    # Set current device info
    current_device_id = device_id

    # Empty image folder to ensure no old images are retained
    empty_image_folder()
    logging.info("Image folder emptied for new signal")

    # Start image fetching
    start_image_fetching()


if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO, 
        format='%(asctime)s - %(levelname)s - %(message)s'
    )
    
    print("\n" + "="*60)
    print("SIGHTCEPTION SERVER INITIALIZING")
    print("="*60)

    # --- Dashboard & API ---
    app = Flask(__name__)

    # In-memory activity log (simple ring buffer)
    ACTIVITY_LOG_MAX = 200
    activity_log = []

    def push_log(source, message):
        ts = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        entry = {"time": ts, "source": source, "message": message}
        activity_log.append(entry)
        if len(activity_log) > ACTIVITY_LOG_MAX:
            del activity_log[:len(activity_log)-ACTIVITY_LOG_MAX]
        logging.info(f"LOG[{source}]: {message}")

    # MQTT log callback
    def on_log_message(topic: str, payload: bytes):
        try:
            msg = payload.decode(errors='ignore')
        except Exception:
            msg = str(payload)
        push_log(source=topic, message=msg)

    DASHBOARD_HTML = """
<!doctype html>
<html>
  <head>
    <meta charset="utf-8"/>
    <meta name="viewport" content="width=device-width, initial-scale=1"/>
    <title>SightCeption Dashboard</title>
    <style>
      body { font-family: system-ui, -apple-system, Segoe UI, Roboto, Arial; margin: 24px; }
      h1 { margin-bottom: 8px; }
      .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 16px; }
      .card { border: 1px solid #ddd; border-radius: 8px; padding: 16px; background: #fff; }
      button { padding: 10px 14px; border: 0; border-radius: 6px; background: #2563eb; color: #fff; cursor: pointer; }
      button.secondary { background: #334155; }
      img { max-width: 100%; border-radius: 6px; border: 1px solid #eee; }
      pre { background: #0b1020; color: #e5e7eb; padding: 12px; border-radius: 6px; max-height: 360px; overflow: auto; }
      .row { display:flex; gap:8px; flex-wrap: wrap; }
      .pill { font-size: 12px; padding: 2px 8px; background: #eef2ff; color: #3730a3; border-radius: 9999px; }
    </style>
  </head>
  <body>
    <h1>SightCeption Dashboard</h1>
    <div class="row">
      <span class="pill">Broker: {{ broker }}</span>
      <span class="pill">Device: {{ device }}</span>
    </div>
    <br/>
    <div class="grid">
      <div class="card">
        <h3>1) Adjust Camera Angle</h3>
        <p>Request a fresh frame from the camera to help adjust the angle.</p>
        <div class="row">
          <button id="btn-capture">Capture Image</button>
          <button class="secondary" id="btn-refresh">Refresh Preview</button>
        </div>
        <p></p>
        <div id="preview"></div>
      </div>

      <div class="card">
        <h3>2) Test Object Detection</h3>
        <p>Trigger capture, run YOLO on server, show result and announce it.</p>
        <button id="btn-detect">Run Detection</button>
        <div id="detect-result"></div>
      </div>

      <div class="card">
        <h3>3) Activity Log</h3>
        <p>Latest system events aggregated from MQTT logs.</p>
        <pre id="log"></pre>
      </div>
    </div>

    <script>
      async function capture() {
        const r = await fetch('/api/capture', {method: 'POST'});
        const j = await r.json();
        return j;
      }
      async function detect() {
        const r = await fetch('/api/detect', {method: 'POST'});
        const j = await r.json();
        return j;
      }
      async function loadStatus() {
        const r = await fetch('/api/status');
        const j = await r.json();
        return j;
      }

      function renderPreview(status) {
        const el = document.getElementById('preview');
        if (status.latest_image_url) {
          el.innerHTML = `<img src="${status.latest_image_url}?t=${Date.now()}" alt="latest image"/>`;
        } else {
          el.innerHTML = '<em>No image yet</em>';
        }
      }
      function renderLog(lines) {
        const el = document.getElementById('log');
        el.textContent = (lines || []).map(x => `[${x.time}] ${x.source}: ${x.message}`).reverse().join('\n');
      }
      function renderDetect(res) {
        const el = document.getElementById('detect-result');
        if (!res) { el.innerHTML = ''; return; }
        const img = res.latest_image_url ? `<img src="${res.latest_image_url}?t=${Date.now()}"/>` : '';
        el.innerHTML = `<p><strong>Detected:</strong> ${(res.detected || []).join(', ') || 'None'}</p>${img}`;
      }

      document.getElementById('btn-capture').addEventListener('click', async () => {
        const j = await capture();
        const st = await loadStatus();
        renderPreview(st);
      });
      document.getElementById('btn-refresh').addEventListener('click', async () => {
        const st = await loadStatus();
        renderPreview(st);
      });
      document.getElementById('btn-detect').addEventListener('click', async () => {
        const j = await detect();
        renderDetect(j);
      });

      async function tick() {
        const st = await loadStatus();
        renderLog(st.activity);
        setTimeout(tick, 1500);
      }
      (async () => {
        const st = await loadStatus();
        renderPreview(st);
        renderLog(st.activity);
        tick();
      })();
    </script>
  </body>
</html>
"""

    @app.route("/dashboard")
    def dashboard():
        return render_template_string(DASHBOARD_HTML, broker=f"{BROKER_HOST}:{BROKER_PORT}", device=current_device_id or "(not set)")

    @app.route("/api/capture", methods=["POST"])
    def api_capture():
        # Send a capture request to the ESP32-CAM via a control topic
        topic = "sightception/camera/command"
        ok = mqtt_handler.publish_json(topic, {"action": "capture_once", "ts": int(time.time())})
        push_log("server", f"Capture command sent: {ok}")
        return jsonify({"ok": bool(ok)})

    @app.route("/api/detect", methods=["POST"])
    def api_detect():
        # Step 1: Ask camera to capture
        topic = "sightception/camera/command"
        mqtt_handler.publish_json(topic, {"action": "capture_once", "ts": int(time.time())})
        push_log("server", f"Detect command: capture_once sent")
        # Step 2: Wait briefly to let image arrive if camera is online
        time.sleep(0.8)

        # Use latest current_image.jpg if available
        img_path = os.path.join(os.path.dirname(__file__), "received_images", "current_image.jpg")
        detected = []
        if os.path.exists(img_path):
            detected = yolo_detector.predict(img_path)
            if detected:
                # announce locally
                try:
                    generate_and_play_audio(current_device_id, detected)
                except Exception as e:
                    logging.warning(f"Audio playback failed in detect API: {e}")
        latest_url = url_for('serve_image', filename='current_image.jpg') if os.path.exists(img_path) else None
        push_log("server", f"Detection result: {detected}")
        return jsonify({"detected": detected, "latest_image_url": latest_url})

    @app.route("/api/status")
    def api_status():
        img_dir = os.path.join(os.path.dirname(__file__), "received_images")
        latest_url = None
        latest_path = os.path.join(img_dir, "current_image.jpg")
        if os.path.exists(latest_path):
            latest_url = url_for('serve_image', filename='current_image.jpg')
        return jsonify({
            "broker": f"{BROKER_HOST}:{BROKER_PORT}",
            "device_id": current_device_id,
            "latest_image_url": latest_url,
            "activity": activity_log[-ACTIVITY_LOG_MAX:]
        })

    @app.route('/images/<path:filename>')
    def serve_image(filename):
        directory = os.path.join(os.path.dirname(__file__), 'received_images')
        return send_from_directory(directory, filename, cache_timeout=0)

    # Initialize components
    logging.info("Initializing YOLO detector...")
    yolo_detector = YOLODetector()
    
    logging.info(f"Connecting to MQTT broker: {BROKER_HOST}:{BROKER_PORT}")
    mqtt_handler = SightCeptionMQTTHandler(
        broker_host=BROKER_HOST,
        broker_port=BROKER_PORT,
        username=USERNAME,
        password=PASSWORD
    )

    # Register callbacks
    mqtt_handler.register_signal_callback(on_signal_received)
    mqtt_handler.register_log_callback(on_log_message)

    # Connect and run
    mqtt_handler.connect()

    # Console banner with dashboard URL
    print("="*60)
    print("SERVER IS RUNNING AND WAITING FOR WAKE WORD SIGNALS")
    print("Press button on ESP32 to trigger wake word detection")
    print("Server will fetch images until objects detected")
    print(f"Dashboard: http://127.0.0.1:5000/dashboard")
    print("="*60)

    try:
        app.run(host="127.0.0.1", port=5000, debug=False)
    except KeyboardInterrupt:
        print("\n" + "="*60)
        print("SERVER SHUTTING DOWN")
        print("="*60)
        
        # Clean up
        if image_fetching_active:
            image_fetching_active = False
            mqtt_handler.stop_image_polling()
            mqtt_handler.clear_image_received_callback()