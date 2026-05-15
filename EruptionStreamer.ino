#include <SPI.h>
#include <VS1053.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "cred.h"
#include "spectrumAnalyzer1053b.h"

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
// OLED SSD1306 I2C
// ------------------------------------------------------------
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C

// ------------------------------------------------------------
// Audio
// ------------------------------------------------------------
#define VOLUME 72

// ------------------------------------------------------------
// Timing
// ------------------------------------------------------------
#define SPECTRUM_INTERVAL_MS 10
#define DEBUG_INTERVAL_MS 2000
#define STREAM_TIMEOUT_MS 12000
#define RECONNECT_DELAY_MS 5000

// ------------------------------------------------------------
// VS1053 SCI Register
// ------------------------------------------------------------
#define SCI_WRAM 0x06
#define SCI_WRAMADDR 0x07

// ------------------------------------------------------------
// Spectrum Plugin Adressen
// ------------------------------------------------------------
#define MAX_BANDS 14

uint16_t spectrumBandsAddr = 0;  // STARTET BEI 0 FÜR DYNAMISCHE ERKENNUNG
uint16_t spectrumDataAddr = 0;

// ------------------------------------------------------------
// Eruption Radio UK Stream
// ------------------------------------------------------------
const char* STREAM_HOST = "streaming04.liveboxstream.uk";
const char* STREAM_PATH = "/stream";
const uint16_t STREAM_PORT = 8116;

// ------------------------------------------------------------
// Objekte
// ------------------------------------------------------------
VS1053 player(VS1053_CS, VS1053_DCS, VS1053_DREQ);
WiFiClient client;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ------------------------------------------------------------
// Buffer / Status
// ------------------------------------------------------------
uint8_t mp3buff[32];

uint8_t bandCount = MAX_BANDS;
uint8_t spectrum[MAX_BANDS];
uint8_t peaks[MAX_BANDS];

bool vs1053Ready = false;
bool pluginReady = false;
bool displayReady = false;
bool streamReady = false;

unsigned long lastDataTime = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastSpectrumRead = 0;
unsigned long lastSpectrumDebug = 0;
unsigned long dotTimer = 0;

// ------------------------------------------------------------
// SETUP
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=======================================");
  Serial.println(" ESP32 Eruption Radio UK + VS1053 FFT");
  Serial.println(" Stabile Init-Version (Dynamic Fix 2)");
  Serial.println("=======================================");
  Serial.println();

  printSystemInfo();

  // 1. VS1053 initialisieren
  vs1053Ready = initVS1053Reliable();

  if (!vs1053Ready) {
    Serial.println("[STOP] VS1053 konnte nicht initialisiert werden.");
    while (true) { delay(1000); }
  }

  // 2. Plugin laden
  pluginReady = loadSpectrumPluginReliable();

  // 3. OLED starten
#if USE_DISPLAY
  initDisplay();
  showMessage("VS1053 OK");
#endif

  // 4. WLAN starten
  connectToWiFi();

  // 5. Stream verbinden
  connectToStream();
}

// ------------------------------------------------------------
// LOOP
// ------------------------------------------------------------

void loop() {
  maintainWiFi();

  // Verarbeitet Audio-Daten häppchenweise ohne Blockieren
  readStreamToVS1053();

  // FFT & Display alle 80ms aktualisieren
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

  // Debug-Ausgaben alle 2 Sekunden
  if (millis() - lastSpectrumDebug > DEBUG_INTERVAL_MS) {
    if (pluginReady && spectrumBandsAddr != 0) {
     // printSpectrumDebug();
    } else if (!pluginReady) {
      Serial.println("[SPEC] Plugin nicht bereit.");
    } else {
      Serial.println("[SPEC] Warte auf gueltige FFT-Daten vom DSP...");
    }
    lastSpectrumDebug = millis();
  }

  handleReconnectIfNeeded();
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
      Serial.println();
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

  uint8_t version = player.getChipVersion();
  if (version != 4) return false;

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
  Serial.println("[PLUGIN] Lade Spectrum Analyzer Plugin in den RAM...");

  player.loadUserCode(
    spectrumAnalyzer1053bPlugin,
    SPECTRUM_ANALYZER_1053B_PLUGIN_SIZE);

  delay(300);
  Serial.println("[PLUGIN] Geladen. Erkennung laeuft im Betrieb.");
  Serial.println();
  return true;
}

uint16_t readVS1053Wram(uint16_t address) {
  player.writeRegister(SCI_WRAMADDR, address);
  return player.readRegister(SCI_WRAM);
}

void readSpectrum() {
  if (!pluginReady) return;

  // Dynamische Erkennung während Musik läuft
  if (spectrumBandsAddr == 0) {
    uint16_t bands_1802 = readVS1053Wram(0x1802);
    uint16_t bands_1812 = readVS1053Wram(0x1812);

    if (bands_1802 > 0 && bands_1802 <= MAX_BANDS) {
      spectrumBandsAddr = 0x1802;
      spectrumDataAddr = 0x1804;
      bandCount = bands_1802;
      Serial.print("\n[PLUGIN] Erkennung ERFOLGREICH bei Adresse 0x1802! Bands: ");
      Serial.println(bandCount);
    } else if (bands_1812 > 0 && bands_1812 <= MAX_BANDS) {
      spectrumBandsAddr = 0x1812;
      spectrumDataAddr = 0x1824;
      bandCount = bands_1812;
      Serial.print("\n[PLUGIN] Erkennung ERFOLGREICH bei Adresse 0x1812! Bands: ");
      Serial.println(bandCount);
    } else {
      return;  // DSP dekodiert noch nicht lang genug, im nächsten Intervall erneut versuchen
    }
  }

  // Daten auslesen
  player.writeRegister(SCI_WRAMADDR, spectrumDataAddr);

  for (uint8_t i = 0; i < bandCount; i++) {
    uint16_t raw = player.readRegister(SCI_WRAM);

    uint8_t current = raw & 0x3F;
    uint8_t peak = (raw >> 6) & 0x3F;

    if (current > 31) current = 31;
    if (peak > 31) peak = 31;

    spectrum[i] = current;

    if (peak > peaks[i]) {
      peaks[i] = peak;
    } else if (peaks[i] > 0) {
      peaks[i]--;
    }
  }
}

void printSpectrumDebug() {

  Serial.print("[SPEC] ");
  for (uint8_t i = 0; i < bandCount; i++) {
    Serial.print(spectrum[i]);
    if (i < bandCount - 1) Serial.print(",");
  }
  Serial.println();
}

// ------------------------------------------------------------
// OLED
// ------------------------------------------------------------

void initDisplay() {
#if USE_DISPLAY
  Serial.println("[OLED] Initialisiere...");
  Wire.begin(OLED_SDA, OLED_SCL);
  delay(100);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] FEHLER: SSD1306 nicht gefunden.");
    displayReady = false;
    return;
  }

  displayReady = true;
  Serial.println("[OLED] OK");
  showMessage("Boot OK");
  Serial.println();
#endif
}

void showMessage(const char* msg) {
#if USE_DISPLAY
  if (!displayReady) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println("Eruption Radio UK");
  display.setCursor(0, 18);
  display.println(msg);
  display.display();
#endif
}

void drawSpectrum() {
#if USE_DISPLAY
  if (!displayReady) return;
  if (!pluginReady) return;
  if (spectrumBandsAddr == 0 || bandCount == 0) return;

  display.clearDisplay();

  int centerY = 32;     
  int maxHalfHeight = 31; 
  int barWidth = 11;      
  int gap = 2;
  int startX = 0;

  // 1. ZUERST DIE BALKEN ZEICHNEN
  for (uint8_t i = 0; i < 10; i++) {
    if (i >= bandCount) break; 

    float currentGain = 22.0;
    if (i == 0 || i == 1) currentGain = 22.0; 
    else if (i <= 4)      currentGain = 20.0; 
    else if (i <= 6)      currentGain = 18.0; 
    else if (i == 7)      currentGain = 16.0; 
    else                  currentGain = 14.0; 

    int halfBarH = map(constrain(spectrum[i], 0, currentGain), 0, currentGain, 0, maxHalfHeight);
    int x = startX + (i * (barWidth + gap));

    if (halfBarH > 0) {
      int yTop = centerY - halfBarH;
      int yBottom = centerY + halfBarH;

      // 5-Stufen-Raster
      if (halfBarH >= 25) {
        display.fillRect(x, yTop, barWidth, halfBarH * 2, SSD1306_WHITE);
      }
      else if (halfBarH >= 19) {
        for (int py = yTop; py <= yBottom; py++) {
          for (int px = x; px < x + barWidth; px++) {
            if ((px + py) % 4 != 0) display.drawPixel(px, py, SSD1306_WHITE);
          }
        }
      }
      else if (halfBarH >= 13) {
        for (int py = yTop; py <= yBottom; py++) {
          if (py % 2 == 0) display.drawFastHLine(x, py, barWidth, SSD1306_WHITE);
        }
      }
      else if (halfBarH >= 7) {
        for (int py = yTop; py <= yBottom; py++) {
          for (int px = x; px < x + barWidth; px++) {
            if (px % 2 == 0 && py % 2 == 0) display.drawPixel(px, py, SSD1306_WHITE);
          }
        }
      }
      else {
        for (int py = yTop; py <= yBottom; py++) {
          for (int px = x; px < x + barWidth; px++) {
            if ((px + py) % 4 == 0 && px % 2 == 0) display.drawPixel(px, py, SSD1306_WHITE);
          }
        }
      }
    } else {
      display.drawFastHLine(x, centerY, barWidth, SSD1306_WHITE);
    }
  }

  // 2. JETZT DEN FETTEN HALBSINUS-HÜPFENDEN TEXT DARÜBERSTANZEN
  static int scrollX = 128; 
  static unsigned long lastScroll = 0;
  static float angleOffset = 0; // Bestimmt die Wellenbewegung über die Zeit
  
  const char* tickerText = "E R U P T I O N   R A D I O   U K           "; 
  int textLength = strlen(tickerText);

  display.setTextSize(3);
  display.setTextColor(SSD1306_BLACK); // Ausstanzen
  display.setTextWrap(false);

  int currentX = scrollX;

  // Wir müssen jeden Buchstaben einzeln zeichnen, damit er ein eigenes Y bekommt
  for (int charIdx = 0; charIdx < textLength; charIdx++) {
    char c = tickerText[charIdx];

    // Nur zeichnen, wenn der Buchstabe im sichtbaren Bereich ist
    if (currentX >= -6 && currentX < 128) {
      
      // Berechne den Sinus-Winkel für DIESEN spezifischen Buchstaben
      // charIdx * 0.5 sorgt für den Phasenversatz (jeder Buchstabe ist an einer anderen Stelle der Welle)
      float angle = angleOffset + (charIdx * 0.1);
      
      // abs(sin()) erzeugt die harte "Bounce"-Kurve eines springenden Balls (Halbsinus)
      // Der Buchstabe hüpft bis zu 12 Pixel weit nach oben aus der Mitte heraus
      int yOffset = abs(sin(angle)) * 12; 
      int targetY = 28 - yOffset; // Von der Mittellinie (28) nach oben abziehen

      // --- DER TRICK FÜR FETTSCHRIFT ---
      // Buchstabe 1 zeichnen
      display.setCursor(currentX, targetY);
      display.print(c);
      // Buchstabe 2 minimal versetzt zeichnen -> macht es fett!
      display.setCursor(currentX + 1, targetY);
      display.print(c);

    }
    
    // Abstand zum nächsten Zeichen (Fette Schrift braucht 1px mehr Platz)
    currentX += 9; 
  }

  // Ticker-Geschwindigkeit und Animations-Takt
  if (millis() - lastScroll >= 25) {
    scrollX--; // Text wandert nach links
    angleOffset += 0.15; // Erhöht die Geschwindigkeit des Hüpfens
    
    if (scrollX < -(textLength * 7)) { 
      scrollX = 128; 
    }
    lastScroll = millis();
  }

  display.display();
#endif
}

// ------------------------------------------------------------
// WLAN
// ------------------------------------------------------------

void connectToWiFi() {
  Serial.println("[WIFI] Verbinde...");
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
    Serial.println("[WIFI] FEHLER");
    showMessage("WiFi Fehler");
    return;
  }

  Serial.println("[WIFI] OK");
  showMessage("WiFi OK");
  Serial.println();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("\n[WIFI] Verbindung verloren. Reconnect...");
  showMessage("WiFi reconnect");

  WiFi.disconnect();
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    connectToStream();
  }
}

// ------------------------------------------------------------
// Stream
// ------------------------------------------------------------

void connectToStream() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("[STREAM] Verbinde...");
  showMessage("Stream...");

  client.stop();
  delay(300);

  if (!client.connect(STREAM_HOST, STREAM_PORT)) {
    Serial.println("[STREAM] TCP FEHLER");
    showMessage("Stream Fehler");
    streamReady = false;
    return;
  }

  client.print(String("GET ") + STREAM_PATH + " HTTP/1.0\r\n" + "Host: " + STREAM_HOST + "\r\n" + "User-Agent: ESP32-VS1053-Radio\r\n" + "Icy-MetaData: 0\r\n" + "Connection: close\r\n\r\n");

  if (!skipHttpHeaders()) {
    Serial.println("[STREAM] Header FEHLER");
    showMessage("Header Fehler");
    streamReady = false;
    return;
  }

  Serial.println("[STREAM] Datenstrom laeuft.");
  streamReady = true;
  lastDataTime = millis();
  showMessage("Playing...");
}

bool skipHttpHeaders() {
  unsigned long start = millis();
  while (client.connected() && millis() - start < 7000) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) return true;
    }
  }
  return false;
}

// HIER WAR DIE BLOCKADE: Jetzt limitiert auf max. 10 Chunks pro Aufruf!
void readStreamToVS1053() {
  if (!streamReady) return;

  uint8_t chunksProcessed = 0;

  while (client.available() > 0 && chunksProcessed < 10) {
    int bytesRead = client.read(mp3buff, sizeof(mp3buff));
    if (bytesRead > 0) {
      lastDataTime = millis();
      player.playChunk(mp3buff, bytesRead);
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

  bool resetOk = resetVS1053ForReconnect();
  if (resetOk) {
    pluginReady = loadSpectrumPluginReliable();
    spectrumBandsAddr = 0;  // Adresse zurücksetzen für Neuerkennung
  } else {
    pluginReady = false;
  }

  connectToStream();
  lastDataTime = millis();
}
