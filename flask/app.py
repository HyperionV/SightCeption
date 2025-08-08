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

    @app.route("/api/capture", methods=["POST"])
    def api_capture():
        # Subscribe to image stream while capturing
        mqtt_handler.start_image_polling()

        # Baseline mtime of current image (to detect a fresh one)
        img_path = os.path.join(os.path.dirname(__file__), "received_images", "current_image.jpg")
        prev_mtime = os.path.getmtime(img_path) if os.path.exists(img_path) else 0

        # Send a capture request to the ESP32-CAM via a control topic
        topic = "sightception/camera/command"
        ok = mqtt_handler.publish_json(topic, {"action": "capture_once", "ts": int(time.time())})
        push_log("server", f"Capture command sent: {ok}")

        # Wait up to 6s for a new image
        deadline = time.time() + 6.0
        latest_url = None
        while time.time() < deadline:
            if os.path.exists(img_path):
                try:
                    m = os.path.getmtime(img_path)
                    if m > prev_mtime:
                        latest_url = url_for('serve_image', filename='current_image.jpg')
                        break
                except Exception:
                    pass
            time.sleep(0.15)

        mqtt_handler.stop_image_polling()
        return jsonify({"ok": bool(ok), "latest_image_url": latest_url})

    @app.route("/api/detect", methods=["POST"])
    def api_detect():
        logging.info("/api/detect called")
        # Subscribe to image while capturing
        mqtt_handler.start_image_polling()

        img_path = os.path.join(os.path.dirname(__file__), "received_images", "current_image.jpg")
        prev_mtime = os.path.getmtime(img_path) if os.path.exists(img_path) else 0

        # Ask camera to capture
        topic = "sightception/camera/command"
        mqtt_handler.publish_json(topic, {"action": "capture_once", "ts": int(time.time())})
        push_log("server", "Detect command: capture_once sent")

        # Wait for fresh image
        deadline = time.time() + 6.0
        while time.time() < deadline:
            if os.path.exists(img_path):
                try:
                    m = os.path.getmtime(img_path)
                    if m > prev_mtime:
                        break
                except Exception:
                    pass
            time.sleep(0.15)

        mqtt_handler.stop_image_polling()

        detected = []
        latest_url = url_for('serve_image', filename='current_image.jpg') if os.path.exists(img_path) else None
        if latest_url:
            logging.info("/api/detect: fresh image detected; running YOLO")
            detected = yolo_detector.predict(img_path)
            if detected:
                try:
                    generate_and_play_audio(current_device_id, detected)
                except Exception as e:
                    logging.warning(f"Audio playback failed in detect API: {e}")
        else:
            logging.info("/api/detect: no fresh image within timeout")
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
        # Werkzeug>=3 removed cache_timeout in favor of max_age
        try:
            return send_from_directory(directory, filename, max_age=0)
        except TypeError:
            # Fallback for older Werkzeug
            return send_from_directory(directory, filename)

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