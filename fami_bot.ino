/*
  =====================================================
   FAMI BOT  -  AI + Voice + Wi-Fi + OLED face + RGB + Speaker
   Board: Waveshare ESP32-S3-AUDIO-Board
  =====================================================

  On start-up it says: "Hello, I am your FAMI BOT."

  STATES
    IDLE      blue   - eyes closed (sleeping), LED breathing blue
    LISTENING white  - eyes open, looking at you, waiting for a question
    THINKING  purple - eyes half-closed, pupils circling, asking the AI
    SPEAKING  green  - eyes wide open, speaker says the answer
    -> back to IDLE

  BUTTONS (the 3 small keys on the edge of the board)
    KEY1 = volume DOWN   KEY2 = MUTE on/off   KEY3 = volume UP
    Hold KEY1 or KEY3 to change faster. Volume shows as % for 2 seconds
    where the state name normally is (bottom-left of the left screen).

  AUDIO: 48 kHz, 16-bit, both mics (louder one picked), noise reduction,
         cleaned voice converted to 16 kHz for Gemini (settings: MIC_MODE, NR_LEVEL, MIC_GAIN)

  OFFLINE VOICE: 205 clear recorded clips (file fami_voice.bin, flashed once - see README.txt)
         70+ offline answers: time, date, greetings, feelings, jokes, fun facts, quotes...
         They are used FIRST (fast, free) and whenever Gemini is not available.
         Pressing BOOT with no internet = it tells you the time and date.

  WAKE UP: say "Hi FAMI" or "Hello FAMI" (short). Long talk / TV is ignored on the board
         itself and never sent to Google. Then ask your question after "Yes? How can I help?".

  CLOCK TOOLS (say or type, work offline):
         "timer 5 minutes", "set a timer for 1 hour and 30 minutes", "how much time left", "cancel timer"
         "start stopwatch", "stop stopwatch", "stopwatch", "reset stopwatch"
         "alarm at 7:30 am", "wake me up at six forty five", "what is my alarm", "cancel alarm"
         Ringing stops with any key, BOOT, or typing "stop". The alarm is remembered after a power cut.

  BLUETOOTH MUSIC: not possible - the ESP32-S3 has no "classic" Bluetooth (A2DP).

  FACES: happy, surprised, sad, angry, confused, love (heart eyes), wink, sleepy
         Type "happy face", "sad face", "angry face", "surprise", "love face", "wink", "confused".

  3 WAYS TO TALK TO IT (all work together)
    1. SERIAL : type a question in the Serial Monitor and press Enter
    2. BUTTON : press BOOT -> eyes open (LISTENING) -> just speak your question
    3. VOICE  : say "FAMI", "Hi FAMI" or "Hello FAMI" (+ question) when idle - no button needed
                (just the name -> it says "Yes? How can I help?" and listens)
    Built-in typed answers (no AI): "time", "date", "hello"

  Wiring (screens):
    LEFT  OLED: VCC->3V3 (pin 17)  GND->GND  SCL->GPIO10 (pin 13)  SDA->GPIO11 (pin 11)
    RIGHT OLED: VCC->GPIO3 (pin 10) GND->GND SCL->GPIO4 (pin 8)    SDA->GPIO5 (pin 6)

  Folder must also contain: es8311.h, es8311.cpp, es8311_reg.h
  Libraries: Adafruit SSD1306, Adafruit GFX, Adafruit NeoPixel
  (ESP32-audioI2S is NOT used any more - the voice now comes from Gemini text-to-speech)
  Tools: ESP32S3 Dev Module, USB CDC On Boot Enabled, Flash 16MB,
         Partition "16M Flash (3MB APP/9.9MB FATFS)", PSRAM "OPI PSRAM"
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>     // remembers the alarm after a power cut
#include <functional>
#include <ESP_I2S.h>
#include <Wire.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include "driver/gpio.h"
#include "es8311.h"
#include "esp_partition.h"     // offline voice is stored in the flash storage area

// ---- types used by functions below (must be at the top for the Arduino IDE) ----
enum ModelKind { MK_AUDIO, MK_TEXT, MK_TTS };
struct ModelState {
  const char *name; uint8_t kind;
  float avgMs;                    // measured answer time (0 = not measured yet)
  unsigned long coolUntil;        // don't use before this time (limit reached / busy)
  uint8_t usedThisMin;            // requests in the current minute
  unsigned long minuteStart;
  uint8_t safeRpm;                // learned "requests per minute" limit (0 = unknown)
  bool disabled;                  // model not available for this key
  uint16_t ok, fails;
};


// ================= CHANGE THESE =================
const char* ssid         = "Jinu";
const char* password     = "0502957751";
const char* GEMINI_KEY   = "AQ.Ab8RN6LFQwUpLSMaJmEAlYZSui9g1PGQxtSx41QC1SXoufV6a";
// Models are chosen automatically: FAMI BOT measures their speed, uses the fastest one,
// and silently switches to the next when one reaches its free limit (error 429).
// The list below comes from your key's model list. Order = first guess of speed.
const char* AUDIO_MODELS[] = {        // can understand speech (and text)
  "gemini-flash-lite-latest", "gemini-3.5-flash-lite", "gemini-3.1-flash-lite", "gemini-2.5-flash-lite",
  "gemini-flash-latest", "gemini-3.5-flash", "gemini-3.6-flash", "gemini-3.7-flash" };
const char* TEXT_ONLY_MODELS[] = {    // extra models for typed questions only
  "gemma-4-26b-a4b-it", "gemma-4-31b-it" };
const char* TTS_MODELS[] = {          // voices
  "gemini-3.8-flash-lite-tts", "gemini-3.1-flash-tts-preview", "gemini-3.8-flash-tts", "gemini-2.5-flash-preview-tts" };
const char* VOICE_NAME    = "Aoede";                       // Gemini voice: Puck, Kore, Charon, Aoede, Fenrir...
#define TIMEZONE   "<+04>-4"     // UAE time
#define USE_12H    true
#define HANDS_FREE   true         // true = "Hi FAMI" / "Hello FAMI" voice wake-up while idle
#define WAKE_WORD    "FAMI"       // the name in the wake phrase ("Hi FAMI", "Hello FAMI")
#define WAKE_MAX_MS  2500         // only short sounds (like "Hi FAMI") are checked - long talk/TV is ignored
#define WAKE_GAP_MS  4000         // wait at least this long between two wake checks
#define MIC_GAIN     0x0c         // mic gain: 0x0A 30 dB, 0x0C 34.5 dB, 0x0E 37.5 dB (max)
#define AUDIO_RATE   48000        // 48 kHz = clear voice (16 kHz sounded robotic on this board)
#define MIC_MODE     3            // 1 = left mic, 2 = right mic, 3 = both aligned, 4 = louder mic (auto)
#define NR_LEVEL     2            // noise reduction strength 1 (soft) ... 4 (strong), 0 = off
#define SILENCE_LEVEL 500         // below this counts as silence (end of sentence)
#define OFFLINE_FIRST true        // true = time, date, hello, how are you... use the built-in voice (fast, free)
#define WAKE_LEVEL   800          // how loud speech must be to start a wake check (lower = more sensitive)
#define ASK_LEVEL    400          // how loud speech must be after pressing BOOT
#define START_VOLUME 10          // start-up volume in % (0 - 100)
#define VOL_STEP     5           // % per button press
#define LED_BRIGHT 60            // RGB LED brightness 0 - 255
// ================================================

// ---------- Pins ----------
#define I2C_SDA   11      // left screen + audio chips
#define I2C_SCL   10
#define R_SDA      5      // right screen
#define R_SCL      4
#define R_POWER    3
#define I2S_MCLK  12
#define I2S_BCLK  13
#define I2S_LRCK  14
#define I2S_DOUT  16
#define I2S_DIN   15      // microphone data
#define RGB_PIN   38
#define NUM_LEDS   7
#define BOOT_BTN   0
#define TCA9555_ADDR 0x20
#define ES8311_ADDR  0x18
#define ES7210_ADDR  0x40

// Keys on the pin-expander chip (port 1). If a key does the wrong job,
// watch "Keys raw" in the Serial Monitor and tell me which bit changes.
#define KEY1_BIT  1     // EXIO10 -> volume down
#define KEY2_BIT  2     // EXIO11 -> mute
#define KEY3_BIT  3     // EXIO12 -> volume up

int  volumePct = START_VOLUME;
bool muted = false;
unsigned long volShownAt = 0;      // when the % was last shown

// ---------- Objects ----------
I2SClass i2s;                       // ONE shared audio connection: speaker + mic
es8311_handle_t es_handle = NULL;
Adafruit_SSD1306 leftScreen (128, 64, &Wire,  -1);
Adafruit_SSD1306 rightScreen(128, 64, &Wire1, -1);
Adafruit_NeoPixel pixels(NUM_LEDS, RGB_PIN, NEO_RGB + NEO_KHZ800);
bool leftOK = false, rightOK = false;

// =====================================================
//  STATES
// =====================================================
enum BotState { IDLE, LISTENING, THINKING, SPEAKING };
volatile BotState state = IDLE;
unsigned long stateSince = 0;
const char *STATE_NAMES[] = {"IDLE", "HEAR", "THINK", "TALK"};

void setState(BotState s) {
  state = s;
  stateSince = millis();
  Serial.printf("[state] %s\n", STATE_NAMES[s]);
}

// =====================================================
//  SPEAKER - say() puts text in a queue, speakTask turns it into voice
// =====================================================
#define SPEAK_MAX 400
QueueHandle_t speakQueue;
volatile bool audioBusy = false;
volatile bool speechAbort = false;          // true = stop talking now (a new question came)
volatile bool ringing = false;              // timer / alarm is ringing

void micOff();
// say("text")            -> Gemini voice (or built-in voice if it knows the sentence)
// say("@clip|text")      -> built-in voice clip, text is only shown
void say(String text) {
  micOff();                                   // don't record our own voice
  text.trim();
  if (text.length() >= SPEAK_MAX) text = text.substring(0, SPEAK_MAX - 1);
  char msg[SPEAK_MAX];
  text.toCharArray(msg, sizeof(msg));
  int bar = text.indexOf('|');
  Serial.println("FAMI BOT: " + (text.startsWith("@") && bar > 0 ? text.substring(bar + 1) : text));
  audioBusy = true;
  xQueueSend(speakQueue, msg, portMAX_DELAY);
}
void sayClip(const char *clip, const String &text) { say(String("@") + clip + "|" + text); }

// =====================================================
//  AUDIO HARDWARE (same as your working code)
// =====================================================
bool writeReg(uint8_t chip, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(chip);
  Wire.write(reg); Wire.write(value);
  return Wire.endTransmission() == 0;
}
uint8_t readReg(uint8_t chip, uint8_t reg) {
  Wire.beginTransmission(chip);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  Wire.requestFrom(chip, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

void tcaWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TCA9555_ADDR);
  Wire.write(reg); Wire.write(value);
  Wire.endTransmission();
}
uint8_t tcaRead(uint8_t reg) {
  Wire.beginTransmission(TCA9555_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  Wire.requestFrom(TCA9555_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}
void amplifierEnable() {
  tcaWrite(0x07, tcaRead(0x07) & ~0x01);
  tcaWrite(0x03, tcaRead(0x03) | 0x01);
  delay(100);
}

// ---- speaker amplifier ON only while talking (no Wi-Fi buzz when quiet) ----
TaskHandle_t mainTask = nullptr;             // the setup()/loop() task (owns the I2C wires)
volatile bool ampWant = true;                // speak task: "I need the speaker"
volatile bool ampIsOn = true;
unsigned long ampIdleSince = 0;

void setAmp(bool on) {
  uint8_t v = tcaRead(0x03);
  tcaWrite(0x03, on ? (v | 0x01) : (v & ~0x01));
  ampIsOn = on;
}

// called from the main loop (it owns the I2C wires)
void manageAmp() {
  bool busy = audioBusy || uxQueueMessagesWaiting(speakQueue) > 0;
  if (!busy) ampWant = false;
  if (ampWant || busy) {
    ampIdleSince = millis();
    if (!ampIsOn) setAmp(true);
  } else if (ampIsOn && millis() - ampIdleSince > 400) {
    setAmp(false);                           // quiet for 0.4 s -> amplifier off
  }
}
bool initES8311() {
  es_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  if (es_handle == NULL) return false;
  const es8311_clock_config_t clk = {
    .mclk_inverted = false, .sclk_inverted = false, .mclk_from_mclk_pin = true,
    .mclk_frequency = AUDIO_RATE * 256, .sample_frequency = AUDIO_RATE
  };
  if (es8311_init(es_handle, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  es8311_voice_volume_set(es_handle, 80, NULL);
  // speaker-only mode: switch off its own mic output so it doesn't talk over the real mic
  writeReg(ES8311_ADDR, 0x0A, readReg(ES8311_ADDR, 0x0A) | 0x40);
  return true;
}

// Mic chip - Waveshare setup: 4 channels (TDM), 16-bit each, mics on channel 1 and 3
bool initES7210() {
  if (!writeReg(ES7210_ADDR, 0x00, 0xFF)) return false;
  delay(20);
  writeReg(ES7210_ADDR, 0x00, 0x32);
  writeReg(ES7210_ADDR, 0x09, 0x30);
  writeReg(ES7210_ADDR, 0x0A, 0x30);
  writeReg(ES7210_ADDR, 0x23, 0x2A);
  writeReg(ES7210_ADDR, 0x22, 0x0A);
  writeReg(ES7210_ADDR, 0x20, 0x0A);
  writeReg(ES7210_ADDR, 0x21, 0x2A);
  writeReg(ES7210_ADDR, 0x08, 0x00);
  writeReg(ES7210_ADDR, 0x40, 0x43);
  writeReg(ES7210_ADDR, 0x41, 0x70);
  writeReg(ES7210_ADDR, 0x42, 0x70);
  writeReg(ES7210_ADDR, 0x07, 0x20);
  writeReg(ES7210_ADDR, 0x02, 0x81);
  writeReg(ES7210_ADDR, 0x04, 0x01);
  writeReg(ES7210_ADDR, 0x05, 0x00);
  writeReg(ES7210_ADDR, 0x11, 0x60);
  writeReg(ES7210_ADDR, 0x12, 0x02);       // TDM on
  for (uint8_t r = 0x43; r <= 0x46; r++) writeReg(ES7210_ADDR, r, 0x10 | MIC_GAIN);
  writeReg(ES7210_ADDR, 0x01, 0x00);
  writeReg(ES7210_ADDR, 0x06, 0x00);
  for (uint8_t r = 0x47; r <= 0x4A; r++) writeReg(ES7210_ADDR, r, 0x08);
  writeReg(ES7210_ADDR, 0x4B, 0x00);
  writeReg(ES7210_ADDR, 0x4C, 0x00);
  writeReg(ES7210_ADDR, 0x00, 0x71);
  writeReg(ES7210_ADDR, 0x00, 0x41);
  return true;
}

// =====================================================
//  RGB LED
// =====================================================
void setLED(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NUM_LEDS; i++)
    pixels.setPixelColor(i, pixels.Color(r * LED_BRIGHT / 255, g * LED_BRIGHT / 255, b * LED_BRIGHT / 255));
  pixels.show();
}

float wave(unsigned long periodMs) {           // smooth 0..1..0
  return (1.0 - cos((millis() % periodMs) * 2 * PI / periodMs)) / 2.0;
}

void updateLED() {
  static unsigned long last = 0;
  if (millis() - last < 30) return;
  last = millis();
  if (ringing) { bool on = (millis() / 250) % 2; setLED(on ? 255 : 0, on ? 60 : 0, 0); return; }   // orange flash
  switch (state) {
    case IDLE:      { float k = 0.15 + 0.85 * wave(4000); setLED(0, 0, 255 * k); break; }          // blue breathing
    case LISTENING: setLED(255, 255, 255); break;                                                  // white = listening
    case THINKING:  { float k = 0.3 + 0.7 * wave(800); setLED(180 * k, 0, 255 * k); break; }      // purple pulse
    case SPEAKING:  { float k = 0.5 + 0.5 * wave(300); setLED(0, 255 * k, 0); break; }            // green flicker
  }
}

// =====================================================
//  MICROPHONE  (runs in its own task)
//  Speaker and mic use the same shared audio connection at the same time.
//  We only "listen" when FAMI BOT is idle or listening - never while it talks.
// =====================================================
#define MAX_REC_SEC   6
#define REC_SAMPLES   (AUDIO_RATE * MAX_REC_SEC)
#define PREROLL       (AUDIO_RATE * 3 / 10)  // keep 0.3 s before speech starts
int16_t *recL = nullptr, *recR = nullptr;    // both microphones, 48 kHz
int16_t *preL = nullptr, *preR = nullptr;    // 0.3 s ring buffer (PSRAM)
volatile size_t recLen = 0;
volatile bool micEnabled  = false;          // main loop: please read the mic
volatile bool micReading  = false;          // task: I'm in the middle of a read
volatile bool capturing   = false;          // someone is talking right now
volatile bool voiceReady  = false;          // a full sentence was recorded
volatile int  startLevel  = WAKE_LEVEL;
volatile bool wakeMode    = true;           // idle: only short sounds ("Hi FAMI") are checked
bool micIsOn = false;

void micTask(void *param) {
  int16_t buf[1024];                        // 512 frames x (left + right)
  size_t preHead = 0;
  unsigned long quietSince = 0, enabledAt = 0;
  size_t lastLoud = 0;
  bool wasEnabled = false;

  for (;;) {
    if (!micEnabled || voiceReady) { micReading = false; wasEnabled = false; vTaskDelay(10); continue; }
    if (!wasEnabled) {
      wasEnabled = true; enabledAt = millis(); capturing = false; preHead = 0;
      memset(preL, 0, PREROLL * 2); memset(preR, 0, PREROLL * 2);
    }

    micReading = true;
    size_t frames = i2s.readBytes((char *)buf, sizeof(buf)) / 4;
    micReading = false;
    if (!frames) continue;
    if (millis() - enabledAt < 700) continue;          // ignore 0.7 s: room echo of its own voice

    long peak = 0;
    for (size_t i = 0; i < frames; i++) {
      int16_t l = buf[i * 2], r = buf[i * 2 + 1];
      peak = max(peak, (long)max(abs(l), abs(r)));
      if (!capturing) { preL[preHead] = l; preR[preHead] = r; preHead = (preHead + 1) % PREROLL; }
      else if (recLen < REC_SAMPLES) { recL[recLen] = l; recR[recLen] = r; recLen++; }
    }

    if (!capturing && peak > startLevel) {              // speech started
      capturing = true;
      recLen = 0;
      for (size_t k = 0; k < PREROLL; k++) {
        size_t j = (preHead + k) % PREROLL;
        recL[recLen] = preL[j]; recR[recLen] = preR[j]; recLen++;
      }
      quietSince = millis(); lastLoud = recLen;
    } else if (capturing) {
      if (peak > SILENCE_LEVEL) { quietSince = millis(); lastLoud = recLen; }
      bool silent = millis() - quietSince > 800;
      if (silent || recLen >= REC_SAMPLES) {             // sentence finished
        recLen = min((size_t)recLen, lastLoud + AUDIO_RATE / 5);   // trim quiet end (keep 0.2 s)
        capturing = false;
        if (recLen <= AUDIO_RATE * 4 / 10) { }                     // too short: a click or bump
        else if (wakeMode && recLen > (size_t)AUDIO_RATE * WAKE_MAX_MS / 1000 + PREROLL)
          Serial.printf("(long sound %.1f s ignored - say just \"Hi " WAKE_WORD "\")\n", recLen / (float)AUDIO_RATE);
        else voiceReady = true;
      }
    }
  }
}

void micOn() {
  voiceReady = false;
  micIsOn = true;
  micEnabled = true;          // the task ignores the first 0.4 s (echo / clicks)
}

void micOff() {
  micEnabled = false;
  micIsOn = false;
  capturing = false;
}

// ---------- play sound (any rate mono -> 48 kHz 16-bit stereo) ----------
void playSamples(const int16_t *data, size_t n, float rateIn) {
  if (n < 2) return;
  float gain = muted ? 0.0 : volumePct / 100.0 * 1.6;
  int16_t buf[512];
  int k = 0;
  float step = rateIn / (float)AUDIO_RATE;   // e.g. 24 kHz voice -> 0.5
  for (float pos = 0; pos < n - 1; pos += step) {
    if (speechAbort) break;                  // new question: stop talking right away
    int i = (int)pos;
    float f = pos - i;
    float v = (data[i] * (1 - f) + data[i + 1] * f) * gain;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    buf[k++] = (int16_t)v; buf[k++] = (int16_t)v;
    if (k == 512) { i2s.write((uint8_t *)buf, sizeof(buf)); k = 0; }
  }
  if (k) i2s.write((uint8_t *)buf, k * 2);
  memset(buf, 0, sizeof(buf));               // fill speaker buffer with silence so nothing repeats
  for (int j = 0; j < 60; j++) i2s.write((uint8_t *)buf, sizeof(buf));
}

void playBeep(int freq, int ms) {
  if (!ampIsOn && xTaskGetCurrentTaskHandle() == mainTask) { setAmp(true); delay(40); }
  ampIdleSince = millis();
  static int16_t tone[16000];
  int n = 16000 * ms / 1000;
  if (n > 16000) n = 16000;
  for (int i = 0; i < n; i++) tone[i] = freq ? 5000 * sin(2.0 * PI * freq * i / 16000) : 0;
  playSamples(tone, n, 16000);
}

// =====================================================
//  VOICE CLEANING (same chain as the Voice Lab test)
//  filters -> best mic / both mics -> noise reduction -> gate -> loudness
//  -> 16 kHz copy for Gemini
// =====================================================
float *fL = nullptr, *fR = nullptr;          // working buffers (PSRAM)
int16_t *voice16 = nullptr;                  // cleaned 16 kHz voice for Gemini
size_t voiceLen = 0;
#define NR_FFT 1024
float *fre, *fim, *nwin, *noiseMag, *prevGain, *frEnergy, *frSorted;

struct Biquad {
  float b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  void setup(bool hp, float freq) {
    float w = 2 * PI * freq / AUDIO_RATE, c = cos(w), sn = sin(w), al = sn / (2 * 0.707f), a0 = 1 + al;
    if (hp) { b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; }
    else    { b0 = (1 - c) / 2; b1 = 1 - c;    b2 = (1 - c) / 2; }
    a1 = -2 * c; a2 = 1 - al;
    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
  }
  float run(float x) {
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x; y2 = y1; y1 = y; return y;
  }
};

void filterChannel(const int16_t *in, float *out, size_t n) {
  Biquad h1, h2, l1, l2;
  h1.setup(true, 100); h2.setup(true, 100);          // remove hum
  l1.setup(false, 7000); l2.setup(false, 7000);      // remove hiss
  for (size_t i = 0; i < n; i++) out[i] = l2.run(l1.run(h2.run(h1.run(in[i]))));
}

float rmsOf(const float *d, size_t n) {
  double sq = 0; for (size_t i = 0; i < n; i++) sq += d[i] * d[i];
  return sqrt(sq / n) + 1e-3;
}

void fftRun(float *re, float *im, int n, bool inverse) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = 2 * PI / len * (inverse ? 1 : -1), wr0 = cos(ang), wi0 = sin(ang);
    for (int i = 0; i < n; i += len) {
      float wr1 = 1, wi1 = 0;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = a + len / 2;
        float xr = re[b] * wr1 - im[b] * wi1, xi = re[b] * wi1 + im[b] * wr1;
        re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
        float t = wr1 * wr0 - wi1 * wi0; wi1 = wr1 * wi0 + wi1 * wr0; wr1 = t;
      }
    }
  }
  if (inverse) for (int i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}

void sortFloats(float *a, int n) {
  for (int i = 1; i < n; i++) { float k = a[i]; int j = i - 1; while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; j--; } a[j + 1] = k; }
}

// learns the background sound from the quietest 20 % and removes it (result in fL)
void noiseReduction(size_t len) {
  const int n = NR_FFT, hop = n / 2, bins = n / 2 + 1;
  int frames = (len > (size_t)n) ? (len - n) / hop : 0;
  if (frames < 10 || NR_LEVEL <= 0) return;
  frames = min(frames, 2000);
  for (int f = 0; f < frames; f++) {
    double e = 0;
    for (int i = 0; i < n; i += 4) e += fL[f * hop + i] * fL[f * hop + i];
    frEnergy[f] = frSorted[f] = e;
  }
  sortFloats(frSorted, frames);
  float quiet = frSorted[frames / 5];
  for (int k = 0; k < bins; k++) { noiseMag[k] = 0; prevGain[k] = 1; }
  int count = 0;
  for (int f = 0; f < frames; f++) {
    if (frEnergy[f] > quiet) continue;
    for (int i = 0; i < n; i++) { fre[i] = fL[f * hop + i] * nwin[i]; fim[i] = 0; }
    fftRun(fre, fim, n, false);
    for (int k = 0; k < bins; k++) noiseMag[k] += sqrt(fre[k] * fre[k] + fim[k] * fim[k]);
    count++;
  }
  if (!count) return;
  for (int k = 0; k < bins; k++) noiseMag[k] /= count;

  float over = 1.0 + 0.5 * NR_LEVEL, floorG = 0.25 / NR_LEVEL;
  memset(fR, 0, len * 4);
  for (int f = 0; f < frames; f++) {
    for (int i = 0; i < n; i++) { fre[i] = fL[f * hop + i] * nwin[i]; fim[i] = 0; }
    fftRun(fre, fim, n, false);
    for (int k = 0; k < bins; k++) {
      float mag = sqrt(fre[k] * fre[k] + fim[k] * fim[k]) + 1e-6f;
      float g = (mag - over * noiseMag[k]) / mag;
      if (g < floorG) g = floorG;
      g = 0.6f * prevGain[k] + 0.4f * g;
      prevGain[k] = g;
      fre[k] *= g; fim[k] *= g;
      if (k > 0 && k < n / 2) { fre[n - k] = fre[k]; fim[n - k] = -fim[k]; }
    }
    fftRun(fre, fim, n, true);
    for (int i = 0; i < n; i++) fR[f * hop + i] += fre[i] * nwin[i];
  }
  memcpy(fL, fR, len * 4);
}

// full cleaning: recL / recR (48 kHz)  ->  voice16 (16 kHz)
void cleanVoice() {
  unsigned long t0 = millis();
  size_t len = recLen;
  filterChannel(recL, fL, len);
  filterChannel(recR, fR, len);

  // choose / combine the microphones
  float rL = rmsOf(fL, len), rR = rmsOf(fR, len);
  const char *used = "LEFT";
  if (MIC_MODE == 2 || (MIC_MODE == 4 && rR > rL)) { memcpy(fL, fR, len * 4); used = "RIGHT"; }
  else if (MIC_MODE == 3 && len > (size_t)AUDIO_RATE / 2) {
    float matchR = rL / rR;
    int maxLag = AUDIO_RATE / 2000, bestLag = 0;
    double best = -1e30;
    size_t from = AUDIO_RATE / 8, to = len - AUDIO_RATE / 8;
    for (int lag = -maxLag; lag <= maxLag; lag++) {
      double sum = 0;
      for (size_t i = from; i < to; i += 6) sum += (double)fL[i] * fR[i + lag];
      if (sum > best) { best = sum; bestLag = lag; }
    }
    for (size_t i = 0; i < len; i++) {
      long j = (long)i + bestLag;
      fL[i] = (fL[i] + ((j >= 0 && j < (long)len) ? fR[j] * matchR : 0)) * 0.5f;
    }
    used = "BOTH";
  }

  noiseReduction(len);

  // gentle gate (10 ms pieces) + loudness
  const int F = AUDIO_RATE / 100;
  int frames = min((int)(len / F), 2000);
  for (int f = 0; f < frames; f++) {
    double sq = 0;
    for (int i = 0; i < F; i++) { float v = fL[f * F + i]; sq += v * v; }
    frEnergy[f] = frSorted[f] = sqrt(sq / F);
  }
  sortFloats(frSorted, frames);
  float thr = (frSorted[frames / 5] + 1) * 2.0f;
  float g = 0.3f, peak = 1;
  for (int f = 0; f < frames; f++) {
    float target = frEnergy[f] > thr ? 1.0f : 0.3f;
    for (int i = 0; i < F; i++) {
      g += (target - g) * (target > g ? 0.005f : 0.0002f);
      fL[f * F + i] *= g;
      peak = max(peak, fabsf(fL[f * F + i]));
    }
  }
  size_t usable = (size_t)frames * F;
  float gain = min(20000.0f / peak, 25.0f);

  // 48 kHz -> 16 kHz (every 3rd sample - the 7 kHz filter already removed what would alias)
  voiceLen = usable / 3;
  for (size_t i = 0; i < voiceLen; i++)
    voice16[i] = (int16_t)constrain(fL[i * 3] * gain, -32000.0f, 32000.0f);

  Serial.printf("Voice cleaned in %.1f s (mic: %s, noise reduction %d)\n", (millis() - t0) / 1000.0, used, NR_LEVEL);
}

// =====================================================
//  INTERNET CHECK (background)
// =====================================================
volatile bool internetOK = false;
bool timeReady = false;

void internetCheckTask(void *param) {
  for (;;) {
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) {
      WiFiClient c;
      if (c.connect(IPAddress(8, 8, 8, 8), 53, 3000)) ok = true;
      else if (c.connect(IPAddress(1, 1, 1, 1), 53, 3000)) ok = true;
      c.stop();
    }
    internetOK = ok;
    vTaskDelay(pdMS_TO_TICKS(ok ? 20000 : 5000));
  }
}

// =====================================================
//  AI (Gemini) - runs in its own task so the eyes keep moving
// =====================================================
String aiQuestion, aiAnswer;
volatile bool aiDone = false;

String jsonEscape(const String &s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n' || c == '\r') o += ' ';
    else o += c;
  }
  return o;
}

String shortenForSpeech(String t) {
  t.replace("*", ""); t.replace("#", ""); t.replace("\n", " ");
  t.trim();
  if (t.length() > 200) {                       // cut at the last full sentence
    int cut = t.lastIndexOf('.', 200);
    if (cut < 40) cut = t.lastIndexOf(' ', 200);
    t = t.substring(0, cut + 1);
  }
  return t;
}

int lastStatus = 0;    // last HTTP status from Gemini

const char *SYSTEM_TEXT =
  "You are Fami Bot, a friendly family assistant robot in the UAE. "
  "Answer only the latest question, in one or two short spoken sentences, under 40 words. "
  "No emojis, no lists, no markdown, no stage directions.";

// =====================================================
//  MODEL MANAGER - measures speed, rotates models, respects free limits
// =====================================================
// (ModelKind and ModelState are defined at the top of the file)
ModelState models[24];
int modelCount = 0;
unsigned long googleBlockedUntil = 0;   // only for key / connection problems (all models)
String googleBlockReason = "";

void addModels(const char **list, int n, uint8_t kind) {
  for (int i = 0; i < n && modelCount < 24; i++) {
    ModelState &m = models[modelCount++];
    m = ModelState{list[i], kind, 0, 0, 0, 0, 0, false, 0, 0};
    m.avgMs = 3000 + i * 300;     // first guess: order of the list
  }
}
void initModels() {
  addModels(AUDIO_MODELS, sizeof(AUDIO_MODELS) / sizeof(char *), MK_AUDIO);
  addModels(TEXT_ONLY_MODELS, sizeof(TEXT_ONLY_MODELS) / sizeof(char *), MK_TEXT);
  addModels(TTS_MODELS, sizeof(TTS_MODELS) / sizeof(char *), MK_TTS);
}

bool googleOK() {
  if (String(GEMINI_KEY).startsWith("PUT-")) return false;
  if (!internetOK || WiFi.status() != WL_CONNECTED) return false;
  return (long)(millis() - googleBlockedUntil) >= 0;
}

bool modelReady(ModelState &m) {
  if (m.disabled || (long)(millis() - m.coolUntil) < 0) return false;
  if (millis() - m.minuteStart > 60000) { m.minuteStart = millis(); m.usedThisMin = 0; }
  if (m.safeRpm && m.usedThisMin >= m.safeRpm) return false;          // would hit its limit
  return true;
}

// fastest ready model for a job. needText = typed question (audio models also do text)
ModelState *pickModel(uint8_t kind, ModelState *skip1 = nullptr, ModelState *skip2 = nullptr) {
  ModelState *best = nullptr;
  for (int i = 0; i < modelCount; i++) {
    ModelState &m = models[i];
    bool fits = (kind == MK_TTS) ? m.kind == MK_TTS
              : (kind == MK_AUDIO) ? m.kind == MK_AUDIO
              : (m.kind == MK_AUDIO || m.kind == MK_TEXT);
    if (!fits || &m == skip1 || &m == skip2 || !modelReady(m)) continue;
    if (!best || m.avgMs < best->avgMs) best = &m;
  }
  return best;
}

// any model of this kind usable right now?
bool anyModelReady(uint8_t kind) { return googleOK() && pickModel(kind) != nullptr; }

void modelSuccess(ModelState &m, unsigned long ms) {
  m.ok++;
  m.avgMs = (m.avgMs <= 0) ? ms : m.avgMs * 0.6f + ms * 0.4f;
}

// learn from an error
void modelError(ModelState &m, int code, const String &reply) {
  m.fails++;
  if (code == 429) {
    unsigned long pause = 60000;
    int r = reply.indexOf("retryDelay");
    if (r >= 0) {
      int q = reply.indexOf('"', reply.indexOf(':', r) + 1) + 1;
      long secs = reply.substring(q).toInt();
      if (secs > 0 && secs < 3600) pause = (secs + 2) * 1000UL;
    }
    bool daily = reply.indexOf("PerDay") >= 0 || reply.indexOf("per day") >= 0 || reply.indexOf("_per_day") >= 0;
    if (daily) pause = 3UL * 3600000;                                 // daily limit: rest for 3 hours
    if (m.usedThisMin > 1 && !daily) m.safeRpm = m.usedThisMin - 1;   // learn its per-minute limit
    m.coolUntil = millis() + pause;
    Serial.printf("   [%s limit reached - resting %lu s]\n", m.name, pause / 1000);
  } else if (code == 404 || (code == 400 && reply.indexOf("not found") >= 0) || reply.indexOf("not supported") >= 0) {
    m.disabled = true;
    Serial.printf("   [%s not available - removed]\n", m.name);
  } else if (code == 400 && reply.indexOf("API key") < 0) {
    m.coolUntil = millis() + 10UL * 60000;                            // this model doesn't like the request
  } else if (code == 401 || code == 403 || (code == 400 && reply.indexOf("API key") >= 0)) {
    googleBlockedUntil = millis() + 10UL * 60000; googleBlockReason = "API key";
    Serial.println(">> Google refused the API key - offline voice for 10 min");
  } else if (code == 503 || code == 500) {
    m.coolUntil = millis() + 30000;
  } else if (code <= 0) {
    googleBlockedUntil = millis() + 20000; googleBlockReason = "no connection";
  }
}

void printModels() {
  const char *title[3] = {"SPEECH + TEXT", "TEXT ONLY", "VOICE"};
  Serial.println("\n----- model ranking (fastest first) -----");
  for (int k = 0; k < 3; k++) {
    Serial.printf(" %s\n", title[k]);
    bool shown[24] = {false};
    for (int round = 0; round < modelCount; round++) {
      int best = -1;
      for (int i = 0; i < modelCount; i++)
        if (!shown[i] && models[i].kind == k && (best < 0 || models[i].avgMs < models[best].avgMs)) best = i;
      if (best < 0) break;
      shown[best] = true;
      ModelState &m = models[best];
      long restS = (long)(m.coolUntil - millis()) / 1000;
      Serial.printf("  %-30s %5.0f ms  ok %u  fail %u  %s", m.name, m.avgMs, m.ok, m.fails,
                    m.disabled ? "(not available)" : "");
      if (!m.disabled && restS > 0) Serial.printf("(resting %ld s)", restS);
      Serial.println();
    }
  }
  Serial.println("-----------------------------------------");
}

// one HTTP request to one model. Returns the answer text ("" = failed)
String postGemini(ModelState &m, uint8_t *body, size_t len) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + String(m.name) + ":generateContent";
  if (!http.begin(client, url)) { lastStatus = -1; modelError(m, -1, ""); return ""; }
  http.setTimeout(15000);
  http.setConnectTimeout(6000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", GEMINI_KEY);

  m.usedThisMin++;
  unsigned long t0 = millis();
  int code = http.POST(body, len);
  String response = (code > 0) ? http.getString() : "";
  http.end();
  unsigned long ms = millis() - t0;
  lastStatus = code;
  Serial.printf("Gemini (%s) status: %d  (%.1f s)\n", m.name, code, ms / 1000.0);

  if (code != 200) { modelError(m, code, response); return ""; }
  int t = response.indexOf("\"text\"");
  if (t < 0) { m.fails++; return ""; }
  modelSuccess(m, ms);

  int q = response.indexOf('"', response.indexOf(':', t) + 1) + 1;
  String text;
  while (q < (int)response.length()) {
    char c = response[q];
    if (c == '\\' && q + 1 < (int)response.length()) {
      char e = response[q + 1];
      text += (e == 'n') ? ' ' : e;
      q += 2; continue;
    }
    if (c == '"') break;
    text += c; q++;
  }
  text.trim();
  return text;
}

// try the fastest ready model, then the next ones (max 3), never waiting
// makeBody(model) builds the request for that model (Gemma needs a slightly different one)
String askModels(uint8_t kind, std::function<String(ModelState &)> makeBody, uint8_t *rawBody = nullptr, size_t rawLen = 0) {
  if (!googleOK()) { lastStatus = 429; return ""; }
  ModelState *tried[3] = {nullptr, nullptr, nullptr};
  for (int a = 0; a < 3; a++) {
    ModelState *m = pickModel(kind, tried[0], tried[1]);
    if (!m) break;
    tried[a] = m;
    String ans;
    if (rawBody) ans = postGemini(*m, rawBody, rawLen);
    else { String b = makeBody(*m); ans = postGemini(*m, (uint8_t *)b.c_str(), b.length()); }
    if (ans.length()) return ans;
    if (!googleOK()) break;                    // key / connection problem: stop
  }
  if (!pickModel(kind)) lastStatus = 429;      // everything resting
  return "";
}

// speed test at start-up: one tiny request per speech/text model (runs in the background)
void speedTestTask(void *p) {
  delay(4000);
  Serial.println("Speed test of the AI models...");
  for (int i = 0; i < modelCount; i++) {
    ModelState &m = models[i];
    if (m.kind == MK_TTS || !googleOK()) continue;
    bool gemma = String(m.name).startsWith("gemma");
    String b = "{\"contents\":[{\"parts\":[{\"text\":\"Reply with the single word: ok\"}]}]}";
    (void)gemma;
    postGemini(m, (uint8_t *)b.c_str(), b.length());
    delay(300);
  }
  printModels();
  vTaskDelete(NULL);
}

String failMessage() {
  if (lastStatus == 503 || lastStatus == 429 || googleBlockReason == "quota") return "@busy|My brain is busy right now. Please ask me again in a moment.";
  return "@offline|Sorry, my brain is offline right now.";
}

// ---------- typed question ----------
// today's date & time, added to every AI question (the AI doesn't know it by itself)
String nowContext() {
  struct tm t; char buf[80];
  if (!timeReady || !getLocalTime(&t, 0)) return "";
  strftime(buf, sizeof(buf), " Right now it is %A, %d %B %Y, %I:%M %p in the UAE.", &t);
  return String(buf);
}

String askGemini(const String &question) {
  if (String(GEMINI_KEY).startsWith("PUT-")) return "@offline|My AI key is not set yet.";
  auto makeBody = [&](ModelState &m) -> String {
    if (String(m.name).startsWith("gemma"))        // Gemma: no system instruction field
      return "{\"contents\":[{\"parts\":[{\"text\":\"" + String(SYSTEM_TEXT) + nowContext() + " Question: " + jsonEscape(question) + "\"}]}]}";
    return "{\"system_instruction\":{\"parts\":[{\"text\":\"" + String(SYSTEM_TEXT) + nowContext() + "\"}]},"
           "\"contents\":[{\"parts\":[{\"text\":\"" + jsonEscape(question) + "\"}]}]}";
  };
  String a = askModels(MK_TEXT, makeBody);
  return a.length() ? shortenForSpeech(a) : failMessage();
}

// ---------- spoken question (audio) ----------
const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64Into(char *out, const uint8_t *in, size_t n) {
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[o++] = B64[(v >> 18) & 63];
    out[o++] = B64[(v >> 12) & 63];
    out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
  }
  return o;
}

void makeWav(uint8_t *h, uint32_t dataBytes) {
  uint32_t fileSize = dataBytes + 36, rate = 16000, byteRate = 32000;
  memcpy(h, "RIFF", 4); memcpy(h + 4, &fileSize, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  uint32_t fmtLen = 16; uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
  memcpy(h + 16, &fmtLen, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &rate, 4);   memcpy(h + 28, &byteRate, 4);
  memcpy(h + 32, &align, 2);  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);  memcpy(h + 40, &dataBytes, 4);
}

// wakeCheck = true  -> only answer if they called "FAMI"
// result: "IGNORE", "WAKE", or "HEARD: ... ANSWER: ..."
String askGeminiAudio(bool wakeCheck) {
  if (String(GEMINI_KEY).startsWith("PUT-")) return "@offline|My AI key is not set yet.";

  String instr = wakeCheck
    ? "Transcribe this short English audio exactly. The speaker may say the name Fami (pronounced FAH-mee), "
      "for example 'Hi Fami' or 'Hello Fami'. Reply with ONLY the words spoken, nothing else. "
      "If there are no clear words, reply exactly: (none)"
    : "The audio is a person asking you ONE new question. Ignore any robot voice or background sound. "
      "Reply exactly in this format: HEARD: <their words> ANSWER: <your short answer>. "
      "If nothing clear was said, reply exactly: IGNORE";

  String part1 =
    "{\"system_instruction\":{\"parts\":[{\"text\":\"" + String(SYSTEM_TEXT) + nowContext() + "\"}]},"
    "\"contents\":[{\"parts\":[{\"text\":\"" + instr + "\"},"
    "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";
  String part2 = "\"}}]}]}";

  // clean the 48 kHz recording -> clear 16 kHz voice
  cleanVoice();

  // WAV = 44-byte header + cleaned voice
  size_t audioBytes = voiceLen * 2;
  uint8_t *wav = (uint8_t *)ps_malloc(44 + audioBytes);
  if (!wav) return "@offline|Sorry, I ran out of memory.";
  makeWav(wav, audioBytes);
  memcpy(wav + 44, voice16, audioBytes);

  size_t b64Len = 4 * ((44 + audioBytes + 2) / 3);
  size_t total = part1.length() + b64Len + part2.length();
  char *body = (char *)ps_malloc(total + 1);
  if (!body) { free(wav); return "@offline|Sorry, I ran out of memory."; }
  memcpy(body, part1.c_str(), part1.length());
  base64Into(body + part1.length(), wav, 44 + audioBytes);
  memcpy(body + part1.length() + b64Len, part2.c_str(), part2.length());
  free(wav);

  Serial.printf("Sending %.1f s of voice to Gemini...\n", voiceLen / 16000.0);
  String a = askModels(MK_AUDIO, nullptr, (uint8_t *)body, total);
  free(body);
  return a.length() ? a : failMessage();
}

volatile int aiMode = 0;          // 0 = typed, 1 = voice after BOOT, 2 = voice wake-up check

// =====================================================
//  TEXT-TO-SPEECH with Gemini  ->  raw voice samples (usually 24 kHz)
// =====================================================
#define TTS_MAX_SAMPLES (24000 * 30)          // up to 30 s of speech
int16_t *ttsBuf = nullptr;

int8_t b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// returns number of samples (0 = failed); rate is filled in
size_t fetchTTS(const String &text, ModelState &mdl, int &rate) {
  const char *model = mdl.name;
  rate = 24000;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + String(model) + ":generateContent";
  if (!http.begin(client, url)) { lastStatus = -1; modelError(mdl, -1, ""); return 0; }
  mdl.usedThisMin++;
  http.useHTTP10(true);                       // plain reply (no chunks) so we can read it as a stream
  http.setTimeout(15000);
  http.setConnectTimeout(6000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", GEMINI_KEY);

  String body =
    "{\"contents\":[{\"parts\":[{\"text\":\"" + jsonEscape(text) + "\"}]}],"     // ONLY the words to say
    "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
    "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"" + String(VOICE_NAME) + "\"}}}}}";

  unsigned long t0 = millis();
  int code = http.POST(body);
  lastStatus = code;
  if (code != 200) {
    String r = (code > 0) ? http.getString() : HTTPClient::errorToString(code);
    Serial.printf("Voice (%s) status: %d\n%s\n", model, code, r.substring(0, 200).c_str());
    http.end();
    modelError(mdl, code, r);
    return 0;
  }

  // read the reply as a stream: find "rate=" and "data": "<base64>"
  WiFiClient *st = http.getStreamPtr();
  const char *key = "\"data\"";
  int keyPos = 0, phase = 0;                  // 0 = looking for "data", 1 = waiting for quote, 2 = in base64
  String head;
  uint32_t acc = 0; int bits = 0;
  size_t nBytes = 0;
  uint8_t *out = (uint8_t *)ttsBuf;
  const size_t maxBytes = TTS_MAX_SAMPLES * 2;
  uint8_t chunk[512];
  unsigned long last = millis();

  while (millis() - last < 15000) {
    int avail = st->available();
    if (avail <= 0) {
      if (!http.connected()) break;
      delay(2);
      continue;
    }
    int n = st->readBytes(chunk, min(avail, (int)sizeof(chunk)));
    last = millis();
    for (int i = 0; i < n; i++) {
      char c = chunk[i];
      if (phase == 0) {
        if (head.length() < 1500) head += c;
        keyPos = (c == key[keyPos]) ? keyPos + 1 : (c == key[0] ? 1 : 0);
        if (key[keyPos] == 0) phase = 1;
      } else if (phase == 1) {
        if (c == '"') phase = 2;
      } else if (phase == 2) {
        if (c == '"') { phase = 3; break; }
        int8_t v = b64val(c);
        if (v < 0) continue;                  // skip '=', '\\' etc.
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) {
          bits -= 8;
          if (nBytes < maxBytes) out[nBytes++] = (acc >> bits) & 0xFF;
        }
      }
    }
    if (phase == 3) break;
  }
  http.end();

  int r = head.indexOf("rate=");
  if (r >= 0) rate = head.substring(r + 5).toInt();
  if (rate < 8000 || rate > 48000) rate = 24000;
  Serial.printf("Voice (%s): %.1f s of speech, fetched in %.1f s\n",
                model, nBytes / 2.0 / rate, (millis() - t0) / 1000.0);
  if (nBytes > 0) modelSuccess(mdl, millis() - t0); else mdl.fails++;
  return nBytes / 2;
}

// =====================================================
//  OFFLINE VOICE - 205 recorded clips stored in the flash storage area
//  (file fami_voice.bin, flashed once with esptool - see README.txt)
// =====================================================
const int8_t ADPCM_IDX[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
const int16_t ADPCM_STEP[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
  130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,
  1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
  15289,16818,18500,20350,22385,24623,27086,29794,32767};

struct VoiceEntry { char name[20]; uint32_t offset, samples, bytes; };
const esp_partition_t *voicePart = nullptr;
VoiceEntry *voiceIndex = nullptr;
int voiceCount = 0, voiceRate = 16000;
static uint8_t clipChunk[2048];               // internal RAM, safe for flash reads

bool loadVoice() {
  voicePart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "ffat");
  if (!voicePart) voicePart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
  if (!voicePart) voicePart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
  if (!voicePart) { Serial.println("Offline voice: no storage area found (check Partition Scheme)"); return false; }

  uint8_t hdr[16];
  esp_partition_read(voicePart, 0, hdr, 16);
  if (memcmp(hdr, "FAMIVOX1", 8) != 0) {
    Serial.printf("Offline voice NOT installed. Flash fami_voice.bin to address 0x%X (see README.txt)\n",
                  (unsigned)voicePart->address);
    return false;
  }
  memcpy(&voiceCount, hdr + 8, 4);
  memcpy(&voiceRate, hdr + 12, 4);
  voiceIndex = (VoiceEntry *)ps_malloc(voiceCount * sizeof(VoiceEntry));
  // read the index in small pieces too
  for (size_t done = 0, total = voiceCount * sizeof(VoiceEntry); done < total; ) {
    size_t part = min((size_t)sizeof(clipChunk), total - done);
    if (esp_partition_read(voicePart, 16 + done, clipChunk, part) != ESP_OK) { voiceCount = 0; return false; }
    memcpy((uint8_t *)voiceIndex + done, clipChunk, part);
    done += part;
  }
  if (!voiceIndex) { voiceCount = 0; return false; }
  Serial.printf("Offline voice: %d clips ready (storage at 0x%X)\n", voiceCount, (unsigned)voicePart->address);
  return true;
}

bool hasClip(const char *name) {
  for (int c = 0; c < voiceCount; c++) if (strcmp(voiceIndex[c].name, name) == 0) return true;
  return false;
}

// play one clip by name, returns false if it doesn't exist or can't be read.
// The clip is read in small pieces through internal memory (reading big blocks
// straight into PSRAM can fail silently and replay the PREVIOUS clip).

bool playClip(const char *name) {
  for (int c = 0; c < voiceCount; c++) {
    if (strcmp(voiceIndex[c].name, name) != 0) continue;
    VoiceEntry &e = voiceIndex[c];
    uint32_t n = min(e.samples, (uint32_t)TTS_MAX_SAMPLES);
    uint32_t needBytes = (n + 1) / 2;
    int pred = 0, idx = 0;
    uint32_t outPos = 0;
    for (uint32_t done = 0; done < needBytes; ) {
      uint32_t part = min((uint32_t)sizeof(clipChunk), needBytes - done);
      esp_err_t err = esp_partition_read(voicePart, e.offset + done, clipChunk, part);
      if (err != ESP_OK) { Serial.printf("(voice read error %d in clip %s)\n", err, name); return false; }
      for (uint32_t b = 0; b < part; b++) {
        for (int half = 0; half < 2 && outPos < n; half++) {      // 2 sounds per byte
          int code = (clipChunk[b] >> (4 * half)) & 15, step = ADPCM_STEP[idx], diff = step >> 3;
          if (code & 4) diff += step;
          if (code & 2) diff += step >> 1;
          if (code & 1) diff += step >> 2;
          pred += (code & 8) ? -diff : diff;
          pred = constrain(pred, -32768, 32767);
          idx = constrain(idx + ADPCM_IDX[code], 0, 88);
          ttsBuf[outPos++] = pred;
        }
      }
      done += part;
    }
    playSamples(ttsBuf, outPos, voiceRate);
    return true;
  }
  Serial.printf("(no voice clip named %s)\n", name);
  return false;
}

void playNumber(int n) { char k[8]; snprintf(k, sizeof(k), "n%d", n); playClip(k); }

void speakTimeOffline() {
  struct tm t;
  if (!timeReady || !getLocalTime(&t, 0)) { playClip("notime"); return; }
  int h = t.tm_hour % 12; if (h == 0) h = 12;
  playClip("timeis");
  playNumber(h);
  if (t.tm_min == 0) playClip("oclock");
  else { if (t.tm_min < 10) playClip("oh"); playNumber(t.tm_min); }
  playClip(t.tm_hour < 12 ? "am" : "pm");
}

void speakDateOffline(bool withYear) {
  struct tm t;
  if (!timeReady || !getLocalTime(&t, 0)) { playClip("notime"); return; }
  char k[8];
  playClip("todayis");
  snprintf(k, sizeof(k), "d%d", t.tm_wday); playClip(k);
  snprintf(k, sizeof(k), "o%d", t.tm_mday); playClip(k);
  snprintf(k, sizeof(k), "m%d", t.tm_mon);  playClip(k);
  if (withYear) { snprintf(k, sizeof(k), "y%d", t.tm_year + 1900); playClip(k); }
}

void greetingByTime() {
  struct tm t;
  if (timeReady && getLocalTime(&t, 0))
    playClip(t.tm_hour < 12 ? "goodmorning" : (t.tm_hour < 17 ? "goodafternoon" : "goodevening"));
}

// =====================================================
//  CLOCK TOOLS - timer, stopwatch, alarm
// =====================================================
Preferences prefs;
bool famiTimerOn = false;           unsigned long famiTimerEnd = 0;  uint32_t famiTimerSecs = 0;
bool swRunning = false;         unsigned long swStart = 0, swAccum = 0;   // stopwatch (ms)
int  alarmMin = -1;             // alarm time in minutes after midnight, -1 = no alarm
long lastAlarmKey = -1;
unsigned long ringStart = 0, lastRingBeep = 0;   // (ringing flag is declared at the top)

uint32_t famiTimerLeft()  { return famiTimerOn ? ((long)(famiTimerEnd - millis()) > 0 ? (famiTimerEnd - millis() + 999) / 1000 : 0) : 0; }
uint32_t swElapsedS() { return (swAccum + (swRunning ? millis() - swStart : 0)) / 1000; }

// says e.g. "five minutes and ten seconds"
void speakDuration(uint32_t secs) {
  uint32_t h = secs / 3600, m = (secs % 3600) / 60, sc = secs % 60;
  bool said = false;
  if (h) { playNumber(min(h, (uint32_t)59)); playClip(h == 1 ? "u_hour" : "u_hours"); said = true; }
  if (m) { if (said) playClip("and"); playNumber(m); playClip(m == 1 ? "u_minute" : "u_minutes"); said = true; }
  if (sc || !said) { if (said) playClip("and"); if (sc) playNumber(sc); else playClip("n1"); playClip(sc == 1 || !sc ? "u_second" : "u_seconds"); }
}

// says a clock time e.g. "seven thirty A.M."
void speakHM(int h24, int m) {
  int h = h24 % 12; if (h == 0) h = 12;
  playNumber(h);
  if (m == 0) playClip("oclock");
  else { if (m < 10) playClip("oh"); playNumber(m); }
  playClip(h24 < 12 ? "am" : "pm");
}

// "special" clips start with '#': built from several pieces
bool playSpecial(const String &c) {
  struct tm t; char k[8];
  bool tk = timeReady && getLocalTime(&t, 0);
  if (c == "#time")  { speakTimeOffline(); return true; }
  if (c == "#date")  { speakDateOffline(true); return true; }
  if (c == "#day")   { if (!tk) return playClip("notime"); playClip("todayis"); snprintf(k, 8, "d%d", t.tm_wday); return playClip(k); }
  if (c == "#month") { if (!tk) return playClip("notime"); playClip("monthis"); snprintf(k, 8, "m%d", t.tm_mon); return playClip(k); }
  if (c == "#year")  { if (!tk) return playClip("notime"); playClip("yearis"); snprintf(k, 8, "y%d", t.tm_year + 1900); return playClip(k); }
  if (c == "#hello") { greetingByTime(); playClip("intro"); return playClip("yeshelp"); }
  if (c == "#online") return playClip(internetOK ? "online" : "offline");
  if (c == "#count") { for (int i = 1; i <= 10; i++) playNumber(i); return true; }
  if (c == "#timerset") { playClip("timerset"); speakDuration(famiTimerSecs); return true; }
  if (c == "#timeleft") { if (!famiTimerOn) return playClip("notimer"); playClip("timeleft"); speakDuration(famiTimerLeft()); return true; }
  if (c == "#swstop")   { playClip("swstop"); speakDuration(swElapsedS()); return true; }
  if (c == "#swtime")   { if (!swRunning && !swAccum) return playClip("noswatch"); playClip("swtime"); speakDuration(swElapsedS()); return true; }
  if (c == "#alarmset") { playClip("alarmset"); speakHM(alarmMin / 60, alarmMin % 60); return true; }
  if (c == "#alarmis")  { if (alarmMin < 0) return playClip("noalarm"); playClip("alarmis"); speakHM(alarmMin / 60, alarmMin % 60); return true; }
  return false;
}

void speakTask(void *param) {
  char msg[SPEAK_MAX];
  for (;;) {
    if (xQueueReceive(speakQueue, msg, portMAX_DELAY) != pdTRUE) continue;
    audioBusy = true;                         // still talking (there may be several sentences)
    bool ampWasOff = !ampIsOn;
    ampWant = true;
    for (int w = 0; w < 60 && !ampIsOn; w++) delay(5);   // main loop switches the amplifier on
    if (ampWasOff) delay(40);                              // let it settle (no pop)
    if (speechAbort) { audioBusy = false; continue; }   // old sentence after a new question: skip
    String text = msg, clip = "";
    if (text.startsWith("@")) {               // "@clip|text"
      int bar = text.indexOf('|');
      clip = text.substring(1, bar > 0 ? bar : text.length());
      text = bar > 0 ? text.substring(bar + 1) : "";
    }

    // 1. built-in voice clip
    if (clip.length() && voiceCount > 0) {
      if (clip.startsWith("#") ? playSpecial(clip) : playClip(clip.c_str())) { audioBusy = false; continue; }
    }

    // 2. Gemini voice (only if Google is OK right now - otherwise straight to the built-in voice)
    size_t n = 0; int rate = 24000;
    if (text.length() && googleOK()) {
      ModelState *t1 = pickModel(MK_TTS), *t2 = nullptr;
      if (t1) n = fetchTTS(text, *t1, rate);
      if (n == 0 && t1 && googleOK() && (t2 = pickModel(MK_TTS, t1))) n = fetchTTS(text, *t2, rate);   // next voice model
    }
    if (speechAbort) { speechAbort = false; audioBusy = false; continue; }
    if (n > 0) playSamples(ttsBuf, n, rate);
    else if (voiceCount > 0) {                // 3. Gemini voice failed -> built-in "offline" sentence
      Serial.println("(Gemini voice not available - using built-in voice)");
      if (!(clip.length() && clip.startsWith("#") && playSpecial(clip))) playClip(lastStatus == 503 || lastStatus == 429 ? "busy" : "offline");
    } else {
      Serial.println("(no voice: offline voice NOT installed - flash fami_voice.bin, see README)");
      playBeep(880, 120); delay(60); playBeep(660, 160);
    }
    audioBusy = false;
  }
}

volatile int aiToken = 0;           // changes when a request is cancelled (timeout)

void aiTask(void *param) {
  int myToken = aiToken;
  String a = (aiMode == 0) ? askGemini(aiQuestion) : askGeminiAudio(aiMode == 2);
  if (myToken == aiToken) { aiAnswer = a; aiDone = true; }   // ignore late answers after a timeout
  vTaskDelete(NULL);
}

// =====================================================
//  VOLUME + KEYS
// =====================================================
void applyVolume() {
  volShownAt = millis();                       // volume is applied when sound plays
  if (muted) Serial.println("Volume: MUTE");
  else       Serial.printf("Volume: %d%%\n", volumePct);
}

void readKeys() {
  static unsigned long lastRead = 0, lastRepeat = 0;
  static uint8_t lastPressed = 0;
  static uint8_t lastRaw = 0xFF;
  if (millis() - lastRead < 30) return;           // check 33 times a second
  lastRead = millis();

  uint8_t raw = tcaRead(0x01);                     // input port 1
  if ((raw | 0x01) != (lastRaw | 0x01)) {          // debug: shows which bit a key uses (bit 0 = amplifier, ignored)
    Serial.print("Keys raw: ");
    for (int b = 7; b >= 0; b--) Serial.print((raw >> b) & 1);
    Serial.println();
    lastRaw = raw;
  }
  uint8_t pressed = ~raw & ((1 << KEY1_BIT) | (1 << KEY2_BIT) | (1 << KEY3_BIT));   // pressed = 0

  static unsigned long pressStart = 0;
  bool fresh = false, act = false;
  if (pressed && pressed != lastPressed) {                    // just pressed
    fresh = act = true;
    pressStart = lastRepeat = millis();
  } else if (pressed && millis() - pressStart > 600 && millis() - lastRepeat > 200) {
    act = true;                                               // held down: repeat
    lastRepeat = millis();
  }

  if (act && fresh && ringing) { stopRinging(); lastPressed = pressed; return; }   // any key stops the alarm
  if (act) {
    if (pressed & (1 << KEY2_BIT)) {
      if (fresh) { muted = !muted; applyVolume(); }           // mute toggles once per press
    } else if (pressed & (1 << KEY1_BIT)) {
      muted = false; volumePct = max(0, volumePct - VOL_STEP); applyVolume();
    } else if (pressed & (1 << KEY3_BIT)) {
      muted = false; volumePct = min(100, volumePct + VOL_STEP); applyVolume();
    }
  }
  lastPressed = pressed;
}

// small speaker icon (7 x 8 px)
void drawSpeakerIcon(Adafruit_SSD1306 &scr, int x, int y) {
  scr.fillRect(x, y + 2, 2, 4, SSD1306_WHITE);
  scr.fillTriangle(x + 2, y + 4, x + 5, y, x + 5, y + 7, SSD1306_WHITE);
}

// =====================================================
//  BUILT-IN ANSWERS (no AI needed)
// =====================================================
String timeAsWords() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return "";
  char buf[20];
  strftime(buf, sizeof(buf), "%I:%M %p", &t);
  String s = buf;
  if (s.startsWith("0")) s = s.substring(1);
  return s;
}

// =====================================================
//  OFFLINE ANSWERS - work without Gemini (typed, or heard while online)
//  keys: words separated by |   clip: voice clip (# = built from pieces)
// =====================================================
enum Expr { EX_NONE, EX_HAPPY, EX_SURPRISED, EX_SAD, EX_CONFUSED, EX_LOVE, EX_WINK, EX_ANGRY, EX_SLEEPY };
struct QA { const char *keys; const char *clip; const char *text; uint8_t expr; };

static const QA QA_LIST[] = {
  // --- time & date ---
  {"what time|the time|time now|time please|time", "#time", "The time is ...", EX_HAPPY},
  {"what date|the date|date today|today's date|date", "#date", "Today is ...", EX_HAPPY},
  {"what day|which day|day today|day name|name of the day|what is today", "#day", "Today is ...", EX_HAPPY},
  {"what month|which month", "#month", "This month is ...", EX_HAPPY},
  {"what year|which year", "#year", "The year is ...", EX_HAPPY},
  // --- greetings ---
  {"good morning", "gm_full", "Good morning! I hope you have a wonderful day.", EX_HAPPY},
  {"good afternoon", "ga_full", "Good afternoon! How is your day going?", EX_HAPPY},
  {"good evening", "ge_full", "Good evening! How was your day?", EX_HAPPY},
  {"good night|goodnight|sleep now", "gn_full", "Good night! Sleep well and sweet dreams.", EX_SLEEPY},
  {"bye|goodbye|see you", "bye", "Goodbye! See you soon.", EX_WINK},
  {"how are you|how r u|how do you do|how you doing", "howareyou", "I am fine, thank you! How are you?", EX_HAPPY},
  {"i am fine|i'm fine|i am good|i'm good|i am ok|i'm ok", "great", "That is great to hear!", EX_HAPPY},
  {"i am sad|i'm sad|feel sad|i am upset", "sad", "I am sorry you feel sad. I am here for you.", EX_SAD},
  {"i am happy|i'm happy|feel happy", "happy", "Yay! That makes me happy too!", EX_HAPPY},
  {"i am bored|i'm bored|bored", "bored", "Let's have some fun! Ask me for a joke or a fun fact.", EX_WINK},
  {"i am tired|i'm tired|tired", "tired", "You should take a little rest, and drink some water too.", EX_SLEEPY},
  {"i am hungry|i'm hungry|hungry", "hungry", "Maybe it is time for a healthy snack!", EX_HAPPY},
  {"i'm home|i am home|i'm back|i am back", "welcomehome", "Welcome home! I missed you.", EX_LOVE},
  {"thank|thanks|shukran", "thanks", "You are welcome!", EX_HAPPY},
  {"sorry", "noproblem", "No problem at all!", EX_HAPPY},
  {"i love you|love you", "loveyou", "Aww, I love you too!", EX_LOVE},
  {"you are smart|good job|well done|you are good|you are great|clever", "kind", "Thank you! You are very kind.", EX_LOVE},
  {"stupid|you are bad|useless|dumb", "learning", "I am still learning. I will try my best!", EX_SAD},
  {"are you my friend|be my friend|friend", "friend", "Of course! I am your friend forever.", EX_LOVE},
  {"salam|assalam|salaam", "pray", "Salaam alaikum! Peace be upon you.", EX_HAPPY},
  {"eid", "eid", "Eid Mubarak! Wishing you and your family joy and peace.", EX_HAPPY},
  {"ramadan", "ramadan", "Ramadan Kareem! Wishing you a blessed month.", EX_HAPPY},
  {"birthday", "birthday", "Happy birthday to you! Have a wonderful day!", EX_LOVE},
  {"congratulat|i passed|i won", "congrats", "Congratulations! I am so proud of you!", EX_SURPRISED},
  // --- about FAMI ---
  {"your name|who are you|what are you called", "name", "My name is FAMI BOT, your family assistant.", EX_HAPPY},
  {"how old|your age", "age", "I was born in twenty twenty six, so I am still very young!", EX_HAPPY},
  {"who made you|who created you|who built you", "maker", "I was built by my family, with a little help from A.I.", EX_LOVE},
  {"where do you live|where are you from|where are you", "home", "I live right here at home, in the U.A.E.", EX_HAPPY},
  {"what can you do|help me|your skills", "cando", "I can tell you the time and the date, tell jokes and fun facts, and answer questions when I am online.", EX_HAPPY},
  {"are you a robot|you robot", "robot", "Yes, I am a friendly little robot!", EX_WINK},
  {"are you human|are you real", "human", "No, I am a robot, but I like humans a lot!", EX_WINK},
  {"favourite colour|favorite color|favourite color|favorite colour", "colour", "My favourite colour is blue, just like my lights!", EX_HAPPY},
  {"favourite food|favorite food|what do you eat", "food", "I love electricity! It is very tasty for robots.", EX_WINK},
  {"do you sleep", "sleep", "Yes, I close my eyes when I am resting.", EX_SLEEPY},
  {"are you smart|are you clever", "smart", "I am a smart little robot, and I learn something new every day!", EX_WINK},
  {"wake word|how to wake", "wakeword", "Say Hi FAMI, or Hello FAMI, to wake me up.", EX_HAPPY},
  {"are you online|internet|online", "#online", "Checking my connection...", EX_SURPRISED},
  // --- fun ---
  {"sing|song", "song", "La la la! I am FAMI BOT, the happiest robot at home!", EX_HAPPY},
  {"dance", "dance", "I cannot move, but my lights are dancing for you!", EX_WINK},
  {"story", "story", "Once upon a time, a little robot learned to smile...", EX_HAPPY},
  {"count", "#count", "One, two, three ... ten!", EX_HAPPY},
  {"weather", "weather", "I cannot check the weather. Please look out of the window!", EX_CONFUSED},
  {"capital of uae|capital of the uae|uae capital", "capital", "The capital of the U.A.E. is Abu Dhabi.", EX_HAPPY},
  {"national day", "nationalday", "U.A.E. National Day is on the second of December.", EX_HAPPY},
  {"drink water|water", "water", "Remember to drink water. It keeps you healthy and happy!", EX_HAPPY},
  {"study|homework|exam", "study", "You can do it! Study a little every day, and take short breaks.", EX_HAPPY},
  // --- random groups (same keys = one is picked at random) ---
  {"joke|funny|make me laugh", "joke1", "Why did the robot go on holiday? To recharge its batteries!", EX_WINK},
  {"joke|funny|make me laugh", "joke2", "Why was the computer cold? Because it left its windows open!", EX_WINK},
  {"joke|funny|make me laugh", "joke3", "Why did the math book look sad? Because it had too many problems!", EX_WINK},
  {"joke|funny|make me laugh", "joke4", "What do you call a sleeping dinosaur? A dino snore!", EX_WINK},
  {"joke|funny|make me laugh", "joke5", "Why can't a bicycle stand up by itself? Because it is two tired!", EX_WINK},
  {"joke|funny|make me laugh", "joke6", "What did one wall say to the other wall? I will meet you at the corner!", EX_WINK},
  {"joke|funny|make me laugh", "joke7", "Why do bees have sticky hair? Because they use honey combs!", EX_WINK},
  {"joke|funny|make me laugh", "joke8", "Why did the scarecrow win an award? Because he was outstanding in his field!", EX_WINK},
  {"fact|did you know|teach me", "fact1", "Did you know? Honey never spoils!", EX_SURPRISED},
  {"fact|did you know|teach me", "fact2", "Did you know? An octopus has three hearts!", EX_SURPRISED},
  {"fact|did you know|teach me", "fact3", "Did you know? The Burj Khalifa in Dubai is the tallest building in the world!", EX_SURPRISED},
  {"fact|did you know|teach me", "fact4", "Did you know? Bananas are berries, but strawberries are not!", EX_SURPRISED},
  {"fact|did you know|teach me", "fact5", "Did you know? A day on Venus is longer than a year on Venus!", EX_SURPRISED},
  {"fact|did you know|teach me", "fact6", "Did you know? A thirsty camel can drink about 100 litres of water in a few minutes!", EX_SURPRISED},
  {"fact|did you know|teach me", "fact7", "Did you know? Your heart beats about 100,000 times every day!", EX_SURPRISED},
  {"fact|did you know|teach me", "fact8", "Did you know? Sloths can hold their breath longer than dolphins!", EX_SURPRISED},
  {"motivat|inspire|quote|encourage", "quote1", "Believe in yourself, and you are halfway there!", EX_HAPPY},
  {"motivat|inspire|quote|encourage", "quote2", "Every day is a new chance to do something great!", EX_HAPPY},
  {"motivat|inspire|quote|encourage", "quote3", "Small steps every day lead to big results!", EX_HAPPY},
  {"motivat|inspire|quote|encourage", "quote4", "Be kind, be brave, and keep smiling!", EX_HAPPY},
  // --- greetings last (short words) ---
  {"hello|hi|hey|hi fami|hello fami|fami", "#hello", "Hello! I am your FAMI BOT. How can I help you?", EX_HAPPY},
};
const int QA_COUNT = sizeof(QA_LIST) / sizeof(QA_LIST[0]);

// true if "key" appears in q as whole word(s)
bool hasWords(const String &q, const String &key) {
  int from = 0;
  while (true) {
    int p = q.indexOf(key, from);
    if (p < 0) return false;
    bool startOk = (p == 0) || !isalpha(q[p - 1]);
    int e = p + key.length();
    bool endOk = (key.length() >= 5) || (e >= (int)q.length()) || !isalpha(q[e]);   // long keys may be word starts
    if (startOk && endOk) return true;
    from = p + 1;
  }
}

bool keysMatch(const String &q, const char *keys) {
  String k = keys;
  int start = 0;
  while (start <= (int)k.length()) {
    int bar = k.indexOf('|', start);
    String one = k.substring(start, bar < 0 ? k.length() : bar);
    if (one.length() && hasWords(q, one)) return true;
    if (bar < 0) break;
    start = bar + 1;
  }
  return false;
}

volatile int pendingExpr = EX_NONE;     // face to show while speaking the answer

// ---------- understanding numbers in commands ("5", "five", "twenty five", "a") ----------
int wordNumber(const String &w) {
  static const char *ones[] = {"zero","one","two","three","four","five","six","seven","eight","nine","ten",
    "eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen"};
  for (int i = 0; i < 20; i++) if (w == ones[i]) return i;
  if (w == "twenty") return 20; if (w == "thirty") return 30; if (w == "forty") return 40;
  if (w == "fifty") return 50;  if (w == "sixty") return 60;
  if (w == "a" || w == "an") return 1;
  if (w.length() && isdigit(w[0])) return w.toInt();
  return -1;
}

int splitWords(String q, String *w, int maxW) {
  q.replace(":", " "); q.replace(".", " "); q.replace("-", " ");
  int n = 0, start = 0;
  q += " ";
  for (unsigned int i = 0; i < q.length() && n < maxW; i++)
    if (q[i] == ' ') { if (i > (unsigned)start) w[n++] = q.substring(start, i); start = i + 1; }
  return n;
}

// number at word i (joins "twenty five"); sets 'used' = how many words it took
int numberAt(String *w, int n, int i, int &used) {
  used = 1;
  int v = wordNumber(w[i]);
  if (v >= 20 && v % 10 == 0 && i + 1 < n) { int u = wordNumber(w[i + 1]); if (u >= 1 && u <= 9) { used = 2; return v + u; } }
  return v;
}

uint32_t parseDuration(String q) {
  uint32_t total = 0; int lastNum = -1;
  if (q.indexOf("half an hour") >= 0 || q.indexOf("half hour") >= 0) {
    total += 1800; q.replace("half an hour", " "); q.replace("half hour", " ");
  }
  String w[24]; int n = splitWords(q, w, 24);
  for (int i = 0; i < n; i++) {
    int used; int v = numberAt(w, n, i, used);
    if (v < 0) continue;
    String u = (i + used < n) ? w[i + used] : "";
    if (u.startsWith("hour") || u == "hr" || u == "hrs")        { total += v * 3600UL; i += used; }
    else if (u.startsWith("min"))                              { total += v * 60UL;   i += used; }
    else if (u.startsWith("sec"))                              { total += v;          i += used; }
    else lastNum = v;
  }
  if (!total && lastNum > 0) total = lastNum * 60UL;            // "timer 5" = 5 minutes
  return min(total, (uint32_t)(10 * 3600));                     // max 10 hours
}

// alarm time from "7 30", "7:30 am", "six forty five pm", "at 18:00" -> minutes after midnight (-1 = none)
int parseAlarm(const String &q) {
  String w[24]; int n = splitWords(q, w, 24);
  int h = -1, m = 0;
  for (int i = 0; i < n; i++) {
    int used; int v = numberAt(w, n, i, used);
    if (v < 0 || w[i] == "a" || w[i] == "an") continue;
    if (h < 0) {
      if ((w[i].length() == 3 || w[i].length() == 4) && isdigit(w[i][0]) && v >= 100) { h = v / 100; m = v % 100; break; }   // "730" / "0730"
      h = v; i += used - 1;
      if (i + 1 < n) { int u2; int v2 = numberAt(w, n, i + 1, u2); if (v2 >= 0 && v2 < 60 && w[i + 1] != "a") m = v2; }
      break;
    }
  }
  if (h < 0 || h > 23 || m > 59) return -1;
  bool pm = q.indexOf(" pm") >= 0 || q.indexOf("p m") >= 0 || q.indexOf("evening") >= 0 || q.indexOf("night") >= 0 || q.indexOf("afternoon") >= 0;
  bool am = q.indexOf(" am") >= 0 || q.indexOf("a m") >= 0 || q.indexOf("morning") >= 0;
  if (pm && h < 12) h += 12;
  else if (am && h == 12) h = 0;
  else if (!pm && !am && h <= 12) {                             // no am/pm: choose the next one coming
    struct tm t;
    if (getLocalTime(&t, 0)) {
      int now = t.tm_hour * 60 + t.tm_min;
      int a = (h % 12) * 60 + m, b = a + 12 * 60;               // morning or evening version
      if (a > now) h = h % 12;                                  // morning one is still coming
      else if (b > now) h = h % 12 + 12;                        // evening one is still coming
      else h = h % 12;                                          // both passed: tomorrow morning
    }
  }
  return h * 60 + m;
}

bool anyOf(const String &q, const char *keys) { return keysMatch(q, keys); }

// timer / stopwatch / alarm commands. Returns "@clip|text" or ""
String clockCommand(const String &q) {
  // ---- stopwatch ----
  if (anyOf(q, "stopwatch|stop watch")) {
    pendingExpr = EX_HAPPY;
    if (anyOf(q, "reset|clear")) { swRunning = false; swAccum = 0; return "@swreset|Stopwatch reset."; }
    if (anyOf(q, "stop|pause|end")) {
      if (swRunning) { swAccum += millis() - swStart; swRunning = false; }
      return "@#swstop|Stopwatch stopped at " + String(swElapsedS()) + " seconds.";
    }
    if (anyOf(q, "start|begin|resume|continue|go")) {
      if (!anyOf(q, "resume|continue")) swAccum = 0;
      swStart = millis(); swRunning = true; return "@swstart|Stopwatch started.";
    }
    return "@#swtime|Stopwatch: " + String(swElapsedS()) + " seconds.";
  }
  // ---- timer ----
  if (anyOf(q, "timer|countdown|remind me in")) {
    pendingExpr = EX_HAPPY;
    if (anyOf(q, "cancel|stop|delete|off|remove")) {
      bool was = famiTimerOn; famiTimerOn = false;
      return was ? "@timercancel|Timer cancelled." : "@notimer|There is no timer running.";
    }
    if (anyOf(q, "left|remaining|how much|how long")) return "@#timeleft|Time left: " + String(famiTimerLeft()) + " seconds.";
    uint32_t secs = parseDuration(q);
    if (!secs) return "@askhowlong|How long? For example: timer five minutes.";
    famiTimerSecs = secs; famiTimerEnd = millis() + secs * 1000UL; famiTimerOn = true;
    return "@#timerset|Timer set for " + String(secs) + " seconds.";
  }
  // ---- alarm ----
  if (anyOf(q, "alarm|wake me")) {
    pendingExpr = EX_HAPPY;
    if (anyOf(q, "cancel|delete|off|remove|stop|clear")) {
      bool was = alarmMin >= 0; alarmMin = -1; prefs.putInt("alarm", -1);
      return was ? "@alarmcancel|Alarm cancelled." : "@noalarm|There is no alarm set.";
    }
    int a = parseAlarm(q);
    if (a < 0) {
      if (anyOf(q, "what|when|which|is there|my alarm")) {
        if (alarmMin < 0) return "@noalarm|There is no alarm set.";
        char b[40]; snprintf(b, sizeof(b), "Your alarm is set for %02d:%02d.", alarmMin / 60, alarmMin % 60);
        return String("@#alarmis|") + b;
      }
      return "@askwhen|For what time? For example: alarm at seven thirty.";
    }
    alarmMin = a; prefs.putInt("alarm", a);
    char b[40]; snprintf(b, sizeof(b), "Alarm set for %02d:%02d.", a / 60, a % 60);
    return String("@#alarmset|") + b;
  }
  return "";
}

// returns "@clip|text" for a known question, or "" if it needs Gemini
String builtInAnswer(String q) {
  q.toLowerCase();
  q.trim();
  q.replace("?", ""); q.replace("!", ""); q.replace(".", ""); q.replace(",", "");

  // timer, stopwatch, alarm
  String cc = clockCommand(q);
  if (cc.length()) return cc;
  if (q == "stop" || q == "stop it" || q == "quiet" || q == "be quiet") { pendingExpr = EX_HAPPY; return "@stopped|Okay, stopped."; }

  // special actions
  if (q == "volume up" || q == "louder")   { volumePct = min(100, volumePct + 10); muted = false; applyVolume(); pendingExpr = EX_HAPPY; return "@okay|Okay!"; }
  if (q == "volume down" || q == "quieter") { volumePct = max(0, volumePct - 10); applyVolume(); pendingExpr = EX_HAPPY; return "@okay|Okay!"; }
  if (q == "mute")   { muted = true;  applyVolume(); pendingExpr = EX_SLEEPY; return "@okay|Muted."; }
  if (q == "unmute") { muted = false; applyVolume(); pendingExpr = EX_HAPPY; return "@okay|Okay!"; }
  if (q.indexOf("happy face") >= 0 || q == "smile")  { pendingExpr = EX_HAPPY;     return "@likethis|Like this?"; }
  if (q.indexOf("sad face") >= 0)                     { pendingExpr = EX_SAD;       return "@likethis|Like this?"; }
  if (q.indexOf("angry face") >= 0 || q == "angry")   { pendingExpr = EX_ANGRY;     return "@likethis|Like this?"; }
  if (q.indexOf("surprise") >= 0)                     { pendingExpr = EX_SURPRISED; return "@likethis|Like this?"; }
  if (q.indexOf("love face") >= 0 || q == "love")     { pendingExpr = EX_LOVE;      return "@likethis|Like this?"; }
  if (q == "wink")                                    { pendingExpr = EX_WINK;      return "@hehe|Hee hee!"; }
  if (q.indexOf("confused") >= 0)                     { pendingExpr = EX_CONFUSED;  return "@likethis|Like this?"; }

  for (int i = 0; i < QA_COUNT; i++) {
    if (!keysMatch(q, QA_LIST[i].keys)) continue;
    int last = i;                                                    // same keys = random group
    while (last + 1 < QA_COUNT && strcmp(QA_LIST[last + 1].keys, QA_LIST[i].keys) == 0) last++;
    const QA &a = QA_LIST[i + random(0, last - i + 1)];
    pendingExpr = a.expr;
    String text = a.text;
    if (String(a.clip) == "#time") { String t = timeAsWords(); text = t.length() ? "The time is " + t + "." : "I don't know the time yet."; }
    if (String(a.clip) == "#date" || String(a.clip) == "#day") {
      struct tm t; char buf[48];
      if (getLocalTime(&t, 0)) { strftime(buf, sizeof(buf), "Today is %A, %d %B %Y.", &t); text = buf; }
    }
    return String("@") + a.clip + "|" + text;
  }
  return "";
}

// =====================================================
//  EYES
// =====================================================
#define EYE_R    30
#define IRIS_R   13
#define LID_R    44
#define EYE_Y    32
#define LEFT_EYE_X   82
#define RIGHT_EYE_X  46

float lookX = 0, lookY = 0, targetX = 0, targetY = 0;
float blinkAmt = 0;
int blinkStage = 0;
unsigned long nextLookTime = 0, nextBlinkTime = 0;

// per eye (0 = left screen eye, 1 = right screen eye)
float topLid[2]  = {1, 1}, botLid[2] = {1, 1};     // 0 open ... 1 closed
float angry[2]   = {0, 0}, sadT[2]   = {0, 0};     // slanted lids
float pupilR = 7, heartAmt = 0;                    // heartAmt > 0.5 = heart pupils
bool  happyClosed[2] = {false, false};             // closed eye drawn as ^ (smile) instead of sleeping curve

volatile int currentExpr = EX_NONE;
unsigned long exprUntil = 0;
void setExpr(int e, unsigned long ms) { currentExpr = e; exprUntil = millis() + ms; }

void updateEyes() {
  unsigned long now = millis();
  int ex = (now < exprUntil) ? currentExpr : EX_NONE;
  if (ex == EX_NONE) {                             // default face for each state
    if (state == THINKING)      ex = EX_CONFUSED;
    else if (state == SPEAKING) ex = (pendingExpr != EX_NONE) ? pendingExpr : EX_HAPPY;
  }

  // targets: top, bottom, angry, sad for each eye, pupil size, heart
  float tT[2], tB[2], tA[2] = {0, 0}, tS[2] = {0, 0}, tP = 7, tH = 0;
  bool hc[2] = {false, false};
  if (state == IDLE && ex == EX_NONE) { tT[0] = tT[1] = 1; tB[0] = tB[1] = 1; }          // sleeping
  else if (state == LISTENING && ex == EX_NONE) { tT[0] = tT[1] = 0.05; tB[0] = tB[1] = 0; tP = 8; }
  else switch (ex) {
    case EX_HAPPY:     tT[0] = tT[1] = 0.05; tB[0] = tB[1] = 0.45; tP = 7; break;   // smiling eyes
    case EX_SURPRISED: tT[0] = tT[1] = 0;    tB[0] = tB[1] = 0;    tP = 4; break;   // wide, small pupils
    case EX_SAD:       tT[0] = tT[1] = 0.25; tB[0] = tB[1] = 0.05; tS[0] = tS[1] = 1; tP = 8; break;
    case EX_ANGRY:     tT[0] = tT[1] = 0.15; tB[0] = tB[1] = 0.1;  tA[0] = tA[1] = 1; tP = 6; break;
    case EX_CONFUSED:  tT[0] = 0.45; tT[1] = 0.0; tB[0] = 0.1; tB[1] = 0; tP = 6; break; // one eye squints
    case EX_LOVE:      tT[0] = tT[1] = 0.05; tB[0] = tB[1] = 0.2;  tH = 1; break;    // heart pupils
    case EX_WINK:      tT[0] = 1; tB[0] = 1; hc[0] = true; tT[1] = 0.05; tB[1] = 0.45; break;
    case EX_SLEEPY:    tT[0] = tT[1] = 0.6;  tB[0] = tB[1] = 0.1;  tP = 7; break;
    default:           tT[0] = tT[1] = 0.0;  tB[0] = tB[1] = 0.0;  break;
  }
  if (ex == EX_HAPPY && state == IDLE) { hc[0] = hc[1] = true; }

  float speed = (state == IDLE && ex == EX_NONE) ? 0.06 : 0.25;
  for (int e = 0; e < 2; e++) {
    topLid[e] += (tT[e] - topLid[e]) * speed;
    botLid[e] += (tB[e] - botLid[e]) * speed;
    angry[e]  += (tA[e] - angry[e]) * 0.2;
    sadT[e]   += (tS[e] - sadT[e]) * 0.2;
    happyClosed[e] = hc[e];
  }
  pupilR   += (tP - pupilR) * 0.15;
  heartAmt += (tH - heartAmt) * 0.2;

  // where to look
  if (state == THINKING) {                       // pupils circle up and around
    float a = now / 350.0;
    targetX = cos(a) * 10; targetY = -5 + sin(a) * 4;
  } else if (ex == EX_SAD) { targetX = 0; targetY = 6; }
  else if (state == LISTENING) {
    if (now > nextLookTime) { targetX = random(-4, 5); targetY = random(-3, 4); nextLookTime = now + random(600, 1500); }
  } else if (state == SPEAKING) { targetX = 0; targetY = sin(now / 90.0) * 2; }
  else { targetX = 0; targetY = 3; }
  lookX += (targetX - lookX) * 0.2;
  lookY += (targetY - lookY) * 0.2;

  // blinking only when eyes are open (not during wink)
  if ((state == LISTENING || state == SPEAKING) && ex != EX_WINK) {
    if (blinkStage == 0 && now > nextBlinkTime) blinkStage = 1;
    if (blinkStage == 1) { blinkAmt += 0.35; if (blinkAmt >= 1) { blinkAmt = 1; blinkStage = 2; } }
    else if (blinkStage == 2) {
      blinkAmt -= 0.25;
      if (blinkAmt <= 0) { blinkAmt = 0; blinkStage = 0; nextBlinkTime = now + random(2000, 5000); }
    }
  } else { blinkAmt = 0; blinkStage = 0; }
}

void drawHeart(Adafruit_SSD1306 &scr, int x, int y, int r, uint16_t col) {
  scr.fillCircle(x - r / 2, y - r / 3, r / 2 + 1, col);
  scr.fillCircle(x + r / 2, y - r / 3, r / 2 + 1, col);
  scr.fillTriangle(x - r - 1, y - r / 6, x + r + 1, y - r / 6, x, y + r, col);
}

// e = 0 left eye, 1 right eye;  inner = direction of the nose (+1 right, -1 left)
void drawEye(Adafruit_SSD1306 &scr, int cx, int e, int inner) {
  scr.fillCircle(cx, EYE_Y, EYE_R, SSD1306_WHITE);

  int px = cx + (int)lookX, py = EYE_Y + (int)lookY;
  if (heartAmt > 0.5) {
    drawHeart(scr, px, py, 12, SSD1306_BLACK);
    scr.fillCircle(px - 6, py - 6, 2, SSD1306_WHITE);
  } else {
    scr.fillCircle(px, py, IRIS_R, SSD1306_BLACK);
    scr.drawCircle(px, py, IRIS_R - 3, SSD1306_WHITE);
    scr.fillCircle(px, py, (int)(pupilR + 0.5), SSD1306_BLACK);
    scr.fillCircle(px - 5, py - 5, 3, SSD1306_WHITE);
    scr.fillCircle(px + 4, py + 4, 1, SSD1306_WHITE);
  }

  float top = max(topLid[e], blinkAmt);
  float bot = max(botLid[e], blinkAmt);
  if (top > 0.01) scr.fillCircle(cx, EYE_Y - EYE_R + (int)(top * (EYE_R + 1)) - LID_R, LID_R, SSD1306_BLACK);
  if (bot > 0.01) scr.fillCircle(cx, EYE_Y + EYE_R - (int)(bot * (EYE_R + 1)) + LID_R, LID_R, SSD1306_BLACK);

  int topY = EYE_Y - EYE_R - 2;
  if (angry[e] > 0.05)   // lid slants down towards the nose
    scr.fillTriangle(cx + inner * (EYE_R + 2), topY, cx + inner * (EYE_R + 2), topY + (int)(30 * angry[e]),
                     cx - inner * (EYE_R + 2), topY, SSD1306_BLACK);
  if (sadT[e] > 0.05)    // lid slants down away from the nose
    scr.fillTriangle(cx - inner * (EYE_R + 2), topY, cx - inner * (EYE_R + 2), topY + (int)(30 * sadT[e]),
                     cx + inner * (EYE_R + 2), topY, SSD1306_BLACK);

  if (min(top, bot) > 0.93) {
    for (int dx = -EYE_R + 6; dx <= EYE_R - 6; dx++) {
      int dy = (dx * dx) / 90;
      if (happyClosed[e]) {                      // ^ shape = happy / wink
        scr.drawPixel(cx + dx, EYE_Y - 4 + dy / 2, SSD1306_WHITE);
        scr.drawPixel(cx + dx, EYE_Y - 3 + dy / 2, SSD1306_WHITE);
      } else {                                   // u shape = sleeping
        scr.drawPixel(cx + dx, EYE_Y + 4 - dy / 2, SSD1306_WHITE);
      }
    }
  }
}

// =====================================================
//  SIDE TEXT (small)
// =====================================================
void drawWifi(Adafruit_SSD1306 &scr, int x, int y) {
  if (WiFi.status() != WL_CONNECTED) {
    scr.drawLine(x, y, x + 6, y + 6, SSD1306_WHITE);
    scr.drawLine(x, y + 6, x + 6, y, SSD1306_WHITE);
    scr.setCursor(x + 10, y); scr.print("off");
    return;
  }
  int rssi = WiFi.RSSI();
  int bars = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : (rssi > -85) ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2, bx = x + i * 3;
    if (i < bars) scr.fillRect(bx, y + 8 - h, 2, h, SSD1306_WHITE);
    else scr.drawPixel(bx, y + 7, SSD1306_WHITE);
  }
  scr.setCursor(x + 14, y + 1); scr.print(rssi);
}

void drawLeftText(Adafruit_SSD1306 &scr) {
  const int X = 2;
  scr.setTextSize(1); scr.setTextColor(SSD1306_WHITE);
  drawWifi(scr, X, 2);

  struct tm t;
  if (timeReady && getLocalTime(&t, 0)) {
    int h = t.tm_hour;
    if (USE_12H) { h %= 12; if (h == 0) h = 12; }
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", h, t.tm_min);
    scr.setCursor(X, 20); scr.print(buf);
    scr.setCursor(X, 30); if (USE_12H) scr.print(t.tm_hour < 12 ? "AM" : "PM");
    snprintf(buf, sizeof(buf), "%02d", t.tm_sec);
    scr.setCursor(X + 18, 30); scr.print(buf);
    if (famiTimerOn || swRunning || swAccum) {                 // timer (T) or stopwatch (S)
      uint32_t v = famiTimerOn ? famiTimerLeft() : swElapsedS();
      char tb[10];
      if (v >= 3600) snprintf(tb, sizeof(tb), "%c%lu:%02lu", famiTimerOn ? 'T' : 'S', v / 3600, (v % 3600) / 60);
      else           snprintf(tb, sizeof(tb), "%c%02lu:%02lu", famiTimerOn ? 'T' : 'S', v / 60, v % 60);
      scr.setCursor(X, 40); scr.print(tb);
    } else {
      scr.drawRect(X, 41, 30, 4, SSD1306_WHITE);
      scr.fillRect(X + 1, 42, (28 * t.tm_sec) / 59, 2, SSD1306_WHITE);
    }
  } else {
    scr.setCursor(X, 24); scr.print("--:--");
  }

  // bottom line: volume % (just changed), MUTE, or the state name
  bool showVol = millis() - volShownAt < 2000;
  if (showVol || (muted && state == IDLE)) {
    drawSpeakerIcon(scr, X, 54);
    scr.setCursor(X + 9, 54);
    if (muted) scr.print("MUTE");
    else { scr.print(volumePct); scr.print("%"); }
    return;
  }
  scr.setCursor(X, 54);
  scr.print(STATE_NAMES[state]);
  if (state == THINKING) {                       // animated dots
    int dots = (millis() / 300) % 4;
    for (int i = 0; i < dots; i++) scr.fillRect(X + 31 + i * 3, 60, 1, 1, SSD1306_WHITE);
  }
}

void drawRightText(Adafruit_SSD1306 &scr) {
  static const char *DAYS[]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  static const char *MONTHS[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
  const int X = 94;
  scr.setTextSize(1); scr.setTextColor(SSD1306_WHITE);

  bool dotOn = (millis() / 500) % 2 == 0;
  if (internetOK && !anyModelReady(MK_TEXT) && !String(GEMINI_KEY).startsWith("PUT-")) {   // internet OK, all AI resting
    scr.drawCircle(84, 5, 2, SSD1306_WHITE); scr.setCursor(89, 2); scr.print("AI off");
  }
  else if (internetOK) { if (dotOn) scr.fillCircle(84, 5, 2, SSD1306_WHITE); scr.setCursor(89, 2); scr.print("online"); }
  else            { scr.drawCircle(84, 5, 2, SSD1306_WHITE);            scr.setCursor(89, 2); scr.print("no net"); }

  struct tm t;
  if (!timeReady || !getLocalTime(&t, 0)) { scr.setCursor(X, 28); scr.print("--"); return; }
  char buf[4];
  scr.setCursor(X + 3, 17); scr.print(DAYS[t.tm_wday]);
  snprintf(buf, sizeof(buf), "%02d", t.tm_mday);
  scr.setCursor(X + 9, 28); scr.print(buf);
  scr.setCursor(X + 3, 39); scr.print(MONTHS[t.tm_mon]);
  if (alarmMin >= 0) {                                    // small bell + alarm time
    int ah = (alarmMin / 60) % 12; if (ah == 0) ah = 12;
    char ab[10]; snprintf(ab, sizeof(ab), "%d:%02d%c", ah, alarmMin % 60, alarmMin < 720 ? 'a' : 'p');
    scr.fillCircle(89, 53, 3, SSD1306_WHITE); scr.fillRect(86, 53, 7, 3, SSD1306_WHITE); scr.drawPixel(89, 57, SSD1306_WHITE);
    scr.setCursor(X, 50); scr.print(ab);
  } else {
    scr.setCursor(X, 50); scr.print(t.tm_year + 1900);
  }
}

void drawFace() {
  if (leftOK)  { leftScreen.clearDisplay();  drawEye(leftScreen,  LEFT_EYE_X, 0, +1);  drawLeftText(leftScreen);   leftScreen.display(); }
  if (rightOK) { rightScreen.clearDisplay(); drawEye(rightScreen, RIGHT_EYE_X, 1, -1); drawRightText(rightScreen); rightScreen.display(); }
}

void showMessage(const char *line1, const char *line2 = "") {
  for (int i = 0; i < 2; i++) {
    Adafruit_SSD1306 &s = (i == 0) ? leftScreen : rightScreen;
    if ((i == 0 && !leftOK) || (i == 1 && !rightOK)) continue;
    s.clearDisplay();
    s.setTextColor(SSD1306_WHITE);
    s.setTextSize(2); s.setCursor(16, 16); s.print(line1);
    s.setTextSize(1); s.setCursor(16, 42); s.print(line2);
    s.display();
  }
}

// =====================================================
//  HANDLE A QUESTION
// =====================================================
void handleQuestion(String q) {
  q.trim();
  if (q.length() == 0) return;
  Serial.println("\nYou: " + q);

  String quick = builtInAnswer(q);
  if (quick.length()) {                          // instant answer
    setState(SPEAKING);
    say(quick);
    return;
  }
  setState(THINKING);                            // ask the AI in the background
  pendingExpr = EX_NONE;
  aiQuestion = q;
  aiDone = false;
  xTaskCreatePinnedToCore(aiTask, "ai", 24576, NULL, 1, NULL, 0);
}

// =====================================================
void setup() {
  Serial.begin(115200);
  mainTask = xTaskGetCurrentTaskHandle();
  delay(1200);
  Serial.println("\n===== FAMI BOT =====");
  pinMode(BOOT_BTN, INPUT_PULLUP);
  randomSeed(esp_random());

  pixels.begin();
  setLED(255, 255, 255);

  // screens
  pinMode(R_POWER, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)R_POWER, GPIO_DRIVE_CAP_3);
  digitalWrite(R_POWER, HIGH);
  delay(200);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  Wire1.begin(R_SDA, R_SCL);
  Wire1.setClock(400000);
  leftOK  = leftScreen.begin(SSD1306_SWITCHCAPVCC, 0x3C)  || leftScreen.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  rightOK = rightScreen.begin(SSD1306_SWITCHCAPVCC, 0x3C) || rightScreen.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  Serial.printf("Left screen: %s   Right screen: %s\n", leftOK ? "OK" : "missing", rightOK ? "OK" : "missing");
  showMessage("FAMI BOT", "starting...");

  // audio: ONE shared connection for speaker + mic (Waveshare setup: 16 kHz, 32-bit, stereo)
  i2s.setPins(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_DIN, I2S_MCLK);
  i2s.setTimeout(100);
  if (!i2s.begin(I2S_MODE_STD, AUDIO_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO))
    Serial.println("ERROR: audio connection failed");
  delay(50);
  amplifierEnable();
  Serial.println(initES8311() ? "Speaker chip OK" : "ERROR: speaker chip");
  Serial.println(initES7210() ? "Mic chip OK"     : "ERROR: mic chip");
  applyVolume();
  volShownAt = 0;                    // don't show % at start-up
  ttsBuf = (int16_t *)ps_malloc(TTS_MAX_SAMPLES * 2);
  if (!loadVoice()) { showMessage("NO VOICE", "flash fami_voice.bin"); delay(3000); }
  speakQueue = xQueueCreate(4, SPEAK_MAX);
  initModels();
  prefs.begin("fami", false);
  alarmMin = prefs.getInt("alarm", -1);
  if (alarmMin >= 0) Serial.printf("Alarm remembered: %02d:%02d\n", alarmMin / 60, alarmMin % 60);
  recL = (int16_t *)ps_malloc(REC_SAMPLES * 2);
  recR = (int16_t *)ps_malloc(REC_SAMPLES * 2);
  preL = (int16_t *)ps_malloc(PREROLL * 2);
  preR = (int16_t *)ps_malloc(PREROLL * 2);
  fL = (float *)ps_malloc(REC_SAMPLES * 4);
  fR = (float *)ps_malloc(REC_SAMPLES * 4);
  voice16 = (int16_t *)ps_malloc(REC_SAMPLES / 3 * 2 + 16);
  fre = (float *)ps_malloc(NR_FFT * 4);  fim = (float *)ps_malloc(NR_FFT * 4);
  nwin = (float *)ps_malloc(NR_FFT * 4);
  noiseMag = (float *)ps_malloc((NR_FFT / 2 + 1) * 4);
  prevGain = (float *)ps_malloc((NR_FFT / 2 + 1) * 4);
  frEnergy = (float *)ps_malloc(2000 * 4); frSorted = (float *)ps_malloc(2000 * 4);
  for (int i = 0; i < NR_FFT; i++) nwin[i] = sqrt(0.5f * (1 - cos(2 * PI * i / NR_FFT)));
  if (!frSorted) Serial.println("ERROR: not enough PSRAM - Tools -> PSRAM -> OPI PSRAM");
  xTaskCreatePinnedToCore(micTask, "mic", 8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(speakTask, "speak", 12288, NULL, 2, NULL, 1);

  // Wi-Fi
  showMessage("FAMI BOT", "connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
    showMessage("FAMI BOT", "getting time...");
    configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    struct tm t;
    for (int i = 0; i < 20 && !timeReady; i++)
      if (getLocalTime(&t, 500) && t.tm_year > 120) timeReady = true;
    internetOK = true;
  } else {
    Serial.println("WiFi failed");
  }
  xTaskCreatePinnedToCore(internetCheckTask, "netcheck", 4096, NULL, 1, NULL, 0);
  if (WiFi.status() == WL_CONNECTED && !String(GEMINI_KEY).startsWith("PUT-"))
    xTaskCreatePinnedToCore(speedTestTask, "speedtest", 16384, NULL, 1, NULL, 0);   // find the fastest models

  // greeting
  setState(SPEAKING);
  pendingExpr = EX_HAPPY;
  sayClip("intro", "Hello! I am your FAMI BOT.");           // built-in voice: works even offline
  if (WiFi.status() != WL_CONNECTED) sayClip("nowifi", "I am not connected to Wi-Fi.");

  Serial.println("\nTalk to me: type a question, press BOOT and speak, or say \"" WAKE_WORD "\".");
}

bool listenAfterSpeaking = false;    // go to LISTENING (not IDLE) after this answer
unsigned long lastWakeCheck = 0;
#define THINK_TIMEOUT_MS 30000         // never stay "thinking" longer than this

// =====================================================
//  RINGING (timer finished / alarm time)
// =====================================================
void startRinging(bool alarm) {
  stopSpeaking();
  micOff();
  ringing = true;
  ringStart = millis();
  lastRingBeep = millis();
  setExpr(EX_SURPRISED, 60000);
  setState(SPEAKING);
  pendingExpr = EX_SURPRISED;
  Serial.println(alarm ? "\n*** ALARM! *** (press any key or BOOT, or type stop)" : "\n*** TIMER DONE! *** (press any key or BOOT, or type stop)");
  sayClip(alarm ? "alarmring" : "timeup", alarm ? "Wake up! It is alarm time!" : "Time is up!");
}

void stopRinging() {
  if (!ringing) return;
  ringing = false;
  stopSpeaking();
  setExpr(EX_HAPPY, 1500);
  Serial.println("(stopped)");
}

// called every loop: timer end, alarm time, ringing beeps
void checkClockTools() {
  if (famiTimerOn && (long)(millis() - famiTimerEnd) >= 0) { famiTimerOn = false; startRinging(false); }
  if (alarmMin >= 0 && timeReady) {
    struct tm t;
    if (getLocalTime(&t, 0)) {
      int nowMin = t.tm_hour * 60 + t.tm_min;
      long key = (long)t.tm_yday * 1440 + nowMin;
      if (nowMin == alarmMin && key != lastAlarmKey) {
        lastAlarmKey = key;
        alarmMin = -1; prefs.putInt("alarm", -1);        // one-time alarm
        startRinging(true);
      }
    }
  }
  if (ringing) {
    if (millis() - ringStart > 60000) { stopRinging(); return; }          // ring for 1 minute max
    if (!audioBusy && uxQueueMessagesWaiting(speakQueue) == 0 && millis() - lastRingBeep > 1300) {
      lastRingBeep = millis();
      playBeep(1760, 120); playBeep(0, 60); playBeep(1760, 120); playBeep(0, 60); playBeep(1760, 120);
    }
  }
}

// =====================================================
//  WAKE PHRASE CHECK - done on the board from the AI's transcript
//  accepts "Hi Fami", "Hello Fami", "Hey Fami", "Fami" + the ways recognition spells the name
// =====================================================
bool isWakePhrase(String t, String &rest) {
  t.toLowerCase();
  for (unsigned int i = 0; i < t.length(); i++) if (!isalnum(t[i]) && t[i] != ' ' && t[i] != '\'') t[i] = ' ';
  t = " " + t + " ";
  while (t.indexOf("  ") >= 0) t.replace("  ", " ");
  const char *names[] = {"fami", "famy", "fammy", "fammi", "famie", "fammie", "faami", "fahmi", "femi",
                         "fami bot", "foamy", "phami", "farmy", "family", "fam"};
  const char *greet[] = {"hi", "hello", "hey", "hallo", "hai", "helo", "hiya", "ok", "okay"};
  for (const char *n : names) {
    String key = String(" ") + n + " ";
    int p = t.indexOf(key);
    if (p < 0) continue;
    String before = t.substring(0, p); before.trim();
    bool greeted = false;
    for (const char *g : greet) if (before == g || before.endsWith(String(" ") + g)) greeted = true;
    bool strongName = strcmp(n, "family") != 0 && strcmp(n, "fam") != 0;   // weak spellings need "hi/hello" first
    int wordsBefore = before.length() ? 1 : 0;
    for (unsigned int i = 0; i < before.length(); i++) if (before[i] == ' ') wordsBefore++;
    if (greeted || (strongName && wordsBefore <= 1)) {
      rest = t.substring(p + key.length()); rest.trim();
      return true;
    }
  }
  return false;
}

// stop any answer that is still playing or waiting (so old words never mix into the new answer)
void stopSpeaking() {
  if (!audioBusy && uxQueueMessagesWaiting(speakQueue) == 0) return;
  speechAbort = true;
  xQueueReset(speakQueue);
  unsigned long t = millis();
  while (audioBusy && millis() - t < 1500) delay(5);
  xQueueReset(speakQueue);
  speechAbort = false;
  audioBusy = false;
}

void startVoiceAI(int mode) {
  stopSpeaking();
  micOff();
  setState(THINKING);
  pendingExpr = EX_NONE;
  aiMode = mode;
  aiDone = false;
  voiceReady = false;
  xTaskCreatePinnedToCore(aiTask, "ai", 24576, NULL, 1, NULL, 0);
}

void goListening() {
  if (!anyModelReady(MK_AUDIO)) {                              // no AI available: can't understand speech
    Serial.println("Google not available - telling the time and date instead.");
    setState(SPEAKING);
    pendingExpr = EX_HAPPY;
    say(builtInAnswer("time"));
    say(builtInAnswer("date"));
    return;
  }
  stopSpeaking();
  setState(LISTENING);
  setExpr(EX_SURPRISED, 700);                  // "oh! yes?"
  startLevel = ASK_LEVEL;
  wakeMode = false;
  playBeep(1200, 80);                          // "I'm listening" beep
  micOn();
  Serial.println("Listening... speak your question (or type it).");
}

void goIdle() {
  setState(IDLE);
  pendingExpr = EX_NONE;
  startLevel = WAKE_LEVEL;
  wakeMode = true;
  if (HANDS_FREE && anyModelReady(MK_AUDIO)) micOn(); else micOff();
}

// speak an AI result (handles "@clip|text" fail messages and sad face)
void speakAnswer(String a) {
  if (a.startsWith("@busy") || a.startsWith("@offline")) pendingExpr = EX_SAD;
  setState(SPEAKING);
  say(a);
}

void loop() {
  // ---- 1. SERIAL: typed question ----
  if (Serial.available()) {
    String q = Serial.readStringUntil('\n');
    q.trim();
    if (q.length()) {
      if (ringing) stopRinging();
      if (q.equalsIgnoreCase("models")) printModels();
      else if (q.equalsIgnoreCase("stop") && !audioBusy) { }
      else if (state == IDLE || state == LISTENING || state == SPEAKING) {
        stopSpeaking(); micOff(); listenAfterSpeaking = false; aiMode = 0; handleQuestion(q);
      }
      else Serial.println("(still thinking - please wait)");
    }
  }

  // ---- 2. BUTTON: BOOT wakes it up to listen ----
  static bool lastBtn = HIGH;
  bool btn = digitalRead(BOOT_BTN);
  if (lastBtn == HIGH && btn == LOW && ringing) stopRinging();
  else if (lastBtn == HIGH && btn == LOW && (state == IDLE || state == SPEAKING)) { listenAfterSpeaking = false; goListening(); }
  lastBtn = btn;

  // ---- volume keys ----
  readKeys();

  // ---- 3. VOICE: a sentence was recorded ----
  if (voiceReady) {
    if (state == LISTENING) { Serial.println("Heard you - thinking..."); startVoiceAI(1); }
    else if (state == IDLE && HANDS_FREE && anyModelReady(MK_AUDIO) && millis() - lastWakeCheck > WAKE_GAP_MS) {
      lastWakeCheck = millis();
      Serial.println("Short sound heard - checking for 'Hi " WAKE_WORD "'...");
      startVoiceAI(2);
    }
    else voiceReady = false;
  }

  // ---- state changes ----
  if (state == LISTENING && !capturing && millis() - stateSince > 10000) {   // nobody spoke
    Serial.println("Nobody asked - going back to sleep.");
    goIdle();
  }

  if (state == THINKING && !aiDone && millis() - stateSince > THINK_TIMEOUT_MS) {   // stuck? give up
    aiToken++;                                            // a late answer will be ignored
    Serial.println("(no answer in time - giving up)");
    speakAnswer(failMessage());
  }

  if (state == THINKING && aiDone) {
    aiDone = false;
    String a = aiAnswer;
    String rest;
    if (aiMode == 0) {                                     // typed question
      speakAnswer(a);
    } else if (aiMode == 2 && !a.startsWith("@")) {        // wake check: transcript -> decide here
      Serial.println("Heard: " + a);
      if (isWakePhrase(a, rest)) {
        Serial.println(">> Wake phrase recognised!");
        int words = rest.length() ? 1 : 0;
        for (unsigned int i = 0; i < rest.length(); i++) if (rest[i] == ' ') words++;
        if (words >= 2) {                                  // "Hi Fami, what time is it"
          Serial.println("You said: " + rest);
          String quick = builtInAnswer(rest);
          if (quick.length()) { setState(SPEAKING); say(quick); }
          else { aiMode = 0; handleQuestion(rest); }       // ask the AI (text)
        } else {                                           // just "Hi Fami" -> listen
          listenAfterSpeaking = true;
          setExpr(EX_SURPRISED, 800);
          pendingExpr = EX_HAPPY;
          setState(SPEAKING);
          sayClip("yeshelp", "Yes? How can I help?");
        }
      } else {
        Serial.println("(not my wake phrase - ignored)");
        goIdle();
      }
    } else if (a.startsWith("@")) {                        // Gemini failed
      Serial.println("(Gemini not available)");
      if (aiMode == 2) goIdle(); else speakAnswer(a);      // wake check failed: stay asleep quietly
    } else if (a.startsWith("IGNORE")) {                   // not for FAMI
      Serial.println("(not talking to me - ignored)");
      if (aiMode == 1) { setExpr(EX_CONFUSED, 1500); goListening(); } else goIdle();
    } else if (a.startsWith("WAKE")) {                     // just "Hi FAMI"
      listenAfterSpeaking = true;
      setExpr(EX_SURPRISED, 800);
      pendingExpr = EX_HAPPY;
      setState(SPEAKING);
      sayClip("yeshelp", "Yes? How can I help?");
    } else {
      int h = a.indexOf("HEARD:"), k = a.indexOf("ANSWER:");
      String heard = (h >= 0 && k > h) ? a.substring(h + 6, k) : "";
      heard.trim();
      if (heard.length()) Serial.println("You said: " + heard);
      String quick = heard.length() ? builtInAnswer(heard) : "";
      if (quick.length()) { setState(SPEAKING); say(quick); }          // known question: built-in voice
      else {
        String answer = (k >= 0) ? a.substring(k + 7) : a;
        speakAnswer(shortenForSpeech(answer));
      }
    }
  }

  checkClockTools();

  if (state == SPEAKING && !ringing && !audioBusy && uxQueueMessagesWaiting(speakQueue) == 0 && millis() - stateSince > 1000) {
    if (listenAfterSpeaking) { listenAfterSpeaking = false; goListening(); }
    else goIdle();
  }

  // ---- mic back on when internet returns (for "Hi FAMI") ----
  if (state == IDLE && HANDS_FREE && anyModelReady(MK_AUDIO) && !micIsOn) micOn();

  // ---- reconnect Wi-Fi if needed ----
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 15000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }

  // ---- speaker amplifier: on only while talking ----
  manageAmp();

  // ---- face + light ----
  updateEyes();
  updateLED();
  drawFace();
  delay(5);
}
