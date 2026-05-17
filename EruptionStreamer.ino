#include <SPI.h>
#include <VS1053.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "cred.h"
#include "spectrumAnalyzer1053b.h"
#include "radio_bitmap.h"

// ------------------------------------------------------------
// Display
// ------------------------------------------------------------
#define USE_DISPLAY 1  // 1 = OLED aktiv, 0 = OLED aus

// ------------------------------------------------------------
// ESP32 SPI Pins
// ------------------------------------------------------------
#define ESP32_SCK 18
#define ESP32_MISO 19
#define ESP32_MOSI 23

// ------------------------------------------------------------
// VS1053 Pins
// ------------------------------------------------------------
#define VS1053_CS 32    // XCS
#define VS1053_DCS 33   // XDCS
#define VS1053_DREQ 35  // DREQ
#define VS1053_RST 27   // RST / XRST / XRESET

// ------------------------------------------------------------
// OLED SSD1306 / SSD1309 I2C
// ------------------------------------------------------------
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C

#define OLED_DRIVER_AUTO   0
#define OLED_DRIVER_SSD1306 1
#define OLED_DRIVER_SSD1309 2
#define OLED_DRIVER OLED_DRIVER_AUTO

#define OLED_DISPLAY_OFFSET_Y 0

#define OLED_CONTRAST_SSD1306 0xCF
#define OLED_CONTRAST_SSD1309 0x9F

// ------------------------------------------------------------
// Audio
// ------------------------------------------------------------
#define VOLUME 85

// ------------------------------------------------------------
// Timing
// ------------------------------------------------------------
#define SPECTRUM_INTERVAL_MS 10 
#define DEBUG_INTERVAL_MS 2000
#define STREAM_TIMEOUT_MS 12000
#define RECONNECT_DELAY_MS 5000
#define SCHEDULE_REFRESH_MS 300000UL

// ------------------------------------------------------------
// Boot-Zoom-Stufen fuer das Startbild
// ------------------------------------------------------------
#define BOOT_STAGE_POWER_ON       0
#define BOOT_STAGE_SYSTEM_INFO    1
#define BOOT_STAGE_VS1053_OK      2
#define BOOT_STAGE_PLUGIN_OK      3
#define BOOT_STAGE_WIFI_START     4
#define BOOT_STAGE_WIFI_OK        5
#define BOOT_STAGE_STREAM_TCP     6
#define BOOT_STAGE_STREAM_HEADERS 7
#define BOOT_STAGE_STREAM_READY   8

// ------------------------------------------------------------
// Startup-Ueberblendung
// ------------------------------------------------------------
#define STARTUP_TILE_SIZE 8
#define STARTUP_TILE_COLS (OLED_WIDTH / STARTUP_TILE_SIZE)
#define STARTUP_TILE_ROWS (OLED_HEIGHT / STARTUP_TILE_SIZE)
#define STARTUP_TILE_COUNT (STARTUP_TILE_COLS * STARTUP_TILE_ROWS)
#define STARTUP_TILE_REVEAL_PER_FRAME 4

// ------------------------------------------------------------
// VS1053 SCI Register
// ------------------------------------------------------------
#define SCI_WRAM 0x06
#define SCI_WRAMADDR 0x07

// ------------------------------------------------------------
// Spectrum Plugin Adressen
// ------------------------------------------------------------
#define MAX_BANDS 14

uint16_t spectrumBandsAddr = 0;
uint16_t spectrumDataAddr = 0;

// ------------------------------------------------------------
// Eruption Radio UK Stream
// ------------------------------------------------------------
const char* STREAM_HOST = "streaming04.liveboxstream.uk";
const char* STREAM_PATH = "/stream";
const uint16_t STREAM_PORT = 8116;

// ------------------------------------------------------------
// Eruption Radio UK Schedule API
// ------------------------------------------------------------
const char* SCHEDULE_URL = "https://api.eruptionradio.uk/schedule/now";
const char* EDGE_WIN11_USER_AGENT =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
  "AppleWebKit/537.36 (KHTML, like Gecko) "
  "Chrome/124.0.0.0 Safari/537.36 Edg/124.0.2478.80";

// ------------------------------------------------------------
// Objekte
// ------------------------------------------------------------
VS1053 player(VS1053_CS, VS1053_DCS, VS1053_DREQ);
WiFiClient client;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
uint8_t activeOledDriver = OLED_DRIVER_AUTO;

// FreeRTOS Task Handles
TaskHandle_t AudioTaskHandle = NULL;

// ------------------------------------------------------------
// Buffer / Status / Leistungsdaten
// ------------------------------------------------------------
uint8_t mp3buff[32];

uint8_t bandCount = MAX_BANDS;
volatile uint8_t spectrum[MAX_BANDS]; 
uint8_t peaks[MAX_BANDS];

bool vs1053Ready = false;
bool pluginReady = false;
bool displayReady = false;
bool streamReady = false;
bool startupImageVisible = false;
bool musicStarted = false;
bool startupTransitionActive = false;
uint8_t startupBootStage = BOOT_STAGE_POWER_ON;
uint8_t startupTileOrder[STARTUP_TILE_COUNT];
bool startupTileRevealed[STARTUP_TILE_COUNT];
uint16_t startupTilesRevealedCount = 0;
unsigned long lastDataTime = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastSpectrumRead = 0;
unsigned long lastSpectrumDebug = 0;
unsigned long dotTimer = 0;

// Performance-Messvariablen (Echtzeit)
volatile int currentFPS = 0;
volatile int cpuLoadCore0 = 0;
volatile int cpuLoadCore1 = 0;

// Variablen fuer die FreeRTOS-Idle-Zaehlung
volatile uint32_t idleCountCore0 = 0;
volatile uint32_t idleCountCore1 = 0;
uint32_t maxIdleCore0 = 0;
uint32_t maxIdleCore1 = 0;

// ------------------------------------------------------------
// DYNAMISCHE METADATEN SPEICHER
// ------------------------------------------------------------
String stationName = "Loading...";
String streamTitle = "E R U P T I O N   R A D I O   U K           ";
String currentSongTitle = "";
String currentShowTime = "";
String currentShowTitle = "";
String currentPresenterName = "";
String currentGenres = "";
int metaInterval = 0;          
int byteCounter = 0;
unsigned long lastScheduleFetch = 0;

// Vorwärtsdeklarationen
void printSystemInfo();
bool initVS1053Reliable();
bool loadSpectrumPluginReliable();
void initDisplay();
bool i2cDeviceExists(uint8_t addr);
void applyOledControllerProfile(uint8_t driver);
const char* oledDriverName(uint8_t driver);
void showMessage(const char* msg);
void showStartupImage();
void setStartupBootStage(uint8_t stage);
uint8_t startupScaleFromBootStage(uint8_t stage);
bool startupImageScaledPixelOn(int16_t x, int16_t y, uint8_t scalePercent);
void initStartupTileTransition();
void advanceStartupTileTransition();
void overlayStartupImageTiles(uint8_t scalePercent);
void renderSpectrumSceneToBuffer();
void hideStartupImageWhenMusicStarts();
void connectToWiFi();
bool fetchScheduleNow();
void updateScheduleIfDue();
String jsonStringValue(const String& json, const char* key);
String cleanJsonText(String value);
void updateScrollerText();
void connectToStream();
void maintainWiFi();
void readStreamToVS1053();
void handleReconnectIfNeeded();
bool skipAndParseHttpHeaders();
void readSpectrum();
void drawSpectrum();

// Idle Task fuer Core 0 (Misst die verbleibende Freizeit von Kern 0)
void idleMeasurerTaskCore0(void * pvParameters) {
  for(;;) {
    idleCountCore0++;
    vTaskDelay(0); // Gibt die CPU sofort wieder frei
  }
}

// Task für Core 0 (Audio & Netzwerk)
void audioCoreTask(void * pvParameters) {
  // Starte den CPU-Messer fuer Kern 0 als Sub-Task auf Kern 0 (niedrigste Prio)
  xTaskCreatePinnedToCore(idleMeasurerTaskCore0, "Idle0", 2048, NULL, 0, NULL, 0);

  for(;;) {
    maintainWiFi();
    readStreamToVS1053();
    handleReconnectIfNeeded();
    vTaskDelay(1 / portTICK_PERIOD_MS); 
  }
}

// ------------------------------------------------------------
// SETUP
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(250);

#if USE_DISPLAY
  initDisplay();
  showStartupImage();
#endif

  Serial.println();
  Serial.println("=======================================");
  Serial.println(" ESP32 Radio + CPU Profiler (Dual-Core)");
  Serial.println("=======================================");

  printSystemInfo();
  setStartupBootStage(BOOT_STAGE_SYSTEM_INFO);
  if (!initVS1053Reliable()) {
    Serial.println("[STOP] VS1053 konnte nicht initialisiert werden.");
    while (true) { delay(1000); }
  }
  setStartupBootStage(BOOT_STAGE_VS1053_OK);

  pluginReady = loadSpectrumPluginReliable();
  setStartupBootStage(BOOT_STAGE_PLUGIN_OK);

  connectToWiFi();
  connectToStream();

  // START DES AUDIO-TASKS AUF CORE 0
  xTaskCreatePinnedToCore(
    audioCoreTask,     
    "AudioTask",       
    8192,              
    NULL,              
    1,                 
    &AudioTaskHandle,  
    0                  
  );
}

// ------------------------------------------------------------
// LOOP (Läuft nativ auf Core 1 - Übernimmt UI, FPS, FFT & CPU1-Messung)
// ------------------------------------------------------------
void loop() {
  // CPU-Leerlauf auf Kern 1 hochzaehlen
  static uint32_t internalIdle1 = 0;
  internalIdle1++;

  updateScheduleIfDue();

  // Sekundlicher Performance-Check (FPS & CPU Auslastung)
  static unsigned long lastPerfMs = 0;
  static int frameCount = 0;
  frameCount++;

  if (millis() - lastPerfMs >= 1000) {
    currentFPS = frameCount;
    frameCount = 0;

    // Kopiere Werte atomar rüber
    uint32_t c0Idle = idleCountCore0;
    uint32_t c1Idle = internalIdle1;
    idleCountCore0 = 0;
    internalIdle1 = 0;

    // Kalibrierung beim Systemstart (die ersten Sekunden ermitteln das Maximum)
    if (c0Idle > maxIdleCore0) maxIdleCore0 = c0Idle;
    if (c1Idle > maxIdleCore1) maxIdleCore1 = c1Idle;

    // Berechnung der prozentualen Last (100% minus freie Zeit)
    if (maxIdleCore0 > 0) {
      cpuLoadCore0 = 100 - ((c0Idle * 100) / maxIdleCore0);
    }
    if (maxIdleCore1 > 0) {
      cpuLoadCore1 = 100 - ((c1Idle * 100) / maxIdleCore1);
    }

    // Schutz vor negativen oder fehlerhaften Spitzenwerten
    cpuLoadCore0 = constrain(cpuLoadCore0, 0, 100);
    cpuLoadCore1 = constrain(cpuLoadCore1, 0, 100);

    lastPerfMs = millis();
  }

  if (millis() - lastSpectrumRead > SPECTRUM_INTERVAL_MS) {
    if (pluginReady) {
      readSpectrum(); 
#if USE_DISPLAY
      if (spectrumBandsAddr != 0) {
        drawSpectrum(); 
      }
#endif
    }
    lastSpectrumRead = millis();
  }

  if (millis() - lastSpectrumDebug > DEBUG_INTERVAL_MS) {
    if (!pluginReady) {
      Serial.println("[SPEC] Plugin nicht bereit.");
    } else if (spectrumBandsAddr == 0) {
      Serial.println("[SPEC] Warte auf gueltige FFT-Daten vom DSP...");
    }
    lastSpectrumDebug = millis();
  }
}

// ------------------------------------------------------------
// Systeminfo
// ------------------------------------------------------------
void printSystemInfo() {
  Serial.print("[SYS] CPU MHz: ");
  Serial.println(getCpuFrequencyMhz());
  Serial.print("[SYS] Free heap: ");
  Serial.println(ESP.getFreeHeap());
  Serial.println();
}

// ------------------------------------------------------------
// VS1053 INIT
// ------------------------------------------------------------
bool initVS1053Reliable() {
  Serial.println("[VS1053] Initialisiere...");

  pinMode(VS1053_CS, OUTPUT);
  pinMode(VS1053_DCS, OUTPUT);
  pinMode(VS1053_DREQ, INPUT);
  pinMode(VS1053_RST, OUTPUT);

  digitalWrite(VS1053_CS, HIGH);
  digitalWrite(VS1053_DCS, HIGH);
  digitalWrite(VS1053_RST, HIGH);
  delay(100);

  SPI.begin(ESP32_SCK, ESP32_MISO, ESP32_MOSI, VS1053_CS);
  delay(100);
  for (int attempt = 1; attempt <= 10; attempt++) {
    Serial.print("[VS1053] Init Versuch ");
    Serial.println(attempt);

    digitalWrite(VS1053_CS, HIGH);
    digitalWrite(VS1053_DCS, HIGH);
    digitalWrite(VS1053_RST, LOW);
    delay(100);
    digitalWrite(VS1053_RST, HIGH);
    delay(700);

    player.begin();
    delay(100);

    uint8_t version = player.getChipVersion();
    Serial.print("[VS1053] Chip Version: ");
    Serial.println(version);
    if (version == 4) {
      player.switchToMp3Mode();
      delay(50);
      player.setVolume(VOLUME);
      delay(50);
      Serial.println("[VS1053] OK");
      return true;
    }
    delay(500);
  }
  return false;
}

bool resetVS1053ForReconnect() {
  Serial.println("[VS1053] Reset fuer Reconnect...");
  digitalWrite(VS1053_CS, HIGH);
  digitalWrite(VS1053_DCS, HIGH);
  digitalWrite(VS1053_RST, LOW);
  delay(100);
  digitalWrite(VS1053_RST, HIGH);
  delay(700);

  player.begin();
  delay(100);

  if (player.getChipVersion() != 4) return false;
  player.switchToMp3Mode();
  delay(50);
  player.setVolume(VOLUME);
  delay(50);
  return true;
}

// ------------------------------------------------------------
// Spectrum Plugin
// ------------------------------------------------------------
bool loadSpectrumPluginReliable() {
  Serial.println("[PLUGIN] Lade Spectrum Analyzer Plugin...");
  player.loadUserCode(spectrumAnalyzer1053bPlugin, SPECTRUM_ANALYZER_1053B_PLUGIN_SIZE);
  delay(300);
  return true;
}

uint16_t readVS1053Wram(uint16_t address) {
  player.writeRegister(SCI_WRAMADDR, address);
  return player.readRegister(SCI_WRAM);
}

void readSpectrum() {
  if (!pluginReady) return;
  if (spectrumBandsAddr == 0) {
    uint16_t bands_1802 = readVS1053Wram(0x1802);
    uint16_t bands_1812 = readVS1053Wram(0x1812);
    if (bands_1802 > 0 && bands_1802 <= MAX_BANDS) {
      spectrumBandsAddr = 0x1802;
      spectrumDataAddr = 0x1804;
      bandCount = bands_1802;
    } else if (bands_1812 > 0 && bands_1812 <= MAX_BANDS) {
      spectrumBandsAddr = 0x1812;
      spectrumDataAddr = 0x1824;
      bandCount = bands_1812;
    } else {
      return;
    }
  }

  player.writeRegister(SCI_WRAMADDR, spectrumDataAddr);
  for (uint8_t i = 0; i < bandCount; i++) {
    uint16_t raw = player.readRegister(SCI_WRAM);
    uint8_t current = raw & 0x3F;
    uint8_t peak = (raw >> 6) & 0x3F;
    if (current > 31) current = 31;
    if (peak > 31) peak = 31;

    spectrum[i] = current;
    if (peak > peaks[i]) peaks[i] = peak;
    else if (peaks[i] > 0) peaks[i]--;
  }
}

// ------------------------------------------------------------
// OLED
// ------------------------------------------------------------
void initDisplay() {
#if USE_DISPLAY
  Serial.println("[OLED] Initialisiere...");
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000); 
  delay(50);
  if (!i2cDeviceExists(OLED_ADDR)) {
    Serial.print("[OLED] FEHLER: Kein OLED auf I2C-Adresse 0x");
    Serial.println(OLED_ADDR, HEX);
    displayReady = false;
    return;
  }

#if OLED_DRIVER == OLED_DRIVER_SSD1306
  activeOledDriver = OLED_DRIVER_SSD1306;
#elif OLED_DRIVER == OLED_DRIVER_SSD1309
  activeOledDriver = OLED_DRIVER_SSD1309;
#else
  activeOledDriver = OLED_DRIVER_SSD1309; 
#endif

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] FEHLER: Display-Lib konnte nicht starten.");
    displayReady = false;
    return;
  }

  applyOledControllerProfile(activeOledDriver);
  displayReady = true;
#endif
}

bool i2cDeviceExists(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

const char* oledDriverName(uint8_t driver) {
  if (driver == OLED_DRIVER_SSD1306) return "SSD1306";
  if (driver == OLED_DRIVER_SSD1309) return "SSD1309";
  return "AUTO";
}

void applyOledControllerProfile(uint8_t driver) {
#if USE_DISPLAY
  display.ssd1306_command(0xAE); 
  display.ssd1306_command(0xD5); display.ssd1306_command(0x80); 
  display.ssd1306_command(0xA8); display.ssd1306_command(0x3F); 
  display.ssd1306_command(0xD3); display.ssd1306_command((uint8_t)OLED_DISPLAY_OFFSET_Y);
  display.ssd1306_command(0x40);
  display.ssd1306_command(0x20); display.ssd1306_command(0x00); 
  display.ssd1306_command(0xA1); 
  display.ssd1306_command(0xC8);
  display.ssd1306_command(0xDA); display.ssd1306_command(0x12); 

  if (driver == OLED_DRIVER_SSD1309) {
    display.ssd1306_command(0x81);
    display.ssd1306_command(OLED_CONTRAST_SSD1309);
    display.ssd1306_command(0xD9); display.ssd1306_command(0xF1);
    display.ssd1306_command(0xDB); display.ssd1306_command(0x40);
  } else {
    display.ssd1306_command(0x81); display.ssd1306_command(OLED_CONTRAST_SSD1306);
    display.ssd1306_command(0xD9); display.ssd1306_command(0xF1);
    display.ssd1306_command(0xDB); display.ssd1306_command(0x40);
  }

  display.ssd1306_command(0x8D);
  display.ssd1306_command(0x14); 
  display.ssd1306_command(0xA4); 
  display.ssd1306_command(0xA6); 
  display.ssd1306_command(0x2E);
  display.ssd1306_command(0xAF); 
  display.clearDisplay();
  display.display();
#endif
}

void drawSpectrum() {
#if USE_DISPLAY
  if (!displayReady || !pluginReady || spectrumBandsAddr == 0 || bandCount == 0) return;
  if (startupImageVisible && !musicStarted) return;

  if (startupTransitionActive) {
    advanceStartupTileTransition();
    renderSpectrumSceneToBuffer();
    overlayStartupImageTiles(100);
    display.display();
    return;
  }

  renderSpectrumSceneToBuffer();
  display.display();
#endif
}

void showMessage(const char* msg) {
#if USE_DISPLAY
  if (!displayReady) return;
  if ((startupImageVisible && !musicStarted) || startupTransitionActive) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(stationName); 
  display.setCursor(0, 18);
  display.println(msg);
  display.display();
#endif
}

uint8_t startupScaleFromBootStage(uint8_t stage) {
  if (stage > 8) stage = 8;
  uint8_t scale = 10 + (stage * 10);
  if (scale > 90) scale = 90;
  return scale;
}

bool startupImageScaledPixelOn(int16_t x, int16_t y, uint8_t scalePercent) {
  const int16_t srcCx = STARTUP_IMAGE_WIDTH / 2;
  const int16_t srcCy = STARTUP_IMAGE_HEIGHT / 2;
  const int16_t dstCx = STARTUP_IMAGE_WIDTH / 2;
  const int16_t dstCy = STARTUP_IMAGE_HEIGHT / 2;

  int16_t sx = srcCx + ((int32_t)(x - dstCx) * 100L) / scalePercent;
  int16_t sy = srcCy + ((int32_t)(y - dstCy) * 100L) / scalePercent;
  return startupImageGetPixel(sx, sy);
}

void initStartupTileTransition() {
  for (uint16_t i = 0; i < STARTUP_TILE_COUNT; i++) {
    startupTileOrder[i] = i;
    startupTileRevealed[i] = false;
  }
  startupTilesRevealedCount = 0;

  randomSeed((uint32_t)micros() ^ (uint32_t)millis());
  for (int i = STARTUP_TILE_COUNT - 1; i > 0; i--) {
    int j = random(i + 1);
    uint8_t tmp = startupTileOrder[i];
    startupTileOrder[i] = startupTileOrder[j];
    startupTileOrder[j] = tmp;
  }
}

void advanceStartupTileTransition() {
  if (!startupTransitionActive) return;
  for (uint8_t i = 0; i < STARTUP_TILE_REVEAL_PER_FRAME; i++) {
    if (startupTilesRevealedCount >= STARTUP_TILE_COUNT) break;
    uint8_t tileIndex = startupTileOrder[startupTilesRevealedCount];
    startupTileRevealed[tileIndex] = true;
    startupTilesRevealedCount++;
  }

  if (startupTilesRevealedCount >= STARTUP_TILE_COUNT) {
    startupTransitionActive = false;
    startupImageVisible = false;
  }
}

void overlayStartupImageTiles(uint8_t scalePercent) {
  for (uint16_t tileIndex = 0; tileIndex < STARTUP_TILE_COUNT; tileIndex++) {
    if (startupTileRevealed[tileIndex]) continue;
    int16_t tileX = (tileIndex % STARTUP_TILE_COLS) * STARTUP_TILE_SIZE;
    int16_t tileY = (tileIndex / STARTUP_TILE_COLS) * STARTUP_TILE_SIZE;
    display.fillRect(tileX, tileY, STARTUP_TILE_SIZE, STARTUP_TILE_SIZE, SSD1306_BLACK);

    for (int16_t py = tileY; py < tileY + STARTUP_TILE_SIZE; py++) {
      for (int16_t px = tileX; px < tileX + STARTUP_TILE_SIZE; px++) {
        if (startupImageScaledPixelOn(px, py, scalePercent)) {
          display.drawPixel(px, py, SSD1306_WHITE);
        }
      }
    }
  }
}

void showStartupImage() {
#if USE_DISPLAY
  if (!displayReady) return;
  startupBootStage = BOOT_STAGE_POWER_ON;
  startupImageVisible = true;
  startupTransitionActive = false;
  musicStarted = false;
  startupTilesRevealedCount = 0;
  memset(startupTileRevealed, 0, sizeof(startupTileRevealed));
  drawStartupImageZoom(display, startupScaleFromBootStage(startupBootStage));
#endif
}

void setStartupBootStage(uint8_t stage) {
#if USE_DISPLAY
  if (!displayReady || !startupImageVisible || musicStarted || startupTransitionActive) return;
  if (stage > 8) stage = 8;
  if (stage < startupBootStage) return;

  startupBootStage = stage;
  drawStartupImageZoom(display, startupScaleFromBootStage(startupBootStage));
#endif
}

void hideStartupImageWhenMusicStarts() {
#if USE_DISPLAY
  if (!displayReady || !startupImageVisible || startupTransitionActive) return;
  musicStarted = true;
  startupBootStage = BOOT_STAGE_STREAM_READY;
  drawStartupImageZoom(display, 100);
  initStartupTileTransition();
  startupTransitionActive = true;
#endif
}

// ------------------------------------------------------------
// HOCH-OPTIMIERTER RENDERER FOR UI, SCHATTEN-TEXT & PERFORMANCE INFO
// ------------------------------------------------------------
void renderSpectrumSceneToBuffer() {
  display.clearDisplay();

  // 1. BAYER-RASTER SPECTRUM
  static const uint8_t bayer4x4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
  };

  const int screenH = OLED_HEIGHT;
  const int centerY = OLED_HEIGHT / 2;
  const int slotWidth = 11;
  const int minBarWidth = 3;
  const int gap = 2;
  const int startX = 0;
  const uint8_t rasterLevels = 16;

  for (uint8_t i = 0; i < 10; i++) {
    if (i >= bandCount) break;
    float currentGain = 22.0;
    if (i == 0 || i == 1) currentGain = 22.0;
    else if (i <= 4)      currentGain = 20.0;
    else if (i <= 6)      currentGain = 18.0;
    else if (i == 7)      currentGain = 16.0;
    else                  currentGain = 14.0;
    int gain = (int)currentGain;
    int level = constrain((int)spectrum[i], 0, gain);

    int barHeight = map(level, 0, gain, 0, screenH);
    if (level > 0 && barHeight < 2) barHeight = 2;
    if (barHeight > screenH) barHeight = screenH;
    int yTop = centerY - (barHeight / 2);
    int yBottom = yTop + barHeight - 1;
    if (yTop < 0) yTop = 0;
    if (yBottom >= OLED_HEIGHT) yBottom = OLED_HEIGHT - 1;
    int barWidth = map(level, 0, gain, minBarWidth, slotWidth);
    barWidth = constrain(barWidth, minBarWidth, slotWidth);
    int slotX = startX + (i * (slotWidth + gap));
    int x = slotX + ((slotWidth - barWidth) / 2);
    int densityLevel = map(level, 0, gain, 0, rasterLevels);
    densityLevel = constrain(densityLevel, 0, (int)rasterLevels);

    if (densityLevel > 0 && barHeight > 0) {
      for (int py = yTop; py <= yBottom; py++) {
        if (py < 0 || py >= OLED_HEIGHT) continue;
        for (int px = x; px < x + barWidth; px++) {
          if (px < 0 || px >= OLED_WIDTH) continue;
          uint8_t threshold = bayer4x4[py & 3][px & 3];
          if (densityLevel > threshold) {
            display.drawPixel(px, py, SSD1306_WHITE);
          }
        }
      }
    } else {
      for (int px = x; px < x + minBarWidth; px++) {
        if (px >= 0 && px < OLED_WIDTH) display.drawPixel(px, centerY, SSD1306_WHITE);
      }
    }
  }

  // 2. HIGH-SPEED SCROLLTEXT MIT SCHARFEM SCHATTENWURF
  static float scrollX = 128.0; 
  static float angleOffset = 0;
  const float scrollSpeed = 2.0; 

  int textLength = streamTitle.length();
  display.setTextSize(3);
  display.setTextWrap(false);

  const int charWidth = 19; 
  int startCharIdx = 0;
  if (scrollX < 0) {
    startCharIdx = ((int)(-scrollX)) / charWidth;
  }
  
  // DURCHLAUF 1: Der schwarze Schatten im Hintergrund
  display.setTextColor(SSD1306_BLACK);
  int currentX = ((int)scrollX) + (startCharIdx * charWidth);
  for (int charIdx = startCharIdx; charIdx < textLength; charIdx++) {
    if (currentX >= 128) break;
    char c = streamTitle[charIdx];
    float angle = angleOffset + (charIdx * 0.15);
    int yOffset = abs(sin(angle)) * 12;
    int targetY = 30 - yOffset;

    display.setCursor(currentX + 1, targetY + 1); 
    display.print(c);
    currentX += charWidth;
  }

  // DURCHLAUF 2: Der weiße Vordergrund-Text
  display.setTextColor(SSD1306_WHITE);
  currentX = ((int)scrollX) + (startCharIdx * charWidth);
  for (int charIdx = startCharIdx; charIdx < textLength; charIdx++) {
    if (currentX >= 128) break;
    char c = streamTitle[charIdx];
    float angle = angleOffset + (charIdx * 0.15);
    int yOffset = abs(sin(angle)) * 12;
    int targetY = 30 - yOffset;

    display.setCursor(currentX, targetY);
    display.print(c);
    currentX += charWidth;
  }

  scrollX -= scrollSpeed;
  angleOffset += 0.08; 
  
  if (scrollX < -(textLength * charWidth)) { 
    scrollX = 128.0;
  }

  // 3. PERFORMANCE-ANZEIGEN (FONT SIZE 1 IN DER UNTERSTEN ZEILE)
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Unten Links: CPU 0 Last (Audio/Wifi)
  display.setCursor(0, 56); 
  display.print("C0:");
  display.print(cpuLoadCore0);
  display.print("%");

  // Unten Mitte: CPU 1 Last (UI/Grafik)
  display.setCursor(55, 56); 
  display.print("C1:");
  display.print(cpuLoadCore1);
  display.print("%");

  // Unten Rechts: Aktuelle Framerate
  display.setCursor(110, 56); 
  display.print(currentFPS);
}

// ------------------------------------------------------------
// WLAN
// ------------------------------------------------------------
void connectToWiFi() {
  Serial.println("[WIFI] Verbinde...");
  setStartupBootStage(BOOT_STAGE_WIFI_START);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true);
  delay(500);

  WiFi.begin(ssid, pass);
  showMessage("WiFi...");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    stationName = "WiFi Fehler";
    showMessage("FEHLER");
    return;
  }
  stationName = "WiFi Connected";
  setStartupBootStage(BOOT_STAGE_WIFI_OK);
  showMessage("OK");

  fetchScheduleNow();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  stationName = "WiFi lost";
  showMessage("Reconnect...");

  WiFi.disconnect();
  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    fetchScheduleNow();
    connectToStream();
  }
}

// ------------------------------------------------------------
// Eruption Schedule API
// ------------------------------------------------------------
String cleanJsonText(String value) {
  value.replace("\\/", "/");
  value.replace("\\\"", "\"");
  value.replace("\\n", " ");
  value.replace("\\r", " ");
  value.replace("\\t", " ");
  value.trim();
  return value;
}

String jsonStringValue(const String& json, const char* key) {
  String needle = String("\"") + key + "\":\"";
  int start = json.indexOf(needle);
  if (start < 0) return "";
  start += needle.length();

  String value = "";
  bool escaped = false;
  for (int i = start; i < json.length(); i++) {
    char c = json.charAt(i);
    if (escaped) {
      value += '\\';
      value += c;
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') break;
    value += c;
  }
  return cleanJsonText(value);
}

void updateScrollerText() {
  String text = "";
  if (currentShowTime.length() > 0) {
    text += currentShowTime;
  }
  if (currentShowTitle.length() > 0) {
    if (text.length() > 0) text += "   |   ";
    text += currentShowTitle;
  }
  if (currentPresenterName.length() > 0 && currentPresenterName != currentShowTitle) {
    if (text.length() > 0) text += "   |   ";
    text += currentPresenterName;
  }
  if (currentGenres.length() > 0) {
    if (text.length() > 0) text += "   |   ";
    text += currentGenres;
  }
  if (currentSongTitle.length() > 0) {
    if (text.length() > 0) text += "   |   ";
    text += currentSongTitle;
  }

  if (text.length() == 0) text = "E R U P T I O N   R A D I O   U K";
  streamTitle = text + "          ";
}

bool fetchScheduleNow() {
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.println("[SCHEDULE] Hole aktuelle Show...");
  lastScheduleFetch = millis();

  WiFiClientSecure scheduleClient;
  scheduleClient.setInsecure();

  HTTPClient http;
  http.setUserAgent(EDGE_WIN11_USER_AGENT);
  http.setTimeout(6000);
  if (!http.begin(scheduleClient, SCHEDULE_URL)) {
    Serial.println("[SCHEDULE] HTTP begin fehlgeschlagen.");
    return false;
  }

  http.addHeader("Accept", "application/json, text/plain, */*");
  http.addHeader("Accept-Language", "en-GB,en;q=0.9,de;q=0.8");
  http.addHeader("Cache-Control", "no-cache");
  http.addHeader("Pragma", "no-cache");
  http.addHeader("Connection", "close");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("[SCHEDULE] HTTP Fehler: ");
    Serial.println(code);
    http.end();
    return false;
  }

  String json = http.getString();
  http.end();

  currentShowTime = jsonStringValue(json, "show_time");
  currentShowTitle = jsonStringValue(json, "show");
  currentPresenterName = jsonStringValue(json, "name");
  currentGenres = jsonStringValue(json, "genres");

  updateScrollerText();
  return true;
}

void updateScheduleIfDue() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (lastScheduleFetch == 0) return;
  if (millis() - lastScheduleFetch >= SCHEDULE_REFRESH_MS) {
    fetchScheduleNow();
  }
}

// ------------------------------------------------------------
// Stream & Metadaten Parser
// ------------------------------------------------------------
void connectToStream() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("[STREAM] Verbinde...");
  stationName = "Connecting...";
  showMessage("Stream...");

  client.stop();
  delay(300);
  if (!client.connect(STREAM_HOST, STREAM_PORT)) {
    stationName = "Stream Error";
    showMessage("TCP FEHLER");
    streamReady = false;
    return;
  }
  setStartupBootStage(BOOT_STAGE_STREAM_TCP);

  client.print(String("GET ") + STREAM_PATH + " HTTP/1.0\r\n" +
               "Host: " + STREAM_HOST + "\r\n" +
               "User-Agent: " + EDGE_WIN11_USER_AGENT + "\r\n" +
               "Accept: */*\r\n" +
               "Icy-MetaData: 1\r\n" + 
               "Connection: close\r\n\r\n");

  if (!skipAndParseHttpHeaders()) {
    stationName = "Header Error";
    showMessage("FEHLER");
    streamReady = false;
    return;
  }

  setStartupBootStage(BOOT_STAGE_STREAM_HEADERS);

  Serial.println("[STREAM] Datenstrom laeuft.");
  streamReady = true;
  setStartupBootStage(BOOT_STAGE_STREAM_READY);
  byteCounter = 0;
  lastDataTime = millis();
}

bool skipAndParseHttpHeaders() {
  unsigned long start = millis();
  metaInterval = 0;
  stationName = "Radio Stream";
  while (client.connected() && millis() - start < 7000) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      
      if (line.length() == 0) return true; 

      if (line.startsWith("icy-name:")) {
        stationName = line.substring(9);
        stationName.trim();
        if (currentShowTime.length() == 0 && currentShowTitle.length() == 0) {
          streamTitle = stationName + "          ";
        }
      }
      if (line.startsWith("icy-metaint:")) {
        metaInterval = line.substring(12).toInt();
      }
      if (line.startsWith("icy-br:")) {
        String bitrate = line.substring(7);
        bitrate.trim();
      }
    }
  }
  return false;
}

void readStreamToVS1053() {
  if (!streamReady) return;
  uint8_t chunksProcessed = 0;
  while (client.available() > 0 && chunksProcessed < 10) {
    
    if (metaInterval > 0 && byteCounter == metaInterval) {
      int metaLen = client.read();
      if (metaLen > 0) {
        metaLen *= 16;
        String metaString = "";
        while (metaString.length() < metaLen) {
          if (client.available()) {
            metaString += (char)client.read();
          }
        }
        
        if (metaString.indexOf("StreamTitle='") != -1) {
          int startIdx = metaString.indexOf("StreamTitle='") + 13;
          int endIdx = metaString.indexOf("';", startIdx);
          if (endIdx > startIdx) {
            currentSongTitle = metaString.substring(startIdx, endIdx);
            currentSongTitle.trim();
            updateScrollerText();
          }
        }
      }
      byteCounter = 0;
    }

    int bytesToRead = sizeof(mp3buff);
    if (metaInterval > 0 && (byteCounter + bytesToRead > metaInterval)) {
      bytesToRead = metaInterval - byteCounter;
    }

    int bytesRead = client.read(mp3buff, bytesToRead);
    if (bytesRead > 0) {
      lastDataTime = millis();
#if USE_DISPLAY
      hideStartupImageWhenMusicStarts();
#endif
      player.playChunk(mp3buff, bytesRead);
      byteCounter += bytesRead;
      chunksProcessed++;
      if (millis() - dotTimer > 1000) {
        Serial.print(".");
        dotTimer = millis();
      }
    }
  }
}

void handleReconnectIfNeeded() {
  if (!streamReady) {
    if (millis() - lastReconnectAttempt > RECONNECT_DELAY_MS) {
      lastReconnectAttempt = millis();
      connectToStream();
    }
    return;
  }

  if (millis() - lastDataTime <= STREAM_TIMEOUT_MS) return;
  if (millis() - lastReconnectAttempt <= RECONNECT_DELAY_MS) return;

  Serial.println("\n[STREAM] Keine Daten mehr. Reconnect...");
  lastReconnectAttempt = millis();
  streamReady = false;
  client.stop();
  delay(300);
  if (resetVS1053ForReconnect()) {
    pluginReady = loadSpectrumPluginReliable();
    spectrumBandsAddr = 0;
  } else {
    pluginReady = false;
  }

  connectToStream();
  lastDataTime = millis();
}
