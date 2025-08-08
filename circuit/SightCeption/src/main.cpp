#include <Arduino.h> 
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <driver/i2s.h>
#include <PubSubClient.h>
#include <sightception-wakeword_inferencing.h>

const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* device_id = "sightception-esp32-001";
const char* topic_signal = "sightception/device/sightception-esp32-001/signal";
const char* topic_logs   = "sightception/logs/esp32wroom"; // new: activity logs

WiFiClient espClient;
PubSubClient client(espClient);

// MQTT connection state
bool mqttConnected = false;
unsigned long lastMqttAttempt = 0;
const unsigned long MQTT_RETRY_INTERVAL = 5000;

// WiFi credentials
const char* ssid = "VO GIA";
const char* password = "2129301975";

// I2S pins
#define I2S_WS 22
#define I2S_SCK 19
#define I2S_SD 21

// Control pins
#define BUTTON_PIN 4
#define BUZZER_PIN 2  // Changed from LED_PIN to BUZZER_PIN

// Buzzer configuration
#define BUZZER_CHANNEL 0
#define BUZZER_FREQUENCY 2000  // 2kHz tone
#define BUZZER_RESOLUTION 8

#define SAMPLE_RATE EI_CLASSIFIER_FREQUENCY  
#define SAMPLE_BITS 16
#define RECORD_TIME 1
#define FILENAME "/record.wav"

#define WAKEWORD_THRESHOLD 0.4
const int wakewordSamples = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
const float WAKEWORD_TIME = (float)EI_CLASSIFIER_RAW_SAMPLE_COUNT / SAMPLE_RATE;

bool wakewordDetected = false;
unsigned long lastDetectionTime = 0;

WebServer server(80);

// Button handling
volatile bool buttonPressed = false;
unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 200;

// Audio buffer for wake word detection
int16_t* audioBuffer = nullptr;

// Buzzer control functions
void setupBuzzer() {
  // Configure PWM for buzzer control
  ledcSetup(BUZZER_CHANNEL, BUZZER_FREQUENCY, BUZZER_RESOLUTION);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWrite(BUZZER_CHANNEL, 0); // Start with buzzer off
}

void buzzerOn() {
  ledcWrite(BUZZER_CHANNEL, 128); // 50% duty cycle for moderate volume
}

void buzzerOff() {
  ledcWrite(BUZZER_CHANNEL, 0);
}

void buzzerBeep(int duration_ms) {
  buzzerOn();
  delay(duration_ms);
  buzzerOff();
}

void buzzerPattern() {
  // Wake word detection pattern - 3 quick beeps followed by long beep
  for (int i = 0; i < 3; i++) {
    buzzerBeep(100);
    delay(100);
  }
  buzzerBeep(500); // Longer final beep
}

void buzzerError() {
  // Error pattern - alternating high and low tones
  for (int i = 0; i < 2; i++) {
    ledcWriteTone(BUZZER_CHANNEL, 1000);
    delay(200);
    ledcWriteTone(BUZZER_CHANNEL, 500);
    delay(200);
  }
  buzzerOff();
}

void IRAM_ATTR buttonISR() {
  unsigned long currentTime = millis();
  if (currentTime - lastButtonPress > debounceDelay) {
    buttonPressed = true;
    lastButtonPress = currentTime;
  }
}

// Function declarations
void setupI2S();
void printLittleFSInfo();
int get_signal_data(size_t offset, size_t length, float* out_ptr);
void performWakewordDetection();
void handleRoot();
void handleTestWakeword();
void handleWakewordStatus();
bool writeWavHeader(File &file, int dataSize, int sampleRate);
void handleRecord();
void handleDownload();
void handleStatus();
void handleFormat();

void connectToMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping MQTT");
    return;
  }
  
  Serial.print("Attempting MQTT connection...");
  if (client.connect(device_id)) {
    Serial.println(" connected!");
    mqttConnected = true;
    client.publish(topic_logs, "esp32wroom: connected");
  } else {
    Serial.print(" failed, rc=");
    Serial.print(client.state());
    Serial.println(" retrying later");
    mqttConnected = false;
  }
}

void publishWakeWordSignal() {
  if (!client.connected()) {
    Serial.println("MQTT not connected, cannot send wake word signal");
    return;
  }
  
  String message = "{";
  message += "\"device_id\":\"" + String(device_id) + "\",";
  message += "\"timestamp\":" + String(millis());
  message += "}";
  
  Serial.println("Publishing wake word signal...");
  Serial.println("Message: " + message);
  
  if (client.publish(topic_signal, message.c_str())) {
    Serial.println("Wake word signal published successfully!");
    client.publish(topic_logs, "esp32wroom: wakeword signal published");
  } else {
    Serial.println("Failed to publish wake word signal");
    client.publish(topic_logs, "esp32wroom: wakeword signal publish failed");
  }
}

void setupI2S() {
  i2s_driver_uninstall(I2S_NUM_0);
  
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("I2S driver install failed: %d\n", err);
    return;
  }
  
  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("I2S pin config failed: %d\n", err);
    return;
  }
  
  i2s_zero_dma_buffer(I2S_NUM_0);
}

int get_signal_data(size_t offset, size_t length, float* out_ptr) {
  for (size_t i = 0; i < length; i++) {
    if (offset + i < (size_t)wakewordSamples) {
      out_ptr[i] = (float)audioBuffer[offset + i] / 32768.0f;
    } else {
      out_ptr[i] = 0.0f;
    }
  }
  return 0;
}

void printLittleFSInfo() {
  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  float usage = (float)usedBytes / totalBytes * 100.0;
  
  Serial.printf("LittleFS Total: %d bytes\n", totalBytes);
  Serial.printf("LittleFS Used: %d bytes (%.1f%%)\n", usedBytes, usage);
  Serial.printf("LittleFS Free: %d bytes\n", totalBytes - usedBytes);
}

void performWakewordDetection() {
  Serial.println("=== Starting Wake Word Detection ===");
  Serial.printf("Recording %d samples for %.3f seconds...\n", wakewordSamples, WAKEWORD_TIME);
  
  unsigned long startTime = millis();
  
  i2s_zero_dma_buffer(I2S_NUM_0);
  delay(10);
  
  size_t bytesRead;
  int totalSamples = 0;
  uint8_t tempBuffer[512]; 
  
  memset(audioBuffer, 0, wakewordSamples * sizeof(int16_t));
  
  while (totalSamples < wakewordSamples) {
    esp_err_t result = i2s_read(I2S_NUM_0, tempBuffer, sizeof(tempBuffer), &bytesRead, pdMS_TO_TICKS(100));
    if (result != ESP_OK) {
      Serial.printf("I2S read error: %d\n", result);
      buzzerError();
      return;
    }
    
    if (bytesRead == 0) continue;
    
    int samplesInBuffer = bytesRead / 2;
    int samplesToCopy = min(samplesInBuffer, wakewordSamples - totalSamples);
    
    memcpy(&audioBuffer[totalSamples], tempBuffer, samplesToCopy * 2);
    totalSamples += samplesToCopy;
  }
  
  unsigned long recordTime = millis() - startTime;
  Serial.printf("Recorded %d samples in %lu ms\n", totalSamples, recordTime);
  
  int16_t minVal = 32767, maxVal = -32768;
  long avgVal = 0;
  for (int i = 0; i < wakewordSamples; i++) {
    if (audioBuffer[i] < minVal) minVal = audioBuffer[i];
    if (audioBuffer[i] > maxVal) maxVal = audioBuffer[i];
    avgVal += audioBuffer[i];
  }
  avgVal /= wakewordSamples;
  
  Serial.printf("Audio range: %d to %d, average: %ld\n", minVal, maxVal, avgVal);
  
  if (maxVal - minVal < 100) {
    Serial.println("WARNING: Very low audio amplitude - check microphone");
    buzzerBeep(50); // Short warning beep
  }
  
  signal_t signal;
  signal.total_length = wakewordSamples;
  signal.get_data = &get_signal_data;
  
  Serial.println("Running classifier...");
  startTime = millis();
  
  ei_impulse_result_t result = {0};
  EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
  
  unsigned long classifyTime = millis() - startTime;
  Serial.printf("Classification took %lu ms\n", classifyTime);
  
  if (res != EI_IMPULSE_OK) {
    Serial.printf("Classification failed with error: %d\n", res);
    switch (res) {
      case EI_IMPULSE_INPUT_TENSOR_WAS_NULL:
        Serial.println("Error: Input tensor was null");
        break;
      case EI_IMPULSE_DSP_ERROR:
        Serial.println("Error: DSP processing error");
        break;
      default:
        Serial.println("Error: Unknown classification error");
        break;
    }
    buzzerError();
    return;
  }
  
  Serial.println("=== Classification Results ===");
  bool wakewordFound = false;
  float maxConfidence = 0;
  String detectedLabel = "";
  
  for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    float confidence = result.classification[ix].value;
    String label = String(result.classification[ix].label);
    
    Serial.printf("  %-15s: %.4f", label.c_str(), confidence);
    
    if (confidence > WAKEWORD_THRESHOLD && 
        label != "noise" && 
        label != "unknown" && 
        label != "_unknown" &&
        label != "background") {
      
      if (confidence > maxConfidence) {
        wakewordFound = true;
        maxConfidence = confidence;
        detectedLabel = label;
        Serial.print(" ← DETECTED!");
      }
    }
    Serial.println();
  }
  
  if (wakewordFound) {
    Serial.println("=== WAKE WORD DETECTED! ===");
    Serial.printf("Label: %s\n", detectedLabel.c_str());
    Serial.printf("Confidence: %.4f (threshold: %.2f)\n", maxConfidence, WAKEWORD_THRESHOLD);
    
    wakewordDetected = true;
    lastDetectionTime = millis();
    
    publishWakeWordSignal();
    
    buzzerPattern();
    
  } else {
    Serial.println("Wake word not detected above threshold");
    buzzerOff();
  }
  
  Serial.printf("Free heap after detection: %d bytes\n", esp_get_free_heap_size());
  Serial.println("=== Detection Complete ===\n");
}

void handleRoot() {
  String status = wakewordDetected ? "DETECTED" : "NOT DETECTED";
  String buzzerStatus = "READY";
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='3'>";
  html += "<style>";
  html += "body{font-family:Arial;margin:40px;background:#f0f0f0;}";
  html += "button{padding:12px 24px;margin:8px;font-size:16px;border:none;border-radius:4px;cursor:pointer;}";
  html += ".primary{background:#4CAF50;color:white;}";
  html += ".secondary{background:#2196F3;color:white;}";
  html += ".danger{background:#f44336;color:white;}";
  html += ".status{padding:20px;margin:10px 0;border-radius:8px;font-weight:bold;text-align:center;}";
  html += ".detected{background:#4CAF50;color:white;animation:pulse 2s infinite;}";
  html += ".not-detected{background:#757575;color:white;}";
  html += "@keyframes pulse{0%{opacity:1;}50%{opacity:0.7;}100%{opacity:1;}}";
  html += "</style></head><body>";
  
  html += "<h1>ESP32 Wake Word Detector (BUZZER VERSION)</h1>";
  
  html += "<div class='status " + String(wakewordDetected ? "detected" : "not-detected") + "'>";
  html += "Wake Word: " + status;
  if (wakewordDetected) {
    html += "<br>Detected " + String((millis() - lastDetectionTime) / 1000) + " seconds ago";
  }
  html += "</div>";
  
  html += "<div class='status'>Buzzer: " + buzzerStatus + "</div>";
  html += "<div class='status'>Duration: " + String(WAKEWORD_TIME, 3) + "s | Samples: " + String(wakewordSamples) + "</div>";
  
  html += "<h3>Wake Word Detection</h3>";
  html += "<p><strong>Press the physical button</strong> to trigger wake word detection</p>";
  html += "<button class='secondary' onclick=\"location.href='/test_wakeword'\">Test Detection (Web)</button>";
  html += "<button class='secondary' onclick=\"location.href='/wakeword_status'\">Model Info</button><br>";

  html += "<h3>Audio Recording</h3>";
  html += "<button class='primary' onclick=\"location.href='/record'\">Record 1 second</button>";
  html += "<button class='primary' onclick=\"location.href='/download'\">Download Recording</button><br>";
  
  html += "<h3>System</h3>";
  html += "<button class='secondary' onclick=\"location.href='/status'\">Storage Status</button>";
  html += "<button class='danger' onclick=\"if(confirm('Format filesystem?')) location.href='/format'\">Format Storage</button>";
  
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleTestWakeword() {
  performWakewordDetection();
  server.send(200, "text/html", 
    "<!DOCTYPE html><html><body><h1>Wake Word Test Complete</h1>"
    "<p>Check the serial monitor for results and listen for buzzer patterns.</p>"
    "<p><a href='/'>Go back</a></p></body></html>");
}

void handleWakewordStatus() {
  String html = "<!DOCTYPE html><html><body style='font-family:Arial;margin:40px;'>";
  html += "<h1>Wake Word Model Status (BUZZER VERSION)</h1>";
  
  html += "<h3>Model Configuration</h3>";
  html += "<p><strong>Model Frequency:</strong> " + String(EI_CLASSIFIER_FREQUENCY) + " Hz</p>";
  html += "<p><strong>Model Expected Samples:</strong> " + String(EI_CLASSIFIER_RAW_SAMPLE_COUNT) + "</p>";
  html += "<p><strong>Our Sample Rate:</strong> " + String(SAMPLE_RATE) + " Hz</p>";
  html += "<p><strong>Our Buffer Size:</strong> " + String(wakewordSamples) + " samples</p>";
  html += "<p><strong>Recording Duration:</strong> " + String(WAKEWORD_TIME, 3) + " seconds</p>";
  html += "<p><strong>Detection Threshold:</strong> " + String(WAKEWORD_THRESHOLD) + "</p>";
  html += "<p><strong>Buzzer Frequency:</strong> " + String(BUZZER_FREQUENCY) + " Hz</p>";
  
  html += "<h3>Current Status</h3>";
  html += "<p><strong>Wake Word Detected:</strong> " + String(wakewordDetected ? "YES" : "NO") + "</p>";
  html += "<p><strong>Buzzer Status:</strong> READY</p>";

  html += "<h3>Model Labels</h3><ul>";
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    html += "<li><strong>" + String(ei_classifier_inferencing_categories[i]) + "</strong></li>";
  }
  html += "</ul>";

  html += "<h3>Memory Usage</h3>";
  html += "<p><strong>Free Heap:</strong> " + String(esp_get_free_heap_size()) + " bytes</p>";
  html += "<p><strong>Audio Buffer:</strong> " + String(wakewordSamples * 2) + " bytes</p>";
  
  html += "<br><p><a href='/'>Go back</a></p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

bool writeWavHeader(File &file, int dataSize, int sampleRate) {
  if (file.write((uint8_t*)"RIFF", 4) != 4) return false;
  
  uint32_t fileSize = dataSize + 36;
  if (file.write((uint8_t*)&fileSize, 4) != 4) return false;
  
  if (file.write((uint8_t*)"WAVE", 4) != 4) return false;
  
  if (file.write((uint8_t*)"fmt ", 4) != 4) return false;
  
  uint32_t fmtSize = 16;
  if (file.write((uint8_t*)&fmtSize, 4) != 4) return false;
  
  uint16_t audioFormat = 1;
  if (file.write((uint8_t*)&audioFormat, 2) != 2) return false;
  
  uint16_t numChannels = 1;
  if (file.write((uint8_t*)&numChannels, 2) != 2) return false;
  
  uint32_t sampleRateVal = sampleRate;
  if (file.write((uint8_t*)&sampleRateVal, 4) != 4) return false;
  
  uint32_t byteRate = sampleRate * 2;
  if (file.write((uint8_t*)&byteRate, 4) != 4) return false;
  
  uint16_t blockAlign = 2;
  if (file.write((uint8_t*)&blockAlign, 2) != 2) return false;
  
  uint16_t bitsPerSample = 16;
  if (file.write((uint8_t*)&bitsPerSample, 2) != 2) return false;
  
  if (file.write((uint8_t*)"data", 4) != 4) return false;
  
  uint32_t dataSizeVal = dataSize;
  if (file.write((uint8_t*)&dataSizeVal, 4) != 4) return false;
  
  return true;
}

void handleRecord() {
  Serial.println("Starting 1-second recording...");
  
  const int RECORD_SAMPLE_RATE = 16000;
  
  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  
  const int headerSize = 44;
  const int bytesPerSample = 2;
  const int totalSamples = RECORD_SAMPLE_RATE * RECORD_TIME;
  const int audioDataSize = totalSamples * bytesPerSample;
  const int totalFileSize = headerSize + audioDataSize;
  
  size_t availableBytes = totalBytes - usedBytes;
  if (totalFileSize > availableBytes) {
    if (LittleFS.exists(FILENAME)) {
      LittleFS.remove(FILENAME);
      Serial.println("Deleted old recording to make space");
      availableBytes = totalBytes - LittleFS.usedBytes();
      
      if (totalFileSize > availableBytes) {
        server.send(507, "text/plain", "Insufficient storage space even after cleanup");
        return;
      }
    } else {
      server.send(507, "text/plain", "Insufficient storage space");
      return;
    }
  }
  
  File file = LittleFS.open(FILENAME, "w");
  if (!file) {
    Serial.println("Failed to open file for writing");
    server.send(500, "text/plain", "Failed to create recording file");
    return;
  }
  
  if (!writeWavHeader(file, audioDataSize, RECORD_SAMPLE_RATE)) {
    file.close();
    LittleFS.remove(FILENAME);
    server.send(500, "text/plain", "Failed to write WAV header");
    return;
  }
  
  bool needsReconfigure = (RECORD_SAMPLE_RATE != SAMPLE_RATE);
  if (needsReconfigure) {
    i2s_driver_uninstall(I2S_NUM_0);
    
    i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = RECORD_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = 1024,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_SCK,
      .ws_io_num = I2S_WS,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = I2S_SD
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_zero_dma_buffer(I2S_NUM_0);
    delay(10);
  }
  
  size_t bytesRead;
  uint8_t buffer[1024];
  int totalBytesRead = 0;
  bool recordingSuccess = true;
  
  Serial.println("Recording audio...");
  buzzerBeep(50); // Signal recording start
  
  while (totalBytesRead < audioDataSize && recordingSuccess) {
    esp_err_t result = i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytesRead, portMAX_DELAY);
    if (result != ESP_OK) {
      Serial.printf("I2S read error: %d\n", result);
      recordingSuccess = false;
      break;
    }
    
    size_t bytesToWrite = min((size_t)(audioDataSize - totalBytesRead), bytesRead);
    size_t bytesWritten = file.write(buffer, bytesToWrite);
    
    if (bytesWritten != bytesToWrite) {
      Serial.printf("Write error: expected %d, wrote %d\n", bytesToWrite, bytesWritten);
      recordingSuccess = false;
      break;
    }
    
    totalBytesRead += bytesWritten;
  }
  
  file.close();
  
  if (needsReconfigure) {
    setupI2S();
  }
  
  if (!recordingSuccess) {
    LittleFS.remove(FILENAME);
    buzzerError();
    server.send(500, "text/plain", "Recording failed due to I/O error");
    return;
  }
  
  Serial.printf("Recording complete! Wrote %d bytes\n", totalBytesRead);
  printLittleFSInfo();
  
  // Signal successful recording
  buzzerBeep(200);
  delay(100);
  buzzerBeep(200);
  
  server.send(200, "text/html", 
    "<!DOCTYPE html><html><body><h1>Recording Complete!</h1>"
    "<p>Successfully recorded " + String(RECORD_TIME) + " seconds of audio</p>"
    "<p><a href='/'>Go back</a> | <a href='/download'>Download</a></p></body></html>");
}

void handleDownload() {
  if (!LittleFS.exists(FILENAME)) {
    server.send(404, "text/html", 
      "<!DOCTYPE html><html><body><h1>No Recording Found</h1>"
      "<p>Please record audio first.</p><p><a href='/'>Go back</a></p></body></html>");
    return;
  }
  
  File file = LittleFS.open(FILENAME, "r");
  if (!file) {
    server.send(500, "text/plain", "Failed to open recording file");
    return;
  }
  
  server.sendHeader("Content-Disposition", "attachment; filename=" + String(FILENAME));
  server.streamFile(file, "audio/wav");
  file.close();
}

void handleStatus() {
  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  float usage = (float)usedBytes / totalBytes * 100.0;
  
  String html = "<!DOCTYPE html><html><body style='font-family:Arial;margin:40px;'>";
  html += "<h1>Storage Status</h1>";
  html += "<p><strong>Total Space:</strong> " + String(totalBytes) + " bytes (" + String(totalBytes/1024) + " KB)</p>";
  html += "<p><strong>Used Space:</strong> " + String(usedBytes) + " bytes (" + String(usedBytes/1024) + " KB)</p>";
  html += "<p><strong>Free Space:</strong> " + String(totalBytes - usedBytes) + " bytes (" + String((totalBytes - usedBytes)/1024) + " KB)</p>";
  html += "<p><strong>Usage:</strong> " + String(usage, 1) + "%</p>";
  html += "<p><strong>Recording exists:</strong> " + String(LittleFS.exists(FILENAME) ? "Yes" : "No") + "</p>";
  
  if (LittleFS.exists(FILENAME)) {
    File file = LittleFS.open(FILENAME, "r");
    if (file) {
      html += "<p><strong>Recording size:</strong> " + String(file.size()) + " bytes</p>";
      file.close();
    }
  }

  html += "<h3>Memory Status</h3>";
  html += "<p><strong>Free Heap:</strong> " + String(esp_get_free_heap_size()) + " bytes</p>";
  html += "<p><strong>Min Free Heap:</strong> " + String(esp_get_minimum_free_heap_size()) + " bytes</p>";
  
  html += "<br><p><a href='/'>Go back</a></p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleFormat() {
  Serial.println("Formatting LittleFS...");
  
  if (LittleFS.format()) {
    Serial.println("LittleFS formatted successfully");
    buzzerBeep(100);
    delay(50);
    buzzerBeep(100);
    server.send(200, "text/html", 
      "<!DOCTYPE html><html><body><h1>Storage Formatted</h1>"
      "<p>Filesystem has been formatted successfully.</p>"
      "<p><a href='/'>Go back</a></p></body></html>");
  } else {
    Serial.println("LittleFS format failed");
    buzzerError();
    server.send(500, "text/plain", "Failed to format LittleFS");
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setupBuzzer();
  
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
  
  Serial.println("=== Edge Impulse Model Validation ===");
  Serial.printf("Model expected frequency: %d Hz\n", EI_CLASSIFIER_FREQUENCY);
  Serial.printf("Model expected samples: %d\n", EI_CLASSIFIER_RAW_SAMPLE_COUNT);
  Serial.printf("Model expected duration: %.3f seconds\n", WAKEWORD_TIME);
  Serial.printf("Model label count: %d\n", EI_CLASSIFIER_LABEL_COUNT);
  
  Serial.println("Model labels:");
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    Serial.printf("  [%d]: '%s'\n", i, ei_classifier_inferencing_categories[i]);
  }
  
  if (EI_CLASSIFIER_FREQUENCY <= 0 || EI_CLASSIFIER_RAW_SAMPLE_COUNT <= 0) {
    Serial.println("FATAL ERROR: Invalid model parameters!");
    while(1) { 
      buzzerBeep(200);
      delay(200);
    }
  }
  
  if (EI_CLASSIFIER_FREQUENCY > 48000) {
    Serial.println("FATAL ERROR: Model sample rate too high for ESP32 I2S!");
    while(1) {
      buzzerBeep(100);
      delay(100);
    }
  }
  
  audioBuffer = (int16_t*)malloc(wakewordSamples * sizeof(int16_t));
  if (!audioBuffer) {
    Serial.println("CRITICAL: Failed to allocate audio buffer!");
    while(1) delay(1000);
  }
  Serial.printf("Audio buffer allocated: %d samples (%d bytes)\n", 
                wakewordSamples, wakewordSamples * sizeof(int16_t));
  
  Serial.println("Mounting LittleFS...");
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed");
    return;
  }
  
  printLittleFSInfo();
  
  setupI2S();
  
  Serial.printf("Free heap: %d bytes\n", esp_get_free_heap_size());
  
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }

  client.setServer(mqtt_server, mqtt_port);
  Serial.println("MQTT client configured");

  if (WiFi.status() == WL_CONNECTED) {
    connectToMQTT();
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed - continuing without web interface");
  }
  
  server.on("/", handleRoot);
  server.on("/record", handleRecord);
  server.on("/download", handleDownload);
  server.on("/status", handleStatus);
  server.on("/format", handleFormat);
  server.on("/wakeword_status", handleWakewordStatus);
  server.on("/test_wakeword", handleTestWakeword);
  server.begin();
  
  Serial.println("=== Setup Complete ===");
  Serial.printf("Wake word detection duration: %.3f seconds\n", WAKEWORD_TIME);
  Serial.println("Press button to start wake word detection");
  
  // Test buzzer on startup - 2 quick beeps to confirm operation
  buzzerBeep(100);
  delay(100);
  buzzerBeep(100);
}

void loop() {
  server.handleClient();
  
  if (!client.connected() && mqttConnected) {
    mqttConnected = false;
    Serial.println("MQTT disconnected");
  }
  
  if (!client.connected() && (millis() - lastMqttAttempt > MQTT_RETRY_INTERVAL)) {
    connectToMQTT();
    lastMqttAttempt = millis();
  } else if (client.connected()) {
    client.loop();
  }
  
  if (buttonPressed) {
    buttonPressed = false;
    Serial.println("Button pressed - starting wake word detection");
    performWakewordDetection();
  }
  
  // Auto-turn off detection state after 5 seconds
  if (wakewordDetected && (millis() - lastDetectionTime > 5000)) {
    wakewordDetected = false;
    buzzerOff();
    Serial.println("Wake word detection expired");
  }
}
