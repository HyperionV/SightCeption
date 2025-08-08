#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>

#define MQTT_MAX_PACKET_SIZE 30000  // Changed back to working version size
#include <PubSubClient.h>

// Wi-Fi credentials
const char* ssid = "VO GIA";
const char* password = "2129301975";

// MQTT Broker
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* device_id = "ESP32-CAM-Client";
const char* publish_topic = "hydroshiba/esp32/cam_image";  // Your existing topic
const char* subscribe_topic = "sightception/device/sightception-esp32-001/signal";  // Listen for wake word

// Pin definition for AI-Thinker ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

WiFiClient espClient;
PubSubClient client(espClient);

// State management
bool imageRequested = false;
unsigned long lastSignalTime = 0;
const unsigned long SIGNAL_TIMEOUT = 10000;  // 10 seconds timeout

void reconnect() {
  // Fixed: Infinite retry like working version, no attempt limit
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    if (client.connect(device_id)) {
      Serial.println(" connected!");
      
      // Subscribe to wake word signal topic
      if (client.subscribe(subscribe_topic)) {
        Serial.println("Successfully subscribed to wake word signals");
        Serial.print("Subscribed to: ");
        Serial.println(subscribe_topic);
      } else {
        Serial.println("Failed to subscribe to wake word signals");
      }
      
    } else {
      Serial.print(" failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);  // Changed to match working version delay
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message received on topic: ");
  Serial.println(topic);
  
  // Convert payload to string
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("Message content: ");
  Serial.println(message);
  
  // Check if this is the wake word signal topic
  if (String(topic) == subscribe_topic) {
    Serial.println("=== WAKE WORD SIGNAL RECEIVED ===");
    
    // Basic validation - check for required JSON fields
    if (message.indexOf("device_id") != -1 && message.indexOf("timestamp") != -1) {
      Serial.println("Valid wake word signal detected - triggering image capture");
      imageRequested = true;
      lastSignalTime = millis();
    } else {
      Serial.println("Invalid signal format - ignoring");
      Serial.println("Expected JSON with device_id and timestamp fields");
    }
  } else {
    Serial.println("Message from unknown topic - ignoring");
  }
}

void captureAndSendImage() {
  Serial.println("Attempting to capture and send image...");
  Serial.printf("Free heap before capture: %u bytes\n", ESP.getFreeHeap());

  // Discard first few frames for color calibration
  for(int i = 0; i < 3; i++) {
    camera_fb_t * temp_fb = esp_camera_fb_get();  // Use different variable name
    if(temp_fb) esp_camera_fb_return(temp_fb);
    delay(200);
  }
  
  // Now capture the actual image
  camera_fb_t * fb = esp_camera_fb_get();  // Single declaration here

  if(!fb) {
    Serial.println("Camera capture failed!");
    return;
  }

  Serial.printf("Image captured successfully! Size: %u bytes\n", fb->len);
  Serial.printf("Free heap after capture: %u bytes\n", ESP.getFreeHeap());

  // Check connection status RIGHT BEFORE publishing
  if (!client.connected()) {
    Serial.println("MQTT not connected - cannot publish image");
    esp_camera_fb_return(fb);
    return;
  }

  // Publish image to your existing topic
  Serial.println("Publishing image...");
  bool published = client.publish(publish_topic, (const uint8_t *)fb->buf, fb->len);

  if(published) {
    Serial.println("✓ Image published successfully!");
    Serial.print("Published to topic: ");
    Serial.println(publish_topic);
    Serial.printf("Image size: %u bytes\n", fb->len);
  } else {
    Serial.println("✗ Image publish failed!");
    Serial.print("MQTT Client State: ");
    Serial.println(client.state());
    Serial.printf("Client connected: %s\n", client.connected() ? "YES" : "NO");
    Serial.printf("Image size: %u bytes\n", fb->len);
    
    // Print common error states
    switch(client.state()) {
      case -4: Serial.println("Error: Connection timeout"); break;
      case -3: Serial.println("Error: Connection lost"); break;
      case -2: Serial.println("Error: Connect failed"); break;
      case -1: Serial.println("Error: Disconnected"); break;
      case 1: Serial.println("Error: Bad protocol"); break;
      case 2: Serial.println("Error: Bad client ID"); break;
      case 3: Serial.println("Error: Unavailable"); break;
      case 4: Serial.println("Error: Bad credentials"); break;
      case 5: Serial.println("Error: Unauthorized"); break;
    }
  }

  // CRITICAL: Always return the frame buffer to prevent memory leaks
  esp_camera_fb_return(fb);
  
  Serial.printf("Free heap after publish: %u bytes\n", ESP.getFreeHeap());
  Serial.println("=== IMAGE CAPTURE COMPLETE ===\n");
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("=== SightCeption ESP32-CAM Starting ===");

  // Camera configuration - Using working version settings
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;  // Keep reduced size for reliability
  config.jpeg_quality = 12;  // Use working version's quality setting
  config.fb_count = 2;

  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }
  Serial.println("Camera initialized successfully");
  Serial.printf("Camera settings: %s resolution, JPEG quality %d\n", 
                config.frame_size == FRAMESIZE_QVGA ? "QVGA (320x240)" : "Unknown", 
                config.jpeg_quality);

  // Wi-Fi connection - Simplified like working version
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // MQTT connection - Fixed buffer size
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setBufferSize(20480);  // FIXED: Back to working version size
  
  Serial.println("Connecting to MQTT...");
  reconnect();
  
  Serial.println("=== Setup Complete ===");
  Serial.println("Waiting for wake word signals...");
  Serial.print("Listening on topic: ");
  Serial.println(subscribe_topic);
  Serial.print("Publishing images to: ");
  Serial.println(publish_topic);
}

void loop() {
  // Handle MQTT connection - Simplified like working version
  if (!client.connected()) {
    Serial.println("MQTT disconnected, attempting reconnection...");
    reconnect();
  }
  client.loop();

  // Handle image capture requests from wake word signals
  if (imageRequested) {
    imageRequested = false;
    Serial.println("=== WAKE WORD TRIGGERED IMAGE CAPTURE ===");
    captureAndSendImage();
  }

  // Timeout old signals (optional safety feature)
  if (lastSignalTime > 0 && (millis() - lastSignalTime > SIGNAL_TIMEOUT)) {
    lastSignalTime = 0;
  }
}

// Test function for debugging (from working version)
void testSmallPublish() {
  if (client.publish("hydroshiba/test", "ESP32-CAM alive")) {
    Serial.println("✓ Test message sent successfully");
  } else {
    Serial.println("✗ Test message failed");
  }
}
