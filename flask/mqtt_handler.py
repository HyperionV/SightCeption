import paho.mqtt.client as mqtt
import logging
import threading
import json
import base64
import os
from datetime import datetime

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

class SightCeptionMQTTHandler:
    """
    Handles MQTT communication for the SightCeption server.
    """
    def __init__(self, broker_host, broker_port, username=None, password=None):
        """
        Initializes the MQTT handler.
        """
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

        # Only set credentials if provided
        if username and password:
            self.client.username_pw_set(username, password)
            logging.info("MQTT authentication configured")
        else:
            logging.info("MQTT configured without authentication (public broker)")

        self.broker_host = broker_host
        self.broker_port = broker_port
        
        self.callbacks = {}
        self.latest_image_path = None
        self.image_received = False
        self.image_received_callback = None  # Callback for immediate image processing
        self.signal_callback = None  # Callback for signal messages
        self.log_callback = None  # Callback for generic log messages

    def _on_connect(self, client, userdata, flags, rc, properties):
        """
        Callback for when the client connects to the broker.
        """
        if rc == 0:
            logging.info("Successfully connected to MQTT Broker!")
            # Subscribe to the specific signal topic
            signal_topic = "sightception/device/sightception-esp32-001/signal"
            self.client.subscribe(signal_topic)
            logging.info(f"Subscribed to signal topic: {signal_topic}")
            # Subscribe to raw image topic (started/stopped explicitly too)
            # Also subscribe to logs by default for dashboard aggregation
            logs_topic = "sightception/logs/#"
            try:
                self.client.subscribe(logs_topic)
                logging.info(f"Subscribed to logs topic: {logs_topic}")
            except Exception as e:
                logging.warning(f"Failed to subscribe to logs topic: {e}")
        else:
            logging.error(f"Failed to connect, return code {rc}")

    def _on_message(self, client, userdata, msg):
        """
        Callback for when a message is received.
        """
        logging.info(f"Received message on topic {msg.topic}")
        try:
            # Handle signal messages
            if msg.topic == "sightception/device/sightception-esp32-001/signal":
                self._handle_signal_message(msg)
                return
            
            # Handle logs aggregation
            if msg.topic.startswith("sightception/logs/"):
                if self.log_callback:
                    try:
                        self.log_callback(msg.topic, msg.payload)
                    except Exception as e:
                        logging.error(f"Error in log callback: {e}")
                return
                
            # Handle raw image data from ESP32 camera
            if msg.topic == "hydroshiba/esp32/cam_image":
                self._handle_raw_image_data(msg)
                return
                
            # Handle image data separately
            if msg.topic.endswith('/image_data'):
                self._handle_image_data(msg)
                return
                
            payload = json.loads(msg.payload.decode())
            logging.info(f"Payload: {payload}")
            
            # Check for a registered callback for this topic
            topic_parts = msg.topic.split('/')
            if len(topic_parts) == 4:
                # e.g., sightception/device/device_id/image_request
                device_id = topic_parts[2]
                event_type = topic_parts[3]
                logging.info(f"Processing {event_type} from device {device_id}")
                
                if event_type in self.callbacks:
                    self.callbacks[event_type](device_id, payload)
                else:
                    logging.warning(f"No callback registered for event type: {event_type}")
        except json.JSONDecodeError as e:
            logging.warning(f"Received non-JSON message on topic {msg.topic}: {msg.payload}")
        except Exception as e:
            logging.error(f"Error processing message on topic {msg.topic}: {e}")

    def _handle_signal_message(self, msg):
        """
        Handle wake word signal messages from ESP32.
        """
        try:
            payload = json.loads(msg.payload.decode())
            logging.info(f"Signal message received: {payload}")
            
            # Extract device_id from the signal
            device_id = payload.get("device_id", "sightception-esp32-001")
            timestamp = payload.get("timestamp", 0)
            
            logging.info(f"Wake word signal from device {device_id} at timestamp {timestamp}")
            
            # Call the signal callback if registered
            if self.signal_callback:
                self.signal_callback(device_id, payload)
            else:
                logging.warning("No signal callback registered")
                
        except json.JSONDecodeError as e:
            logging.error(f"Failed to parse signal message JSON: {e}")
        except Exception as e:
            logging.error(f"Error handling signal message: {e}")

    def _handle_image_data(self, msg):
        """
        Handle incoming image data from ESP32.
        """
        try:
            payload = json.loads(msg.payload.decode())
            image_data_b64 = payload.get('image_data')
            device_id = msg.topic.split('/')[2]
            
            if image_data_b64:
                # Decode base64 image data
                image_data = base64.b64decode(image_data_b64)
                
                # Save image with timestamp
                timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                image_filename = f"received_image_{device_id}_{timestamp}.jpg"
                image_path = os.path.join(os.path.dirname(__file__), "received_images", image_filename)
                
                # Ensure directory exists
                os.makedirs(os.path.dirname(image_path), exist_ok=True)
                
                # Save image
                with open(image_path, 'wb') as f:
                    f.write(image_data)
                
                self.latest_image_path = image_path
                self.image_received = True
                logging.info(f"Image saved: {image_path}")
            else:
                logging.warning("No image data in payload")
                
        except Exception as e:
            logging.error(f"Error handling image data: {e}")

    def _handle_raw_image_data(self, msg):
        """
        Handle raw image data from ESP32 camera.
        """
        try:
            # The payload is raw image data
            image_data = msg.payload
            
            # Save image with fixed name (overwrites previous image)
            image_filename = "current_image.jpg"
            image_path = os.path.join(os.path.dirname(__file__), "received_images", image_filename)
            
            # Ensure directory exists
            os.makedirs(os.path.dirname(image_path), exist_ok=True)
            
            # Save image (overwrites if exists)
            with open(image_path, 'wb') as f:
                f.write(image_data)
            
            self.latest_image_path = image_path
            self.image_received = True
            logging.info(f"Raw image saved: {image_path}")
            
            # Trigger immediate detection if callback is set
            if self.image_received_callback:
                self.image_received_callback()
                
        except Exception as e:
            logging.error(f"Error handling raw image data: {e}")

    def register_callback(self, event_type, function):
        """
        Register a callback function for a specific event type.
        e.g., event_type = "image_request"
        """
        self.callbacks[event_type] = function
        logging.info(f"Callback registered for event type: {event_type}")

    def register_signal_callback(self, function):
        """
        Register a callback function for signal messages.
        """
        self.signal_callback = function
        logging.info("Signal callback registered")

    def register_log_callback(self, function):
        """
        Register a callback function for logs messages.
        The callback signature should be: func(topic: str, payload: bytes)
        """
        self.log_callback = function
        logging.info("Log callback registered")

    def connect(self):
        """
        Connect to the MQTT broker and start the loop in a background thread.
        """
        try:
            logging.info(f"Connecting to {self.broker_host}:{self.broker_port}...")
            self.client.connect(self.broker_host, self.broker_port)
            # Start the loop in a non-blocking way
            thread = threading.Thread(target=self.client.loop_forever)
            thread.daemon = True
            thread.start()
            logging.info("MQTT client loop started in background thread")
        except Exception as e:
            logging.error(f"Could not connect to MQTT broker: {e}")

    def publish(self, topic, payload):
        """
        Publish a message to a specific topic.
        """
        try:
            if not isinstance(payload, str):
                payload = json.dumps(payload)
            result = self.client.publish(topic, payload)
            status = result[0]
            if status == 0:
                logging.info(f"Successfully sent message to topic {topic}")
            else:
                logging.warning(f"Failed to send message to topic {topic}")
            return status == 0
        except Exception as e:
            logging.error(f"Error publishing to topic {topic}: {e}")
            return False

    def get_latest_image_path(self):
        """
        Get the path to the latest received image.
        """
        return self.latest_image_path

    def has_received_image(self):
        """
        Check if an image has been received.
        """
        return self.image_received

    def set_image_received_callback(self, callback):
        """
        Set callback to be called immediately when image is received.
        """
        self.image_received_callback = callback

    def clear_image_received_callback(self):
        """
        Clear the image received callback.
        """
        self.image_received_callback = None

    def start_image_polling(self):
        """
        Subscribe to image topic to start receiving images.
        """
        image_topic = "hydroshiba/esp32/cam_image"
        self.client.subscribe(image_topic)
        logging.info(f"Started image polling - subscribed to: {image_topic}")

    def stop_image_polling(self):
        """
        Unsubscribe from image topic to stop receiving images.
        """
        image_topic = "hydroshiba/esp32/cam_image"
        self.client.unsubscribe(image_topic)
        logging.info(f"Stopped image polling - unsubscribed from: {image_topic}") 

    def publish_json(self, topic, payload_dict):
        """Publish a JSON payload (dict) convenience helper."""
        try:
            return self.publish(topic, payload_dict)
        except Exception as e:
            logging.error(f"publish_json error on {topic}: {e}")
            return False