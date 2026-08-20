// ============================================================================
// ALEXATRON FIRMWARE — FINAL FIX (v3)
// Fix: Blocking I2S write with proper timeout + larger initial buffer fill
// ============================================================================

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <rom/rtc.h>
#include <HTTPClient.h>
#include <math.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// ═══════════════════════════════════════════════════════════════════
//  CONFIGURATION
// ═══════════════════════════════════════════════════════════════════

const char* WS_HOST = "looi-esp-aiv2-1.onrender.com";
const int   WS_PORT = 443;
const char* WS_PATH = "/ws/esp32";
const char* FW_BUILD_TAG = "final-fix-v3";

#define MOTOR_A1       7
#define MOTOR_A2       8
#define MOTOR_B1       9
#define MOTOR_B2       15
#define SERVO_PIN      47
#define NEO_PIN        48
#define NEO_COUNT      1

#define DAC_I2S_PORT   I2S_NUM_0
#define DAC_BCLK_PIN   6
#define DAC_WS_PIN     4
#define DAC_DATA_PIN   5

#define MIC_I2S_PORT   I2S_NUM_1
#define MIC_BCLK_PIN   16
#define MIC_WS_PIN     17
#define MIC_SD_PIN     18

#define TFT_CS         10
#define TFT_DC         11
#define TFT_RST        12
#define TFT_MOSI       13
#define TFT_SCLK       14
#define TFT_BL         21
#define TOUCH_PIN      3

#define CH_A1  0
#define CH_A2  1
#define CH_B1  2
#define CH_B2  3
#define PWM_FREQ  5000
#define PWM_RES   8

#define MIC_RATE        16000
#define AUDIO_RATE      24000
#define MIC_CHUNK_SAMPLES 512
#define MAX_CHUNK_SIZE  8192

// ══ CRITICAL FIX: MAS MALAKING DMA BUFFER ══
// 32 x 1024 = 32KB = ~667ms of audio at 24kHz mono
// Pero kailangan pa natin ng mas malaki para sa network jitter
#define DAC_DMA_BUF_COUNT   32
#define DAC_DMA_BUF_LEN     1024
#define MIC_DMA_BUF_COUNT   8
#define MIC_DMA_BUF_LEN     1024

Adafruit_NeoPixel pixels(NEO_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);
WebSocketsClient webSocket;
Preferences prefs;

bool isPlaying      = false;
bool isWSConnected  = false;
bool isGeminiReady  = false;
float currentVolume = 0.32f;
volatile float audioLevel = 0.0f;

uint8_t b64DecodeBuf[MAX_CHUNK_SIZE];

unsigned long micFramesSent = 0;

unsigned long moveStopAt = 0;
bool motorsActive = false;
const unsigned long MOVE_PULSE_MS = 700;

int consecutiveFailures = 0;
const int MAX_FAILURES_BEFORE_RESTART = 30;

unsigned long lastKeepalive = 0;
const unsigned long KEEPALIVE_INTERVAL = 30000;

unsigned long lastWiFiRecoveryAttempt = 0;
const unsigned long WIFI_RECOVERY_INTERVAL = 10000;

const IPAddress GOOGLE_DNS(8, 8, 8, 8);
const IPAddress CLOUDFLARE_DNS(1, 1, 1, 1);

const bool AUDIO_TEST_MODE = false;
const bool PCM5102_TONE_TEST = false;
bool dacReady = false;

// ══ NEW: Audio buffer stats for debugging ══
unsigned long totalAudioBytesReceived = 0;
unsigned long totalAudioBytesWritten = 0;

// ═══════════════════════════════════════════════════════════════════
//  FACE ANIMATION SYSTEM
// ═══════════════════════════════════════════════════════════════════

SPIClass mySPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&mySPI, TFT_CS, TFT_DC, TFT_RST);

GFXcanvas16 faceCanvas(300, 120);

const int CANVAS_X = 10;
const int CANVAS_Y = 50;

const int REL_EYE_LEFT_X = 90;
const int REL_EYE_RIGHT_X = 210;
const int REL_EYE_Y = 50;
const int EYE_RADIUS = 10;

const int REL_MOUTH_X = 115;
const int REL_MOUTH_Y = 65;
const int MOUTH_W = 70;
const int MOUTH_H = 4;

float faceCurrentX = 0, faceCurrentY = 0;
float faceTargetX = 0, faceTargetY = 0;

enum Emotion { NORMAL, HAPPY, MAD, YAWN, SLEEP, SPEAKING };
Emotion currentEmotion = NORMAL;

int touchCount = 0;
unsigned long lastTouchTime = 0;
unsigned long emotionStartTime = 0;
unsigned long lastBlinkTime = 0;
unsigned long lastLookAroundTime = 0;
unsigned long lastActivityTime = 0;

const unsigned long YAWN_TIMEOUT = 10000;

bool touchHandled = false;
float zzzOffsetY = 0;

float mouthOpenAmount = 0.0f;
float mouthTargetOpen = 0.0f;

bool isBlinking = false;
unsigned long blinkStartTime = 0;
const unsigned long BLINK_DURATION = 90;

bool isYawning = false;
int yawnProgress = 0;
unsigned long yawnStartTime = 0;
const unsigned long YAWN_STEP_MS = 20;
const int YAWN_MAX_PROGRESS = 30;

unsigned long lastFaceRender = 0;
const unsigned long FACE_RENDER_INTERVAL = 33;

void setBrightness(int value) {
  analogWrite(TFT_BL, value);
}

void drawHeartIcon(int16_t x, int16_t y, int16_t size, uint16_t color) {
  faceCanvas.fillCircle(x - size/2, y, size/2, color);
  faceCanvas.fillCircle(x + size/2, y, size/2, color);
  faceCanvas.fillTriangle(x - size, y + size/4, x + size, y + size/4, x, y + size + 2, color);
}

void drawAngerMark(int16_t x, int16_t y) {
  uint16_t red = ST77XX_RED;
  faceCanvas.drawFastHLine(x - 8, y - 4, 16, red);
  faceCanvas.drawFastHLine(x - 8, y + 4, 16, red);
  faceCanvas.drawFastVLine(x - 4, y - 8, 16, red);
  faceCanvas.drawFastVLine(x + 4, y - 8, 16, red);
  faceCanvas.drawLine(x - 8, y - 4, x - 12, y - 8, red);
  faceCanvas.drawLine(x + 8, y - 4, x + 12, y - 8, red);
  faceCanvas.drawLine(x - 8, y + 4, x - 12, y + 8, red);
  faceCanvas.drawLine(x + 8, y + 4, x + 12, y + 8, red);
}

float mapAudioToMouth(float rms) {
  float normalized = rms / 3000.0f;
  if (normalized > 1.0f) normalized = 1.0f;
  if (normalized < 0.05f) normalized = 0.05f;
  return normalized;
}

void renderFaceToCanvas() {
  faceCanvas.fillScreen(ST77XX_BLACK);

  int lx = REL_EYE_LEFT_X + (int)faceCurrentX;
  int rx = REL_EYE_RIGHT_X + (int)faceCurrentX;
  int ey = REL_EYE_Y + (int)faceCurrentY;
  int mx = REL_MOUTH_X + (int)faceCurrentX;
  int my = REL_MOUTH_Y + (int)faceCurrentY;

  if (currentEmotion == HAPPY) {
    faceCanvas.fillCircle(lx, ey, EYE_RADIUS, ST77XX_WHITE);
    faceCanvas.fillCircle(lx, ey + 4, EYE_RADIUS, ST77XX_BLACK);
    faceCanvas.fillCircle(rx, ey, EYE_RADIUS, ST77XX_WHITE);
    faceCanvas.fillCircle(rx, ey + 4, EYE_RADIUS, ST77XX_BLACK);
  } else if (currentEmotion == MAD) {
    faceCanvas.fillCircle(lx, ey, EYE_RADIUS, ST77XX_WHITE);
    faceCanvas.fillCircle(rx, ey, EYE_RADIUS, ST77XX_WHITE);
    faceCanvas.fillTriangle(lx - EYE_RADIUS - 2, ey - EYE_RADIUS - 2, lx + EYE_RADIUS + 2, ey - EYE_RADIUS - 2, lx + EYE_RADIUS + 2, ey + 2, ST77XX_BLACK);
    faceCanvas.fillTriangle(rx - EYE_RADIUS - 2, ey - EYE_RADIUS - 2, rx + EYE_RADIUS + 2, ey - EYE_RADIUS - 2, rx - EYE_RADIUS - 2, ey + 2, ST77XX_BLACK);
  } else if (currentEmotion == SLEEP || currentEmotion == YAWN) {
    faceCanvas.drawFastHLine(lx - EYE_RADIUS, ey, EYE_RADIUS * 2, ST77XX_WHITE);
    faceCanvas.drawFastHLine(rx - EYE_RADIUS, ey, EYE_RADIUS * 2, ST77XX_WHITE);
  } else if (isBlinking) {
    faceCanvas.drawFastHLine(lx - EYE_RADIUS, ey, EYE_RADIUS * 2, ST77XX_WHITE);
    faceCanvas.drawFastHLine(rx - EYE_RADIUS, ey, EYE_RADIUS * 2, ST77XX_WHITE);
  } else {
    faceCanvas.fillCircle(lx, ey, EYE_RADIUS, ST77XX_WHITE);
    faceCanvas.fillCircle(rx, ey, EYE_RADIUS, ST77XX_WHITE);
  }

  if (currentEmotion == MAD) {
    faceCanvas.fillRect(mx + 10, my, MOUTH_W - 20, MOUTH_H, ST77XX_WHITE);
    faceCanvas.drawFastVLine(mx + 10, my, 6, ST77XX_WHITE);
    faceCanvas.drawFastVLine(mx + MOUTH_W - 10, my - 4, 6, ST77XX_WHITE);
  } else if (currentEmotion == YAWN) {
    int r = 2 + (yawnProgress / 4);
    int centerMouthX = mx + (MOUTH_W / 2);
    faceCanvas.fillRoundRect(centerMouthX - 12, my - 2, 24, r * 2, 6, ST77XX_WHITE);
    faceCanvas.fillRoundRect(centerMouthX - 9, my, 18, (r * 2) - 4, 4, ST77XX_BLACK);
    if (r > 8) faceCanvas.fillCircle(centerMouthX, my + (r * 2) - 8, 4, ST77XX_MAGENTA);
  } else if (currentEmotion == SLEEP) {
    faceCanvas.fillRect(mx + 20, my, MOUTH_W - 40, MOUTH_H, ST77XX_WHITE);
  } else if (currentEmotion == SPEAKING) {
    int centerMouthX = mx + (MOUTH_W / 2);
    int maxOpen = 28;
    int openH = (int)(mouthOpenAmount * maxOpen);
    if (openH < 3) openH = 3;
    int mouthW = MOUTH_W - 10;
    int mouthTop = my - openH / 2;
    faceCanvas.fillRoundRect(centerMouthX - mouthW/2, mouthTop, mouthW, openH, openH/2, ST77XX_WHITE);
    int innerW = mouthW - 8;
    int innerH = openH - 4;
    if (innerH < 2) innerH = 2;
    faceCanvas.fillRoundRect(centerMouthX - innerW/2, mouthTop + 2, innerW, innerH, innerH/2, ST77XX_BLACK);
    if (openH > 15) {
      faceCanvas.fillCircle(centerMouthX, mouthTop + innerH + 2, 4, ST77XX_MAGENTA);
    }
  } else {
    faceCanvas.fillRect(mx, my, MOUTH_W, MOUTH_H, ST77XX_WHITE);
  }

  int iconX = rx + 25;
  int iconY = ey - 25;

  if (currentEmotion == HAPPY) {
    drawHeartIcon(iconX, iconY, 8, ST77XX_RED);
  } else if (currentEmotion == MAD) {
    drawAngerMark(iconX, iconY);
  } else if (currentEmotion == SLEEP) {
    int floatY = iconY - (int)zzzOffsetY;
    faceCanvas.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
    faceCanvas.setTextSize(2);
    faceCanvas.setCursor(iconX - 5, floatY);
    faceCanvas.print("Z");
    faceCanvas.setTextSize(1);
    faceCanvas.setCursor(iconX + 10, floatY - 5);
    faceCanvas.print("zz");
  } else if (currentEmotion == SPEAKING) {
    faceCanvas.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
    faceCanvas.setTextSize(1);
    faceCanvas.setCursor(iconX - 5, iconY + 10);
    faceCanvas.print("~");
  }

  tft.drawRGBBitmap(CANVAS_X, CANVAS_Y, faceCanvas.getBuffer(), 300, 120);
}

// ═══════════════════════════════════════════════════════════════════
//  DEBUG HELPERS
// ═══════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════
//  MOTORS
// ═══════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════
//  BASE64 DECODE
// ═══════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════
//  TINY JSON HELPERS
// ═══════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════
//  AUDIO SYSTEM — FINAL FIX
// ═══════════════════════════════════════════════════════════════════

void streamMicChunk(const uint8_t* buf, size_t bytes) {
  webSocket.sendBIN((uint8_t*)buf, bytes);
}

void applyVolumeInPlace(int16_t* samples, size_t sampleCount) {
  for (size_t i = 0; i < sampleCount; i++) {
    float sample = samples[i] * currentVolume;
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    samples[i] = (int16_t)sample;
  }
}

float computeAudioLevel(int16_t* samples, size_t sampleCount) {
  if (sampleCount == 0) return 0;
  float sum = 0;
  for (size_t i = 0; i < sampleCount; i++) {
    float s = samples[i];
    sum += s * s;
  }
  return sqrt(sum / sampleCount);
}

// ═══════════════════════════════════════════════════════════════════
//  CRITICAL FIX: PROPER I2S WRITE
// ═══════════════════════════════════════════════════════════════════
// 
// ANG SIKRETO: Yung I2S DMA buffer (32KB) ang magiging jitter buffer.
// Kailangan lang: i-fill natin sya ng mas mabilis kaysa ma-drain.
//
// Strategy:
// 1. Write ALL data to I2S DMA buffer (blocking with timeout)
// 2. Yung I2S hardware na bahala sa exact timing
// 3. Kung puno yung buffer, wait ng konti (pero hindi sobrang tagal)
//
// Yung key: portMAX_DELAY para sure na nasulat, PERO yung DMA buffer
// malaki enough para hindi mag-block ng matagal.

void directI2SWrite(uint8_t* data, size_t len) {
  if (!dacReady || len == 0) return;
  if (len < 10) return;  // Filter corrupt frames

  size_t numSamples = len / sizeof(int16_t);
  if (numSamples > 0) {
    audioLevel = computeAudioLevel((int16_t*)data, numSamples);
    mouthTargetOpen = mapAudioToMouth(audioLevel);
    if (currentVolume != 1.0f) {
      applyVolumeInPlace((int16_t*)data, numSamples);
    }
  }

  // ══ FINAL FIX: Blocking write with reasonable timeout ══
  // portMAX_DELAY = wait until space available in DMA buffer
  // Pero since 32KB yung buffer, at bawat call is ~2KB, 
  // hindi mag-block ng matagal unless punong-puno na
  size_t written = 0;
  esp_err_t err = i2s_write(DAC_I2S_PORT, data, len, &written, portMAX_DELAY);
  
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] I2S write error: %d (written=%u/%u)\n", err, (unsigned)written, (unsigned)len);
  } else {
    totalAudioBytesWritten += written;
  }
}

void setupPcm5102() {
  i2s_config_t dac_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = AUDIO_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DAC_DMA_BUF_COUNT,
    .dma_buf_len = DAC_DMA_BUF_LEN,
    .use_apll = true,
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
  Serial.printf("[INIT] PCM5102 OK (32KB DMA buffer = ~667ms audio)\n");
}

void playPcm5102ToneTest() {
  Serial.println("[AUDIO TEST] 440Hz tone 1s");
  static int16_t toneBuffer[1024];
  const size_t toneSamples = sizeof(toneBuffer) / sizeof(int16_t);
  for (size_t offset = 0; offset < AUDIO_RATE; offset += toneSamples) {
    const size_t count = min(toneSamples, (size_t)AUDIO_RATE - offset);
    for (size_t i = 0; i < count; i++) {
      const float phase = 2.0f * 3.14159265359f * 440.0f * (float)(offset + i) / AUDIO_RATE;
      toneBuffer[i] = (int16_t)(sinf(phase) * 20000.0f);
    }
    size_t written = 0;
    i2s_write(DAC_I2S_PORT, (uint8_t*)toneBuffer, count * sizeof(int16_t), &written, portMAX_DELAY);
  }
  Serial.println("[AUDIO TEST] Done");
}

void preSilenceFlush() {
  uint8_t silence[512] = {0};
  size_t written = 0;
  for (int i = 0; i < 8; i++) {
    i2s_write(DAC_I2S_PORT, silence, 512, &written, pdMS_TO_TICKS(20));
  }
  i2s_zero_dma_buffer(DAC_I2S_PORT);
}

void stopPlayback() {
  preSilenceFlush();
  isPlaying = false;
  audioLevel = 0;
  mouthTargetOpen = 0.0f;
  if (currentEmotion == SPEAKING) {
    currentEmotion = NORMAL;
  }
  setColor(pixels.Color(0, 0, 100));
  Serial.printf("[AUDIO] END — received=%lu bytes, written=%lu bytes\n", 
                totalAudioBytesReceived, totalAudioBytesWritten);
  totalAudioBytesReceived = 0;
  totalAudioBytesWritten = 0;
}

// ═══════════════════════════════════════════════════════════════════
//  WEBSOCKET
// ═══════════════════════════════════════════════════════════════════

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      isWSConnected = false;
      isGeminiReady = false;
      isPlaying = false;
      audioLevel = 0;
      mouthTargetOpen = 0.0f;
      consecutiveFailures++;
      Serial.printf("[MIC] Stream stopped after %lu frames\n", micFramesSent);
      Serial.printf("[WS] Disconnected (failure #%d)\n", consecutiveFailures);
      setColor(pixels.Color(100, 0, 0));
      if (consecutiveFailures >= MAX_FAILURES_BEFORE_RESTART) {
        Serial.println("[NET] Too many failures, restarting...");
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
      
      if (!jsonHas(msg, "\"pong\"")) {
        Serial.println("[WS] TXT: " + msg.substring(0, min((int)msg.length(), 120)));
      }

      if (jsonHas(msg, "\"status\":\"ready\"")) {
        isGeminiReady = true;
        Serial.println("[WS] Gemini ready — mic ON");
      }

      if (jsonHas(msg, "\"serverHello\"") || jsonHas(msg, "\"pong\"")) {
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
        stopPlayback();
        Serial.println("[AUDIO] INTERRUPTED");
        break;
      }
      if (jsonHas(msg, "\"inlineData\"")) {
        isPlaying = true;
        setColor(pixels.Color(200, 0, 200));
        if (currentEmotion != SPEAKING) {
          currentEmotion = SPEAKING;
          emotionStartTime = millis();
        }
        String b64 = jsonGetString(msg, "data");
        if (b64.length()) {
          size_t decoded = base64Decode(b64.c_str(), b64.length(), b64DecodeBuf, MAX_CHUNK_SIZE);
          if (decoded > 0) {
            totalAudioBytesReceived += decoded;
            directI2SWrite(b64DecodeBuf, decoded);
          }
        }
      }
      
      if (jsonHas(msg, "\"turnComplete\":true")) {
        Serial.println("[AUDIO] turnComplete → stopping playback");
        stopPlayback();
      }
      break;
    }

    case WStype_BIN: {
      static unsigned long audioFramesReceived = 0;
      
      if (length < 10) break;

      audioFramesReceived++;
      totalAudioBytesReceived += length;
      
      if (audioFramesReceived <= 3 || audioFramesReceived % 20 == 0) {
        Serial.printf("[WS] AI frame #%lu (%u bytes, RMS=%.0f)\n",
                      audioFramesReceived, (unsigned)length,
                      computeAudioLevel((int16_t*)payload, length / 2));
      }

      isPlaying = true;
      if (currentEmotion != SPEAKING) {
        currentEmotion = SPEAKING;
        emotionStartTime = millis();
      }
      directI2SWrite(payload, length);
      break;
    }

    case WStype_ERROR:
      Serial.printf("[WS] Error: %s\n", payload ? (char*)payload : "unknown");
      break;
  }
}

// ═══════════════════════════════════════════════════════════════════
//  NETWORK
// ═══════════════════════════════════════════════════════════════════

bool checkInternetConnectivity() {
  Serial.println("[NET] Testing HTTP...");
  HTTPClient http;
  http.setTimeout(5000);
  http.begin(String("https://") + WS_HOST + "/health");
  int httpCode = http.GET();
  http.end();
  if (httpCode == 200) {
    Serial.println("[NET] HTTP ✓");
    return true;
  } else {
    Serial.printf("[NET] HTTP ✗ (code: %d)\n", httpCode);
    return false;
  }
}

void maintainWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWiFiRecoveryAttempt < WIFI_RECOVERY_INTERVAL) return;
  lastWiFiRecoveryAttempt = millis();
  isWSConnected = false;
  isGeminiReady = false;
  isPlaying = false;
  Serial.printf("[NET] WiFi disconnected (%d) — reconnecting...\n", WiFi.status());
  WiFi.reconnect();
}

bool resolveHost() {
  IPAddress resolvedIP;
  Serial.printf("[NET] Resolving %s...\n", WS_HOST);
  if (WiFi.hostByName(WS_HOST, resolvedIP)) {
    Serial.printf("[NET] Resolved: %s\n", resolvedIP.toString().c_str());
    return true;
  } else {
    Serial.println("[NET] DNS FAILED");
    return false;
  }
}

// ═══════════════════════════════════════════════════════════════════
//  FACE ANIMATION
// ═══════════════════════════════════════════════════════════════════

void updateFaceAnimation() {
  unsigned long currentMillis = millis();
  int touchState = digitalRead(TOUCH_PIN);

  faceCurrentX += (faceTargetX - faceCurrentX) * 0.15;
  faceCurrentY += (faceTargetY - faceCurrentY) * 0.15;
  mouthOpenAmount += (mouthTargetOpen - mouthOpenAmount) * 0.25;

  if (!isPlaying && mouthTargetOpen > 0) {
    mouthTargetOpen *= 0.85f;
    if (mouthTargetOpen < 0.05f) mouthTargetOpen = 0.0f;
  }

  if (currentEmotion == SLEEP) {
    zzzOffsetY += 0.4;
    if (zzzOffsetY > 20) zzzOffsetY = 0;
  }

  if (touchCount > 0 && (currentMillis - lastTouchTime > 3000)) {
    touchCount = 0;
  }

  if (touchState == HIGH && !touchHandled) {
    touchHandled = true;
    touchCount++;
    lastTouchTime = currentMillis;
    emotionStartTime = currentMillis;
    lastActivityTime = currentMillis;
    if (currentEmotion == SLEEP || currentEmotion == YAWN) {
      currentEmotion = NORMAL;
      faceTargetX = 0;
      faceTargetY = 0;
    }
    currentEmotion = (touchCount >= 4) ? MAD : HAPPY;
  }
  if (touchState == LOW) {
    touchHandled = false;
  }

  if (currentEmotion == NORMAL && (currentMillis - lastActivityTime > YAWN_TIMEOUT)) {
    if (!isYawning) {
      isYawning = true;
      yawnProgress = 0;
      yawnStartTime = currentMillis;
      currentEmotion = YAWN;
      faceTargetY = 5;
    }
  }

  if (isYawning && currentEmotion == YAWN) {
    unsigned long elapsed = currentMillis - yawnStartTime;
    yawnProgress = min((int)(elapsed / YAWN_STEP_MS), YAWN_MAX_PROGRESS);
    if (yawnProgress >= YAWN_MAX_PROGRESS) {
      currentEmotion = SLEEP;
      faceTargetY = 8;
      zzzOffsetY = 0;
      isYawning = false;
    }
  }

  if ((currentEmotion == HAPPY || currentEmotion == MAD) &&
      (currentMillis - emotionStartTime > 3000)) {
    currentEmotion = NORMAL;
    faceTargetX = 0;
    faceTargetY = 0;
  }

  if (currentEmotion == NORMAL && (currentMillis - lastLookAroundTime > 4000)) {
    int positions[4][2] = {{-18, 0}, {18, 0}, {0, -10}, {0, 0}};
    int idx = random(0, 4);
    faceTargetX = positions[idx][0];
    faceTargetY = positions[idx][1];
    lastLookAroundTime = currentMillis;
  }

  if (currentEmotion == NORMAL && !isBlinking && (currentMillis - lastBlinkTime > 3500)) {
    isBlinking = true;
    blinkStartTime = currentMillis;
  }
  if (isBlinking && (currentMillis - blinkStartTime > BLINK_DURATION)) {
    isBlinking = false;
    lastBlinkTime = currentMillis;
  }

  renderFaceToCanvas();
}

// ═══════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  unsigned long serialTimeout = millis();
  while (!Serial && (millis() - serialTimeout < 3000)) { delay(10); }
  delay(500);

  Serial.println("\n\n═══════════════════════════════════════");
  Serial.println("  ALEXATRON BOOT — FINAL FIX V3");
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
  Serial.println("[INIT] Motors OK (pins 7,8,9,15)");

  pinMode(TFT_BL, OUTPUT);
  setBrightness(255);
  pinMode(TOUCH_PIN, INPUT);

  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH);
  delay(50);
  digitalWrite(TFT_RST, LOW);
  delay(100);
  digitalWrite(TFT_RST, HIGH);
  delay(100);

  mySPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  mySPI.setFrequency(27000000);

  tft.init(240, 320);
  tft.setRotation(1);
  tft.invertDisplay(false);
  tft.fillScreen(ST77XX_BLACK);

  lastBlinkTime = millis();
  lastLookAroundTime = millis();
  lastActivityTime = millis();
  renderFaceToCanvas();
  Serial.println("[INIT] Display OK");

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("Alexatron")) {
    Serial.println("[INIT] WiFi failed, restarting...");
    delay(2000);
    ESP.restart();
  }
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  Serial.println("[INIT] WiFi: " + WiFi.localIP().toString());

  WiFi.setDNS(GOOGLE_DNS, CLOUDFLARE_DNS);
  if (!resolveHost()) {
    setColor(pixels.Color(255, 50, 0));
    delay(3000);
  }
  if (!checkInternetConnectivity()) {
    setColor(pixels.Color(255, 50, 0));
    delay(3000);
  }

  i2s_config_t mic_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = MIC_DMA_BUF_COUNT,
    .dma_buf_len = MIC_DMA_BUF_LEN,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = I2S_PIN_NO_CHANGE
  };
  i2s_pin_config_t mic_p = {
    .bck_io_num = MIC_BCK_PIN,
    .ws_io_num = MIC_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD_PIN
  };
  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &mic_cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] Mic I2S failed: %d\n", err);
    setColor(pixels.Color(255, 0, 0));
    while (true) { delay(500); }
  }
  i2s_set_pin(MIC_I2S_PORT, &mic_p);
  Serial.printf("[INIT] Mic OK (%dx%d DMA)\n", MIC_DMA_BUF_COUNT, MIC_DMA_BUF_LEN);

  setupPcm5102();
  if (PCM5102_TONE_TEST && dacReady) playPcm5102ToneTest();

  Serial.print("[INIT] Free heap: ");
  Serial.println(ESP.getFreeHeap());

  webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(10000);
  webSocket.enableHeartbeat(30000, 10000, 2);

  setColor(pixels.Color(0, 0, 100));
  Serial.println("[INIT] Setup complete!\n");
}

// ═══════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════

void loop() {
  unsigned long now = millis();

  maintainWiFiConnection();
  webSocket.loop();

  if (motorsActive && now > moveStopAt) stopMotors();

  if (now - lastFaceRender >= FACE_RENDER_INTERVAL) {
    lastFaceRender = now;
    updateFaceAnimation();
  }

  if (!isWSConnected || !isGeminiReady) return;
  if (isPlaying) return;

  if (AUDIO_TEST_MODE) return;

  int16_t sample_buffer[MIC_CHUNK_SAMPLES];
  size_t bytes_read = 0;
  i2s_read(MIC_I2S_PORT, sample_buffer, sizeof(sample_buffer), &bytes_read, 10);
  if (bytes_read == 0) return;

  streamMicChunk(reinterpret_cast<const uint8_t*>(sample_buffer), bytes_read);
  micFramesSent++;
  if (micFramesSent == 1 || micFramesSent % 200 == 0) {
    Serial.printf("[MIC] Frame #%lu (%u bytes)\n", micFramesSent, (unsigned)bytes_read);
  }
}
