#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <rom/rtc.h>
#include <HTTPClient.h>
#include <math.h>

// ── Server ──────────────────────────────────────────────────────────
const char* WS_HOST = "looi-esp-aiv2-1.onrender.com";
const int   WS_PORT = 443;
const char* WS_PATH = "/ws/esp32";
const char* FW_BUILD_TAG = "stereo-jitter-fix-v1";

// ── Hardware pins ── SAME AS YOUR WORKING CODE ─────────────────────
#define MOTOR_A1       10
#define MOTOR_A2       11
#define MOTOR_B1       12
#define MOTOR_B2       13
#define SERVO_PIN      14
#define NEO_PIN        48
#define NEO_COUNT      1
#define DAC_I2S_PORT   I2S_NUM_0
// YOUR WORKING PINS — NOT CHANGED!
#define DAC_BCLK_PIN   6
#define DAC_WS_PIN     4
#define DAC_DATA_PIN   5
#define MIC_I2S_PORT   I2S_NUM_1
#define MIC_BCLK_PIN   16
#define MIC_WS_PIN     17
#define MIC_SD_PIN     18

// LEDC PWM para sa motors
#define CH_A1  0
#define CH_A2  1
#define CH_B1  2
#define CH_B2  3
#define PWM_FREQ  5000
#define PWM_RES   8

Adafruit_NeoPixel pixels(NEO_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);
WebSocketsClient webSocket;
Preferences prefs;

bool isPlaying      = false;
bool isWSConnected  = false;
bool isGeminiReady  = false;
float currentVolume = 0.32f;
volatile float audioLevel = 0.0f;

#define MIC_RATE     16000
#define AUDIO_RATE   24000
#define MIC_CHUNK_SAMPLES 512
#define MAX_CHUNK_SIZE 8192

// JITTER FIX: Mas maraming blocks para sa mobile data buffer
#define AUDIO_QUEUE_BLOCK_BYTES 2048
#define AUDIO_QUEUE_BLOCK_COUNT 80
uint8_t tempBuffer[MAX_CHUNK_SIZE];
uint8_t b64DecodeBuf[MAX_CHUNK_SIZE];

unsigned long micFramesSent = 0;

unsigned long moveStopAt = 0;
bool motorsActive = false;
const unsigned long MOVE_PULSE_MS = 700;

// ── Network resilience ─────────────────────────────────────────────
int consecutiveFailures = 0;
const int MAX_FAILURES_BEFORE_RESTART = 30;

// Manual keepalive (para sa Globe idle timeout)
unsigned long lastKeepalive = 0;
const unsigned long KEEPALIVE_INTERVAL = 15000;
unsigned long lastWiFiRecoveryAttempt = 0;
const unsigned long WIFI_RECOVERY_INTERVAL = 10000;

const IPAddress GOOGLE_DNS(8, 8, 8, 8);
const IPAddress CLOUDFLARE_DNS(1, 1, 1, 1);

// Debug mode: i-set to true para i-disable ang audio sending
const bool AUDIO_TEST_MODE = false;
// PCM5102 diagnostic — set to true to test 440Hz tone
const bool PCM5102_TONE_TEST = false;
bool dacReady = false;

// Network callbacks must return quickly.
struct AudioQueueBlock {
  size_t length;
  uint8_t data[AUDIO_QUEUE_BLOCK_BYTES];
};

QueueHandle_t audioQueue = nullptr;
TaskHandle_t audioPlaybackTaskHandle = nullptr;
volatile bool audioPlaybackInProgress = false;
volatile bool audioTurnCompletePending = false;

// STEREO FIX: Pre-allocated stereo buffer
static int16_t stereoBuffer[(MAX_CHUNK_SIZE / sizeof(int16_t)) * 2];

// --------------------
// Debug helpers
// --------------------

void setColor(uint32_t color) {
  pixels.setPixelColor(0, color);
  pixels.show();
}

void printBootReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("[BOOT] Reset reason: ");
  switch (reason) {
    case ESP_RST_POWERON: Serial.println("Power-on"); break;
    case ESP_RST_SW: Serial.println("Software restart"); break;
    case ESP_RST_PANIC: Serial.println("Exception/panic"); break;
    case ESP_RST_BROWNOUT: Serial.println("Brownout (mahinang power!)"); break;
    case ESP_RST_WDT: Serial.println("Watchdog timeout"); break;
    default: Serial.println((int)reason); break;
  }
}

// --------------------
// Motors (LEDC PWM)
// --------------------

void setupMotors() {
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcAttach(MOTOR_A1, PWM_FREQ, PWM_RES);
    ledcAttach(MOTOR_A2, PWM_FREQ, PWM_RES);
    ledcAttach(MOTOR_B1, PWM_FREQ, PWM_RES);
    ledcAttach(MOTOR_B2, PWM_FREQ, PWM_RES);
  #else
    ledcSetup(CH_A1, PWM_FREQ, PWM_RES); ledcAttachPin(MOTOR_A1, CH_A1);
    ledcSetup(CH_A2, PWM_FREQ, PWM_RES); ledcAttachPin(MOTOR_A2, CH_A2);
    ledcSetup(CH_B1, PWM_FREQ, PWM_RES); ledcAttachPin(MOTOR_B1, CH_B1);
    ledcSetup(CH_B2, PWM_FREQ, PWM_RES); ledcAttachPin(MOTOR_B2, CH_B2);
  #endif
}

void motorWrite(int pin, int duty) {
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcWrite(pin, duty);
  #else
    int ch = (pin == MOTOR_A1) ? CH_A1 : (pin == MOTOR_A2) ? CH_A2 :
             (pin == MOTOR_B1) ? CH_B1 : CH_B2;
    ledcWrite(ch, duty);
  #endif
}

void stopMotors() {
  motorWrite(MOTOR_A1, 0); motorWrite(MOTOR_A2, 0);
  motorWrite(MOTOR_B1, 0); motorWrite(MOTOR_B2, 0);
  motorsActive = false;
}

void driveMotors(const String& move, uint8_t speed) {
  if (speed == 0) speed = 128;
  stopMotors();
  delay(5);
  if (move == "FORWARD") {
    motorWrite(MOTOR_A1, speed); motorWrite(MOTOR_A2, 0);
    motorWrite(MOTOR_B1, speed); motorWrite(MOTOR_B2, 0);
  } else if (move == "BACKWARD") {
    motorWrite(MOTOR_A1, 0); motorWrite(MOTOR_A2, speed);
    motorWrite(MOTOR_B1, 0); motorWrite(MOTOR_B2, speed);
  } else if (move == "LEFT") {
    motorWrite(MOTOR_A1, 0); motorWrite(MOTOR_A2, speed);
    motorWrite(MOTOR_B1, speed); motorWrite(MOTOR_B2, 0);
  } else if (move == "RIGHT") {
    motorWrite(MOTOR_A1, speed); motorWrite(MOTOR_A2, 0);
    motorWrite(MOTOR_B1, 0); motorWrite(MOTOR_B2, speed);
  } else {
    stopMotors(); return;
  }
  motorsActive = true;
  moveStopAt = millis() + MOVE_PULSE_MS;
}

void applyLed(const String& led) {
  if (led == "LED_RED")        setColor(pixels.Color(255, 0, 0));
  else if (led == "LED_GREEN") setColor(pixels.Color(0, 255, 0));
  else if (led == "LED_BLUE")  setColor(pixels.Color(0, 0, 255));
  else if (led == "LED_WHITE") setColor(pixels.Color(255, 255, 255));
  else if (led == "LED_CYAN")  setColor(pixels.Color(0, 255, 255));
  else if (led == "LED_PURPLE")setColor(pixels.Color(150, 0, 255));
  else if (led == "LED_ORANGE")setColor(pixels.Color(255, 100, 0));
  else if (led == "LED_YELLOW")setColor(pixels.Color(255, 200, 0));
  else if (led == "LED_PINK")  setColor(pixels.Color(255, 0, 150));
  else if (led == "LED_ON")    setColor(pixels.Color(255, 255, 255));
  else if (led == "LED_OFF")   setColor(pixels.Color(0, 0, 0));
}

void handleRobotAction(const String& move, const String& led, int speed) {
  if (led.length()) applyLed(led);
  if (move.length() && move != "NONE") driveMotors(move, (uint8_t)speed);
}

// --------------------
// Base64 decode
// --------------------

int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

size_t base64Decode(const char* in, size_t len, uint8_t* out, size_t outCap) {
  size_t o = 0;
  int val = 0, bits = -8;
  for (size_t i = 0; i < len; i++) {
    char c = in[i];
    if (c == '=' || c == '\0') break;
    int d = b64Val(c);
    if (d < 0) continue;
    val = (val << 6) + d;
    bits += 6;
    if (bits >= 0) {
      if (o >= outCap) break;
      out[o++] = (uint8_t)((val >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return o;
}

// --------------------
// Tiny JSON helpers
// --------------------

String jsonGetString(const String& src, const char* key, int fromIndex = 0) {
  String needle = String("\"") + key + "\":\"";
  int i = src.indexOf(needle, fromIndex);
  if (i < 0) return "";
  int start = i + needle.length();
  int end = src.indexOf('"', start);
  while (end > 0 && src.charAt(end - 1) == '\\') end = src.indexOf('"', end + 1);
  if (end < 0) return "";
  return src.substring(start, end);
}

int jsonGetInt(const String& src, const char* key, int def = 0) {
  String needle = String("\"") + key + "\":";
  int i = src.indexOf(needle);
  if (i < 0) return def;
  int start = i + needle.length();
  int end = start;
  while (end < (int)src.length() && (isDigit(src.charAt(end)) || src.charAt(end) == '-')) end++;
  if (end == start) return def;
  return src.substring(start, end).toInt();
}

bool jsonHas(const String& src, const char* literal) {
  return src.indexOf(literal) >= 0;
}

// --------------------
// Audio
// --------------------

void streamMicChunk(const uint8_t* buf, size_t bytes) {
  webSocket.sendBIN((uint8_t*)buf, bytes);
}

void applyVolume(uint8_t* data, size_t len, float vol) {
  int16_t* samples = (int16_t*)data;
  for (size_t i = 0; i < len / 2; i++) {
    int32_t scaled = (int32_t)((float)samples[i] * vol);
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    samples[i] = (int16_t)scaled;
  }
}

float computeAudioLevel(uint8_t* data, size_t len) {
  const int count = len / sizeof(int16_t);
  float sum = 0;
  for (int i = 0; i < count; i++) {
    int16_t sample = 0;
    memcpy(&sample, data + (i * sizeof(int16_t)), sizeof(sample));
    float s = sample;
    sum += s * s;
  }
  return count ? sqrt(sum / count) : 0;
}

void setupPcm5102() {
  i2s_config_t dac_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = AUDIO_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };
  i2s_pin_config_t dac_pins = {
    .bck_io_num = DAC_BCLK_PIN,
    .ws_io_num = DAC_WS_PIN,
    .data_out_num = DAC_DATA_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  esp_err_t err = i2s_driver_install(DAC_I2S_PORT, &dac_cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] PCM5102 I2S install failed: %d\n", err);
    return;
  }
  err = i2s_set_pin(DAC_I2S_PORT, &dac_pins);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] PCM5102 I2S pins failed: %d\n", err);
    i2s_driver_uninstall(DAC_I2S_PORT);
    return;
  }
  i2s_zero_dma_buffer(DAC_I2S_PORT);
  dacReady = true;
  Serial.printf("[INIT] PCM5102 DAC I2S OK (BCK GPIO %d, WS GPIO %d, DIN GPIO %d, %dkHz)\n",
                DAC_BCLK_PIN, DAC_WS_PIN, DAC_DATA_PIN, AUDIO_RATE / 1000);
}

// STEREO FIX: Pre-allocated buffer, duplicate mono to both channels
void writeStereoPcm(const int16_t* samples, size_t sampleCount) {
  if (!dacReady || sampleCount == 0) return;

  size_t maxSamples = sizeof(stereoBuffer) / (2 * sizeof(int16_t));
  size_t samplesToProcess = min(sampleCount, maxSamples);

  for (size_t i = 0; i < samplesToProcess; i++) {
    stereoBuffer[i * 2] = samples[i];      // LEFT channel
    stereoBuffer[i * 2 + 1] = samples[i];  // RIGHT channel (same as left)
  }

  size_t bytesWritten = 0;
  i2s_write(DAC_I2S_PORT, stereoBuffer, samplesToProcess * 2 * sizeof(int16_t),
            &bytesWritten, portMAX_DELAY);
}

void playPcm5102ToneTest() {
  Serial.println("[AUDIO TEST] PCM5102 I2S: 440Hz tone for 1 second");
  static int16_t toneBuffer[256 * 2];
  const size_t toneSamples = sizeof(toneBuffer) / (2 * sizeof(int16_t));

  for (size_t offset = 0; offset < AUDIO_RATE; offset += toneSamples) {
    const size_t count = min(toneSamples, (size_t)AUDIO_RATE - offset);
    for (size_t i = 0; i < count; i++) {
      const float phase = 2.0f * 3.14159265359f * 440.0f *
                          (float)(offset + i) / AUDIO_RATE;
      const int16_t sample = (int16_t)(sinf(phase) * 20000.0f);
      toneBuffer[i * 2] = sample;
      toneBuffer[i * 2 + 1] = sample;
    }
    size_t bytesWritten = 0;
    i2s_write(DAC_I2S_PORT, toneBuffer, count * 2 * sizeof(int16_t),
              &bytesWritten, portMAX_DELAY);
  }
  Serial.println("[AUDIO TEST] PCM5102 tone ended");
}

void writePcmToPcm5102(const uint8_t* data, size_t len) {
  // Gemini sends little-endian signed 16-bit mono PCM at 24 kHz.
  // Convert to stereo: duplicate each sample to L/R
  writeStereoPcm(reinterpret_cast<const int16_t*>(data), len / sizeof(int16_t));
}

void clearAudioQueue() {
  audioTurnCompletePending = false;
  audioPlaybackInProgress = false;
  if (audioQueue) xQueueReset(audioQueue);
  isPlaying = false;
}

void finishAudioTurnIfDrained() {
  if (!audioTurnCompletePending || audioPlaybackInProgress || !audioQueue) return;
  if (uxQueueMessagesWaiting(audioQueue) != 0) return;
  audioTurnCompletePending = false;
  isPlaying = false;
  Serial.println("[AUDIO] PLAYBACK_END (buffer drained)");
  setColor(pixels.Color(0, 0, 100));
}

void audioPlaybackTask(void*) {
  AudioQueueBlock block;
  for (;;) {
    if (xQueueReceive(audioQueue, &block, portMAX_DELAY) != pdTRUE) continue;
    audioPlaybackInProgress = true;
    isPlaying = true;
    setColor(pixels.Color(200, 0, 200));
    if (block.length > 0) {
      audioLevel = computeAudioLevel(block.data, block.length);
      writePcmToPcm5102(block.data, block.length);
    }
    audioPlaybackInProgress = false;
    finishAudioTurnIfDrained();
  }
}

bool queueAudioForPlayback(const uint8_t* data, size_t len) {
  if (!audioQueue || !data || len == 0) return false;
  size_t offset = 0;
  while (offset < len) {
    AudioQueueBlock block;
    block.length = min(len - offset, (size_t)AUDIO_QUEUE_BLOCK_BYTES);
    memcpy(block.data, data + offset, block.length);
    if (xQueueSend(audioQueue, &block, 0) != pdTRUE) {
      Serial.println("[AUDIO] Playback queue full — dropping newest packet");
      return false;
    }
    offset += block.length;
  }
  isPlaying = true;
  return true;
}

// --------------------
// WebSocket
// --------------------

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      isWSConnected = false;
      isGeminiReady = false;
      clearAudioQueue();
      consecutiveFailures++;
      Serial.printf("[MIC] Continuous stream stopped after %lu frame(s)\n", micFramesSent);
      Serial.printf("[WS] Disconnected (failure #%d, library retry interval active)\n",
                    consecutiveFailures);
      setColor(pixels.Color(100, 0, 0));

      if (consecutiveFailures >= MAX_FAILURES_BEFORE_RESTART) {
        Serial.println("[NET] Too many failures, restarting ESP32...");
        delay(1000);
        ESP.restart();
      }
      break;

    case WStype_CONNECTED: {
      isWSConnected = true;
      isGeminiReady = false;
      consecutiveFailures = 0;
      lastKeepalive = millis();
      Serial.println("[WS] Connected ✓");
      setColor(pixels.Color(0, 0, 100));

      webSocket.sendTXT("{\"deviceHello\":{\"device\":\"alexatron-esp32s3\"}}");
      break;
    }

    case WStype_TEXT: {
      String msg((char*)payload, length);
      Serial.println("[WS] TXT: " + msg.substring(0, min((int)msg.length(), 150)));

      if (jsonHas(msg, "\"status\":\"ready\"")) {
        isGeminiReady = true;
        Serial.println("[WS] Gemini ready — continuous mic streaming enabled");
      }

      if (jsonHas(msg, "\"serverHello\"") || jsonHas(msg, "\"pong\"")) {
        Serial.println("[WS] Server hello/keepalive ack");
        break;
      }

      if (jsonHas(msg, "\"robotAction\"")) {
        String move = jsonGetString(msg, "move");
        String led  = jsonGetString(msg, "led");
        int speed    = jsonGetInt(msg, "speed", 128);
        handleRobotAction(move, led, speed);
        break;
      }
      if (jsonHas(msg, "\"error\"")) {
        Serial.println("[Server error] " + jsonGetString(msg, "error"));
        setColor(pixels.Color(100, 0, 0));
        break;
      }
      if (jsonHas(msg, "\"interrupted\":true")) {
        clearAudioQueue();
        Serial.println("[AUDIO] PLAYBACK_INTERRUPTED");
        setColor(pixels.Color(0, 0, 100));
        break;
      }
      if (jsonHas(msg, "\"inlineData\"")) {
        isPlaying = true;
        setColor(pixels.Color(200, 0, 200));
        String b64 = jsonGetString(msg, "data");
        if (b64.length()) {
          size_t decoded = base64Decode(b64.c_str(), b64.length(), b64DecodeBuf, MAX_CHUNK_SIZE);
          if (decoded > 0) {
            uint8_t* p = b64DecodeBuf;
            if (currentVolume != 1.0f && decoded <= MAX_CHUNK_SIZE) {
              memcpy(tempBuffer, b64DecodeBuf, decoded);
              applyVolume(tempBuffer, decoded, currentVolume);
              p = tempBuffer;
            }
            queueAudioForPlayback(p, decoded);
          }
        }
      }
      if (jsonHas(msg, "\"turnComplete\":true")) {
        audioTurnCompletePending = true;
        finishAudioTurnIfDrained();
        Serial.println("[AUDIO] Gemini turn complete — waiting for playback queue");
      }
      break;
    }

    case WStype_BIN: {
      static unsigned long audioFramesReceived = 0;
      if (length == 0) break;

      audioFramesReceived++;
      if (audioFramesReceived <= 3 || audioFramesReceived % 10 == 0) {
        Serial.printf("[WS] AI audio frame #%lu (%u bytes, RMS=%.0f)\n",
                      audioFramesReceived, (unsigned)length,
                      computeAudioLevel(payload, length));
      }

      queueAudioForPlayback(payload, length);
      break;
    }

    case WStype_ERROR:
      Serial.printf("[WS] Error event: %s\n", payload ? (char*)payload : "unknown");
      break;
  }
}

// --------------------
// Network Diagnostics
// --------------------

bool checkInternetConnectivity() {
  Serial.println("[NET] Testing HTTP connectivity...");
  HTTPClient http;
  http.setTimeout(5000);
  http.begin(String("https://") + WS_HOST + "/health");
  int httpCode = http.GET();
  http.end();

  if (httpCode == 200) {
    Serial.println("[NET] HTTP test ✓ (Server reachable)");
    return true;
  } else {
    Serial.printf("[NET] HTTP test ✗ (code: %d)\n", httpCode);
    return false;
  }
}

void maintainWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWiFiRecoveryAttempt < WIFI_RECOVERY_INTERVAL) return;

  lastWiFiRecoveryAttempt = millis();
  isWSConnected = false;
  isGeminiReady = false;
  clearAudioQueue();
  Serial.printf("[NET] WiFi disconnected (status %d) — reconnecting...\n", WiFi.status());
  WiFi.reconnect();
}

bool resolveHost() {
  IPAddress resolvedIP;
  Serial.printf("[NET] Resolving %s...\n", WS_HOST);

  if (WiFi.hostByName(WS_HOST, resolvedIP)) {
    Serial.printf("[NET] Resolved to: %s\n", resolvedIP.toString().c_str());
    return true;
  } else {
    Serial.println("[NET] DNS resolution FAILED");
    return false;
  }
}

// --------------------
// Setup
// --------------------

void setup() {
  Serial.begin(115200);
  unsigned long serialTimeout = millis();
  while (!Serial && (millis() - serialTimeout < 3000)) { delay(10); }
  delay(500);

  Serial.println("\n\n═══════════════════════════════════════");
  Serial.println("  ALEXATRON BOOT — STEREO + JITTER FIX");
  Serial.println("═══════════════════════════════════════");
  Serial.printf("[INIT] Firmware: %s\n", FW_BUILD_TAG);
  printBootReason();

  pixels.begin();
  setColor(pixels.Color(50, 50, 0));
  Serial.println("[INIT] NeoPixel OK");

  prefs.begin("alexatron", false);
  currentVolume = prefs.getFloat("volume", 0.32f);
  Serial.printf("[INIT] Volume: %.2f\n", currentVolume);

  setupMotors();
  stopMotors();
  Serial.println("[INIT] Motors OK (LEDC)");

  Serial.println("[INIT] Skipping servo (debug mode)");

  Serial.println("[INIT] Creating WiFiManager...");
  WiFiManager wm;

  wm.setConfigPortalTimeout(180);
  Serial.println("[INIT] Starting autoConnect...");

  if (!wm.autoConnect("Alexatron")) {
    Serial.println("[INIT] WiFi failed, restarting...");
    delay(2000);
    ESP.restart();
  }
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  Serial.println("[INIT] WiFi connected: " + WiFi.localIP().toString());

  Serial.println("\n[NET] === Network Diagnostics ===");

  WiFi.setDNS(GOOGLE_DNS, CLOUDFLARE_DNS);
  Serial.println("[NET] DNS set to 8.8.8.8, 1.1.1.1");

  if (!resolveHost()) {
    Serial.println("[NET] WARNING: Cannot resolve server hostname!");
    setColor(pixels.Color(255, 50, 0));
    delay(3000);
  }

  if (!checkInternetConnectivity()) {
    Serial.println("[NET] WARNING: Server HTTPS not reachable!");
    setColor(pixels.Color(255, 50, 0));
    delay(3000);
  }

  Serial.println("[NET] =============================\n");

  Serial.println("[INIT] Installing Mic I2S...");
  i2s_config_t mic_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256
  };
  i2s_pin_config_t mic_p = {
    .bck_io_num = MIC_BCLK_PIN,
    .ws_io_num = MIC_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD_PIN
  };
  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &mic_cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] Mic I2S install failed: %d\n", err);
    setColor(pixels.Color(255, 0, 0));
    while (true) { delay(500); }
  }
  i2s_set_pin(MIC_I2S_PORT, &mic_p);
  Serial.println("[INIT] Mic I2S OK");

  setupPcm5102();

  // JITTER FIX: Mas maraming blocks para sa mobile data buffer
  audioQueue = xQueueCreate(AUDIO_QUEUE_BLOCK_COUNT, sizeof(AudioQueueBlock));
  if (!audioQueue) {
    Serial.println("[ERROR] Audio playback queue allocation failed");
    setColor(pixels.Color(255, 0, 0));
    while (true) { delay(500); }
  }
  if (xTaskCreatePinnedToCore(
        audioPlaybackTask, "audio-playback", 8192, nullptr, 2,
        &audioPlaybackTaskHandle, 1) != pdPASS) {
    Serial.println("[ERROR] Audio playback task creation failed");
    setColor(pixels.Color(255, 0, 0));
    while (true) { delay(500); }
  }
  Serial.printf("[INIT] Audio jitter queue ready (%d blocks / %.1fs stereo)\n",
                AUDIO_QUEUE_BLOCK_COUNT,
                (float)(AUDIO_QUEUE_BLOCK_BYTES * AUDIO_QUEUE_BLOCK_COUNT) /
                  (AUDIO_RATE * sizeof(int16_t) * 2));  // *2 for stereo

  if (PCM5102_TONE_TEST && dacReady) playPcm5102ToneTest();

  Serial.print("[INIT] Free heap before WS: ");
  Serial.println(ESP.getFreeHeap());
  if (ESP.getFreeHeap() < 80000) {
    Serial.println("[WARN] Heap is low for TLS WebSocket");
  }

  webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);

  setColor(pixels.Color(0, 0, 100));
  Serial.println("[INIT] Setup complete!");
  Serial.printf("[INIT] AUDIO_TEST_MODE = %s\n\n", AUDIO_TEST_MODE ? "ON (no audio)" : "OFF");
}

// --------------------
// Loop
// --------------------

void loop() {
  maintainWiFiConnection();
  webSocket.loop();

  if (motorsActive && millis() > moveStopAt) stopMotors();

  if (isWSConnected && (millis() - lastKeepalive >= KEEPALIVE_INTERVAL)) {
    lastKeepalive = millis();
    webSocket.sendTXT("{\"ping\":1}");
    Serial.println("[NET] Keepalive sent");
  }

  if (!isWSConnected || !isGeminiReady) return;
  if (isPlaying) return;

  if (AUDIO_TEST_MODE) {
    return;
  }

  int16_t sample_buffer[MIC_CHUNK_SAMPLES];
  size_t bytes_read = 0;
  i2s_read(MIC_I2S_PORT, sample_buffer, sizeof(sample_buffer), &bytes_read, 10);
  if (bytes_read == 0) return;

  streamMicChunk(reinterpret_cast<const uint8_t*>(sample_buffer), bytes_read);
  micFramesSent++;
  if (micFramesSent == 1 || micFramesSent % 100 == 0) {
    Serial.printf("[MIC] Continuous audio frame #%lu (%u bytes)\n",
                  micFramesSent, (unsigned)bytes_read);
  }
}
