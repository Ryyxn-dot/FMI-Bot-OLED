FAMI BOT - SETUP GUIDE
======================

FILES
  fami_bot\fami_bot.ino     the main program
  fami_bot\es8311.h         speaker chip driver
  fami_bot\es8311.cpp       speaker chip driver
  fami_bot\es8311_reg.h     speaker chip driver
  fami_voice.bin            offline voice (205 clips) - flashed ONCE with esptool (step 5)
  voice_preview.wav         listen on your PC to hear the new offline voice

  Do NOT put bsp_board.c, MIC_MSM.*, es7210.* or Audio.h files in the fami_bot folder.

1. BOARD PACKAGE: "esp32 by Espressif Systems" 3.x

2. LIBRARIES (Library Manager): Adafruit SSD1306, Adafruit GFX Library, Adafruit NeoPixel

3. TOOLS SETTINGS
   Board ................... ESP32S3 Dev Module
   USB CDC On Boot ......... Enabled
   USB Mode ................ Hardware CDC and JTAG
   Flash Size .............. 16MB (128Mb)
   Partition Scheme ........ 16M Flash (3MB APP/9.9MB FATFS)   <- needed for the voice
   PSRAM ................... OPI PSRAM
   Erase All Flash ......... DISABLED  (Enabled would erase the offline voice)

4. EDIT AT THE TOP OF fami_bot.ino
   ssid / password, GEMINI_KEY (never share it)

5. INSTALL THE OFFLINE VOICE (only once - it stays after normal uploads)
   a) Upload fami_bot.ino. The Serial Monitor shows:
        Offline voice NOT installed. Flash fami_voice.bin to address 0x610000
      (use the address it shows - normally 0x610000)
   b) Close the Serial Monitor. In CMD, go to the esptool folder:
        cd "%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\5.3.1"
   c) Flash the voice (change COM4 and the file path):
        esptool.exe --chip esp32s3 --port COM4 write-flash 0x610000 "E:\esp32_projects\fami_voice.bin"
   d) Press RESET. Serial Monitor: "Offline voice: 205 clips ready"

6. TALK TO FAMI BOT
   - VOICE : say "Hi FAMI" or "Hello FAMI" -> "Yes? How can I help?" -> ask
   - BUTTON: BOOT -> beep -> ask   (no internet: BOOT tells time + date)
   - TYPE  : any question in Serial Monitor (115200)
   - KEYS  : KEY1 volume down, KEY2 mute, KEY3 volume up

7. OFFLINE ANSWERS (no Gemini needed) - type or say them:
   time, date, what day, what month, what year, hello, good morning / afternoon /
   evening / night, bye, how are you, I am fine / sad / happy / bored / tired / hungry,
   I'm home, thank you, sorry, I love you, good job, are you my friend, salam, eid,
   ramadan, birthday, congratulations, your name, how old, who made you, where do you
   live, what can you do, are you a robot / human, favourite colour / food, do you sleep,
   are you smart, wake word, are you online, sing, dance, story, count, weather,
   capital of UAE, national day, water, study, joke (8), fact (8), motivate (4),
   volume up / down, mute / unmute
   faces: happy face, sad face, angry face, surprise, love face, wink, confused

7b. SMART MODEL SWITCHING (free Gemini limits)
   - At start-up FAMI BOT tests the speed of every model in AUDIO_MODELS and
     TEXT_ONLY_MODELS (one tiny request each) and ranks them fastest first.
   - Every question uses the FASTEST model that is not resting.
   - When a model answers 429 (free limit), it rests for the time Google asks
     (3 hours for a daily limit) and FAMI BOT silently uses the next fastest one.
   - It also learns each model's per-minute limit and avoids hitting it again.
   - Voice (TTS) models rotate the same way. If all are resting: offline voice.
   - Type "models" in the Serial Monitor to see the ranking and which are resting.
   - Right screen shows "AI off" only when ALL models are resting.

7c. ANSWERS NEVER MIX ANY MORE
   - A new question (typed, BOOT, or voice) stops the current answer immediately
     and clears any old sentences still waiting.
   - The mic ignores the first 0.7 s after speaking (no echo of its own voice).
   - The voice model gets ONLY the words to say (no "say it cheerfully" text).

7d. CLOCK TOOLS (say after "Hi Fami", or type - all work offline)
   TIMER     : "timer 5 minutes", "set a timer for 1 hour 30 minutes", "timer 30 seconds",
               "how much time left", "cancel timer"        (left screen shows T04:59)
   STOPWATCH : "start stopwatch", "stop stopwatch", "stopwatch", "reset stopwatch"   (S01:23)
   ALARM     : "alarm at 7:30 am", "wake me up at six forty five", "alarm 18:00",
               "what is my alarm", "cancel alarm"           (right screen shows bell + time)
   Ringing: orange flashing light + beeps for up to 1 minute.
   Stop it with ANY key, BOOT, or by typing "stop". The alarm survives a power cut.
   NOTE: new voice clips -> flash the NEW fami_voice.bin (same command as before).

7e. BLUETOOTH MUSIC: not possible. The ESP32-S3 chip has only Bluetooth Low Energy,
   phones need "classic" Bluetooth (A2DP) to send music.

8. SETTINGS (top of fami_bot.ino)
   WAKE_LEVEL 700   lower (500) if "Hi FAMI" is not heard, higher if noise wakes it
   WAKE_MAX_MS 2500 longer sounds are ignored on the board (never sent to Google)
   WAKE_GAP_MS 4000 minimum time between two wake checks
   SILENCE_LEVEL 500, MIC_GAIN 0x0E, MIC_MODE 4, NR_LEVEL 3, AUDIO_RATE 48000

9. IF UPLOAD FAILS: close Serial Monitor; hold BOOT, press RESET, release BOOT; check port.
