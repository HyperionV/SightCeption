# SightCeption - AI-Powered Object Detection for Visually Impaired

An assistive technology project that helps visually impaired individuals identify objects through smart glasses with AI-powered object recognition. This implementation is an MVP simulation using Wokwi VSCode extension before building the actual hardware prototype.

## Project Structure

```
SightCeption/
├── circuit/              # Wokwi simulation circuit files and ESP32 firmware
│   ├── diagram.json      # Wokwi circuit diagram
│   ├── esp32_firmware.ino # ESP32 firmware code
│   └── wokwi.toml        # Wokwi configuration
└── flask/                # Flask server backend
    ├── app.py            # Main Flask application
    ├── mqtt_handler.py   # MQTT communication handler
    ├── yolo_api.py       # Object detection via Ultralytics YOLO11
    ├── test_mqtt.py      # MQTT testing utility
    └── requirements.txt  # Python dependencies
```

## System Architecture

**Hardware Simulation Flow:**

1. User presses button on ESP32 → LED indicator activates
2. ESP32 connects to WiFi → Establishes MQTT connection
3. Button press triggers wake word simulation → Publishes MQTT message
4. Flask server receives request → Processes with YOLO11 API
5. Server generates TTS audio → Sends response via MQTT
6. ESP32 plays audio through buzzer patterns

**MQTT Communication:**

- **Broker**: `broker.hivemq.com:1883` (public broker for MVP)
- **Device Topics**: `sightception/device/{device_id}/wake_detected`, `image_request`
- **Server Topics**: `sightception/server/{device_id}/audio_data`, `image_response`

## Prerequisites

### Software Requirements

- **VSCode** with Wokwi extension
- **Python 3.8+** with pip
- **Arduino libraries**: WiFi, PubSubClient

### Python Dependencies

```bash
pip install -r flask/requirements.txt
```

## Quick Start

### 1. ESP32 Simulation (Wokwi)

**Start the simulation:**

```bash
# Open circuit directory in VSCode
code circuit/

# Press F1 → "Wokwi: Start Simulator"
# Or use Ctrl+Shift+P → "Wokwi: Start Simulator"
```

**Expected Behavior:**

- ESP32 connects to WiFi (Wokwi-GUEST)
- Establishes MQTT connection to broker.hivemq.com
- Green button press → LED lights up → MQTT messages sent
- Serial monitor shows connection status and message flow

### 2. Flask Server Testing

**Option A: Quick MQTT Test**

```bash
cd flask
python test_mqtt.py
```

This will connect to the MQTT broker and simulate server responses when you press the ESP32 button.

**Option B: Full Flask Server**

```bash
cd flask
python app.py
```

### 3. Testing the Complete Flow

1. **Start the MQTT test server**: `python flask/test_mqtt.py`
2. **Start Wokwi simulation** in VSCode
3. **Wait for connections**: Both should show "Connected to MQTT broker"
4. **Press the green button** in Wokwi
5. **Observe the flow**:
   - LED lights up on ESP32
   - Serial monitor shows wake word detection
   - MQTT messages appear in test server
   - Server responds with mock object detection
   - ESP32 plays buzzer tones for audio response

## Hardware Configuration

### ESP32 Pin Mapping

- **Button**: GPIO 15 (INPUT_PULLUP)
- **LED**: GPIO 2 (Status indicator)
- **Buzzer**: GPIO 4 (Audio output simulation)

### Wokwi Circuit

The `diagram.json` includes:

- ESP32 DevKit V1
- Push button with pull-up resistor
- LED with current-limiting resistor
- Buzzer for audio feedback

## Troubleshooting

### ⚠️ Common Issue: ESP32 Hangs After Button Press

**Symptoms:**

- Button press lights up LED ✅
- No serial monitor output after button press ❌
- No MQTT messages sent ❌
- ESP32 appears to freeze ❌

**Root Cause:**
The ESP32 code was hanging during MQTT connection attempts in Wokwi simulation due to blocking network operations without proper timeouts.

**✅ Solution Implemented:**
The firmware now includes:

1. **Non-blocking MQTT connection** with 10-second timeout
2. **Detailed debug output** to track execution flow
3. **Proper error handling** for connection failures
4. **Retry mechanism** with 5-second intervals
5. **Connection state management** to prevent hangs

**Key Code Changes:**

```cpp
// Non-blocking MQTT with timeout
bool connectMQTTWithTimeout() {
  unsigned long startTime = millis();

  if (client.connect(device_id)) {
    return true;
  }

  if (millis() - startTime > connectionTimeout) {
    Serial.println("MQTT connection timeout");
    return false;
  }

  return false;
}

// Separate connection handling in main loop
void handleMQTTConnection() {
  if (!mqttConnected && (millis() - lastConnectionAttempt > 5000)) {
    // Attempt connection with timeout
    if (connectMQTTWithTimeout()) {
      mqttConnected = true;
      // Subscribe to topics
    }
  }
}
```

### Debug Steps

1. **Check Serial Monitor Output:**

   ```
   === SightCeption ESP32 Starting ===
   Pins initialized
   WiFi connected!
   Setup complete - ready for button press
   ```

2. **Test MQTT Connectivity:**

   ```bash
   cd flask
   python test_mqtt.py
   # Should show: ✅ Connected to MQTT broker successfully!
   ```

3. **Verify Button Response:**
   - Press button in Wokwi
   - Check for "=== BUTTON PRESSED ===" in serial output
   - LED should light up for 2 seconds

### Network Issues

**WiFi Connection Failed:**

- Ensure using "Wokwi-GUEST" network in code
- Check Wokwi IoT Gateway is enabled in `wokwi.toml`

**MQTT Connection Failed:**

- Verify broker.hivemq.com is accessible
- Check firewall/proxy settings
- Try alternative broker: `test.mosquitto.org`

## Development Notes

### YOLO Integration

- Uses Ultralytics YOLO11 model via direct library integration
- Processes test images from `flask/test_images/` directory
- Generates TTS audio using gTTS library

### MQTT Architecture

- **Pub/Sub Pattern**: Device publishes requests, server publishes responses
- **JSON Payloads**: Structured data with request IDs for correlation
- **Topic Hierarchy**: Organized by device/server and function

### Simulation Limitations

- **Audio**: Simulated via buzzer tone patterns
- **Camera**: Uses pre-stored test images
- **Wake Word**: Simulated by button press
- **Network**: Uses Wokwi's virtual WiFi environment

## Hardware Migration

When migrating to actual hardware:

1. **Replace Wokwi-GUEST** with actual WiFi credentials
2. **Add real camera module** (ESP32-CAM recommended)
3. **Integrate actual wake word detection** (Edge Impulse)
4. **Add proper audio output** (I2S DAC + speaker)
5. **Update MQTT broker** to production instance (AWS IoT Core/HiveMQ Cloud)

## License

This project is part of an academic IoT course implementation.

---

**Status**: ✅ **RESOLVED** - MQTT hanging issue fixed with non-blocking connection logic and proper timeout handling.
