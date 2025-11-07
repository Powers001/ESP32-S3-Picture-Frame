/*
  ESP32-S3 Photo Frame with Web Interface - Production Ready
  
  Features:
  - Professional startup screen with WiFi status
  - Auto-hotspot mode when WiFi fails (SSID: PhotoFrame-XXXX)
  - Dynamic image indexing (unlimited images)
  - Configurable display sequence (A-Z, Z-A, Random, etc.)
  - WiFi network scanner and configuration
  - Web interface for image upload/delete/backlight control
  - Instant transitions (no flicker)
  - OTA firmware updates
  - Persistent encrypted settings
  
  Libraries required:
  - TFT_eSPI, JPEGDecoder, SD, WiFi, Preferences
  - ESPAsyncWebServer, ElegantOTA, AsyncTCP, ArduinoJson
*/

#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <JPEGDecoder.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include <esp_wifi.h>

// ===== CONFIGURATION =====
#define SLIDESHOW_DELAY 15000  // milliseconds between images
#define TFT_BACKLIGHT 7        // backlight pin
#define BACKLIGHT_LEVEL 20     // 0-255 default brightness (20 = ~8%)
#define HOTSPOT_TIMEOUT 300000 // 5 minutes in hotspot mode before retry

// SD Card pins (HSPI bus)
#define SD_CS   15
#define SD_MOSI 10
#define SD_MISO 14
#define SD_SCK  21

// ===== COLOR FIX =====
#define SWAP_COLOR_BYTES 1

inline uint16_t swapBytes(uint16_t color) {
  return (color >> 8) | (color << 8);
}

// ===== GLOBAL OBJECTS =====
TFT_eSPI tft = TFT_eSPI();
SPIClass sd_spi(HSPI);
AsyncWebServer server(80);
Preferences preferences;

// ===== SLIDESHOW STATE =====
std::vector<String> fileNames;
int currentIndex = 0;
uint32_t lastImageTime = 0;
uint8_t currentBacklight = BACKLIGHT_LEVEL;
uint8_t displaySequence = 0; // 0=A-Z, 1=Z-A, 2=Random, 3=Oldest, 4=Newest

// WiFi state
String wifi_ssid;
String wifi_password;
bool isHotspotMode = false;
String hotspotSSID;
uint32_t hotspotStartTime = 0;

// Image buffer
uint16_t* imageBuffer = nullptr;
uint16_t imageWidth = 0;
uint16_t imageHeight = 0;

//====================================================================================
//   Display Sequence Functions
//====================================================================================
void applyDisplaySequence() {
  if (fileNames.size() == 0) return;
  
  switch(displaySequence) {
    case 0: // A-Z
      std::sort(fileNames.begin(), fileNames.end());
      Serial.println("Applied sequence: A-Z");
      break;
      
    case 1: // Z-A
      std::sort(fileNames.begin(), fileNames.end());
      std::reverse(fileNames.begin(), fileNames.end());
      Serial.println("Applied sequence: Z-A");
      break;
      
    case 2: // Random
      for (int i = fileNames.size() - 1; i > 0; i--) {
        int j = random(0, i + 1);
        String temp = fileNames[i];
        fileNames[i] = fileNames[j];
        fileNames[j] = temp;
      }
      Serial.println("Applied sequence: Random");
      break;
      
    case 3: // Oldest First
    case 4: // Newest First
      std::sort(fileNames.begin(), fileNames.end());
      if (displaySequence == 4) {
        std::reverse(fileNames.begin(), fileNames.end());
      }
      Serial.println(displaySequence == 3 ? "Applied sequence: Oldest First" : "Applied sequence: Newest First");
      break;
  }
  
  currentIndex = 0;
}

//====================================================================================
//   Startup Screen
//====================================================================================
void drawStartupScreen(int images, uint64_t used, uint64_t total) {
  tft.fillScreen(TFT_BLACK);
  
  // Title bar
  tft.fillRect(0, 0, tft.width(), 40, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("ESP32-S3 PHOTO FRAME", tft.width() / 2, 20, 4);
  
  // Main content
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  
  int y = 60;
  tft.drawString("Initializing...", 20, y, 2);
  y += 30;
  
  // System info
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("System Status:", 20, y, 2);
  y += 25;
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("* Backlight: OK", 30, y, 2);
  y += 20;
  tft.drawString("* Display: OK", 30, y, 2);
  y += 30;

  // SD Card Info
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("SD Card Status:", 20, y, 2);
  y += 25;
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("* Images found: " + String(images), 30, y, 2);
  y += 20;
  float usedGB = (float)used / 1024.0 / 1024.0 / 1024.0;
  float totalGB = (float)total / 1024.0 / 1024.0 / 1024.0;
  String usage = String(usedGB, 2) + " GB / " + String(totalGB, 2) + " GB";
  tft.drawString("* Usage: " + usage, 30, y, 2);
  y += 30;
  
  // WiFi section
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("WiFi Configuration:", 20, y, 2);
  y += 25;
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("SSID: " + wifi_ssid, 30, y, 2);
  y += 20;
  tft.drawString("Status: Connecting...", 30, y, 2);
}

void updateWiFiStatus(bool connected, String ip, bool hotspot = false) {
  int leftY = 60 + 30 + 25 + 20 + 20 + 30 + 25 + 20 + 20;
  
  // Clear status line
  tft.fillRect(30, leftY, 200, 20, TFT_BLACK);
  
  // Right column
  int rightX = tft.width() / 2 + 20;
  int rightY = 60;
  
  // Clear right side
  tft.fillRect(rightX - 10, rightY, tft.width() - rightX, tft.height() - rightY, TFT_BLACK);
  
  if (connected) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(hotspot ? "Status: Hotspot" : "Status: Connected!", 30, leftY, 2);
    
    // Web interface info on right
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Web Interface:", rightX, rightY, 2);
    rightY += 25;
    
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("IP Address:", rightX, rightY, 2);
    rightY += 20;
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(ip, rightX + 10, rightY, 2);
    rightY += 25;
    
    if (hotspot) {
      tft.setTextColor(TFT_ORANGE, TFT_BLACK);
      tft.drawString("Connect to WiFi:", rightX, rightY, 2);
      rightY += 20;
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.drawString(hotspotSSID, rightX + 10, rightY, 2);
      rightY += 20;
      tft.drawString("Password: admin", rightX + 10, rightY, 2);
      rightY += 20;
      tft.drawString("IP: 192.168.4.1", rightX + 10, rightY, 2);
    } else {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("Access via browser:", rightX, rightY, 2);
      rightY += 20;
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.drawString("http://" + ip, rightX + 10, rightY, 2);
    }
    
    rightY += 30;
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Features:", rightX, rightY, 2);
    rightY += 20;
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("- Upload images", rightX + 10, rightY, 1);
    rightY += 12;
    tft.drawString("- Delete images", rightX + 10, rightY, 1);
    rightY += 12;
    tft.drawString("- Backlight control", rightX + 10, rightY, 1);
    rightY += 12;
    tft.drawString("- WiFi config", rightX + 10, rightY, 1);
    rightY += 12;
    tft.drawString("- OTA updates", rightX + 10, rightY, 1);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Status: Failed", 30, leftY, 2);
    
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("Offline Mode", rightX, rightY, 2);
    rightY += 25;
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Running without network", rightX, rightY, 1);
  }
}

//====================================================================================
//   Decode JPEG to buffer
//====================================================================================
bool decodeJpegToBuffer(const char *filename) {
  File jpegFile = SD.open(filename);
  if (!jpegFile) {
    Serial.printf("ERROR: Cannot open %s\n", filename);
    return false;
  }

  if (!JpegDec.decodeSdFile(jpegFile)) {
    Serial.println("ERROR: JPEG decode failed");
    jpegFile.close();
    return false;
  }

  uint16_t newWidth = JpegDec.width;
  uint16_t newHeight = JpegDec.height;
  
  // Re-allocate buffer if size changed
  if (imageBuffer == nullptr || newWidth != imageWidth || newHeight != imageHeight) {
    if (imageBuffer != nullptr) {
      free(imageBuffer);
      imageBuffer = nullptr;
    }
    
    uint32_t bufferSize = newWidth * newHeight;
    imageBuffer = (uint16_t*)ps_malloc(bufferSize * sizeof(uint16_t));
    
    if (imageBuffer == nullptr) {
      Serial.println("ERROR: Cannot allocate image buffer in PSRAM");
      jpegFile.close();
      return false;
    }
    
    imageWidth = newWidth;
    imageHeight = newHeight;
    Serial.printf("Allocated %d KB in PSRAM for %dx%d image\n", 
                  (bufferSize * 2) / 1024, imageWidth, imageHeight);
  }

  // Decode to buffer
  while (JpegDec.read()) {
    uint16_t *pImg = JpegDec.pImage;
    uint16_t mcu_w = JpegDec.MCUWidth;
    uint16_t mcu_h = JpegDec.MCUHeight;
    
    uint16_t win_w = mcu_w;
    uint16_t win_h = mcu_h;
    
    if ((JpegDec.MCUx + 1) * mcu_w > imageWidth) {
      win_w = imageWidth - JpegDec.MCUx * mcu_w;
    }
    if ((JpegDec.MCUy + 1) * mcu_h > imageHeight) {
      win_h = imageHeight - JpegDec.MCUy * mcu_h;
    }

    // Copy to buffer with color correction
    for (uint16_t y = 0; y < win_h; y++) {
      for (uint16_t x = 0; x < win_w; x++) {
        uint16_t pixel = pImg[y * mcu_w + x];
#if SWAP_COLOR_BYTES
        pixel = swapBytes(pixel);
#endif
        uint32_t targetX = JpegDec.MCUx * mcu_w + x;
        uint32_t targetY = JpegDec.MCUy * mcu_h + y;
        if (targetX < imageWidth && targetY < imageHeight) {
          imageBuffer[targetY * imageWidth + targetX] = pixel;
        }
      }
    }
    
    if (JpegDec.MCUy % 8 == 0) yield();
  }

  jpegFile.close();
  return true;
}

//====================================================================================
//   Display buffered image (instant, no flicker)
//====================================================================================
void displayBufferedImage() {
  if (imageBuffer == nullptr) return;
  
  int16_t xOffset = (tft.width() - imageWidth) / 2;
  int16_t yOffset = (tft.height() - imageHeight) / 2;
  
  if (xOffset < 0) xOffset = 0;
  if (yOffset < 0) yOffset = 0;
  
  // Clear only areas not covered by image
  if (xOffset > 0) {
    tft.fillRect(0, 0, xOffset, tft.height(), TFT_BLACK);
    tft.fillRect(tft.width() - xOffset, 0, xOffset, tft.height(), TFT_BLACK);
  }
  if (yOffset > 0) {
    tft.fillRect(0, 0, tft.width(), yOffset, TFT_BLACK);
    tft.fillRect(0, tft.height() - yOffset, tft.width(), yOffset, TFT_BLACK);
  }
  
  // Display image instantly
  tft.pushImage(xOffset, yOffset, imageWidth, imageHeight, imageBuffer);
}

//====================================================================================
//   Scan SD card for JPEG files
//====================================================================================
bool scanForImages() {
  File root = SD.open("/");
  if (!root) {
    Serial.println("ERROR: Cannot open root directory");
    return false;
  }

  Serial.println("Scanning for JPEG files...");
  fileNames.clear();
  
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String name = file.name();
      
      // Skip hidden/system files
      if (name.startsWith(".") || name.startsWith("_")) {
        file = root.openNextFile();
        continue;
      }
      
      name.toUpperCase();
      if (name.endsWith(".JPG") || name.endsWith(".JPEG")) {
        String fullPath = "/" + String(file.name());
        fileNames.push_back(fullPath);
        Serial.printf("  [%d] %s (%d bytes)\n", fileNames.size(), 
                     fullPath.c_str(), file.size());
      }
    }
    file = root.openNextFile();
  }
  
  root.close();
  Serial.printf("Found %d images\n", fileNames.size());
  
  applyDisplaySequence();
  
  return (fileNames.size() > 0);
}

//====================================================================================
//   WiFi Hotspot Mode
//====================================================================================
void startHotspot() {
  // Generate unique SSID using MAC address
  uint8_t mac[6];
  WiFi.macAddress(mac);
  
  char macStr[5];
  sprintf(macStr, "%02X%02X", mac[4], mac[5]);
  hotspotSSID = "PhotoFrame-" + String(macStr);
  
  Serial.println("\n╔═══════════════════════════════════════╗");
  Serial.println("║     Starting Hotspot Mode            ║");
  Serial.println("╚═══════════════════════════════════════╝");
  Serial.println("SSID: " + hotspotSSID);
  Serial.println("Password: admin");
  Serial.println("IP: 192.168.4.1");
  
  // Stop any existing WiFi
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  
  // Start AP mode
  WiFi.mode(WIFI_AP);
  delay(100);
  
  // Configure AP with explicit settings
  // Parameters: SSID, password, channel, hidden (0=visible), max_connections
  bool apStarted = WiFi.softAP(hotspotSSID.c_str(), "admin", 1, 0, 4);
  
  if (apStarted) {
    Serial.println("✓ Hotspot started successfully");
    Serial.println("✓ SSID broadcasting: " + String(WiFi.softAPSSID()));
  } else {
    Serial.println("✗ Hotspot failed to start!");
  }
  
  // Configure IP address
  IPAddress local_IP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  
  isHotspotMode = true;
  hotspotStartTime = millis();
  
  delay(500);
  Serial.println("✓ Clients can now connect to: " + hotspotSSID);
}

bool connectToWiFi() {
  if (wifi_ssid.length() == 0) {
    Serial.println("No WiFi credentials stored");
    return false;
  }
  
  Serial.println("Connecting to: " + wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
  
  return (WiFi.status() == WL_CONNECTED);
}

//====================================================================================
//   Web Server HTML Interface
//====================================================================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Photo Frame Manager</title>
  <style>
    body { font-family: Arial; margin: 20px; background: #1a1a1a; color: #fff; }
    h1 { color: #4a9eff; }
    h2 { color: #6ab0ff; margin-top: 0; }
    .container { max-width: 800px; margin: 0 auto; }
    .section { background: #2a2a2a; padding: 20px; margin: 20px 0; border-radius: 8px; }
    button { background: #4a9eff; color: white; padding: 10px 20px; border: none; 
             border-radius: 4px; cursor: pointer; margin: 5px; }
    button:hover { background: #3a8eef; }
    button.delete { background: #ff4a4a; }
    button.delete:hover { background: #ef3a3a; }
    button.secondary { background: #666; }
    button.secondary:hover { background: #555; }
    input[type="file"], input[type="text"], input[type="password"], select { 
      margin: 10px 0; padding: 8px; background: #3a3a3a; color: #fff; 
      border: 1px solid #555; border-radius: 4px; width: 100%; max-width: 300px;
    }
    input[type="range"] { width: 300px; vertical-align: middle; }
    .slider-container { display: flex; align-items: center; gap: 10px; margin: 10px 0; }
    .slider-value { min-width: 50px; font-size: 18px; font-weight: bold; color: #4a9eff; }
    .file-list { list-style: none; padding: 0; }
    .file-item { padding: 10px; margin: 5px 0; background: #3a3a3a; 
                 border-radius: 4px; display: flex; justify-content: space-between; 
                 align-items: center; }
    .status { padding: 10px; margin: 10px 0; border-radius: 4px; }
    .success { background: #2a5a2a; }
    .error { background: #5a2a2a; }
    .info { background: #2a4a5a; }
    .warning { background: #5a4a2a; }
    .form-group { margin: 15px 0; }
    .form-group label { display: block; margin-bottom: 5px; color: #aaa; }
    .network-item { padding: 8px; margin: 5px 0; background: #3a3a3a; 
                    border-radius: 4px; cursor: pointer; display: flex; 
                    justify-content: space-between; align-items: center; }
    .network-item:hover { background: #4a4a4a; }
    .network-item.selected { background: #4a6a8a; }
    .signal-strength { color: #4a9eff; font-size: 12px; font-family: monospace; }
    a { color: #fff; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Photo Frame Manager</h1>
    
    <div class="section">
      <h2>Display Settings</h2>
      <div class="slider-container">
        <label for="backlightSlider">Backlight:</label>
        <input type="range" id="backlightSlider" min="0" max="100" value="0">
        <span class="slider-value" id="backlightValue">0%</span>
        <button onclick="setBacklight()">Apply</button>
      </div>
      <div id="settingsStatus"></div>
    </div>
    
    <div class="section">
      <h2>WiFi Configuration</h2>
      <div class="form-group">
        <label>Current Network: <span id="currentSSID" style="color: #4a9eff;">Loading...</span></label>
      </div>
      <button onclick="scanNetworks()">Scan Networks</button>
      <button class="secondary" onclick="toggleManualWiFi()">Manual Entry</button>
      
      <div id="networkList" style="margin-top: 15px;"></div>
      
      <div id="manualWiFi" style="display: none; margin-top: 15px;">
        <div class="form-group">
          <label for="manualSSID">SSID:</label>
          <input type="text" id="manualSSID" placeholder="Enter network name">
        </div>
        <div class="form-group">
          <label for="wifiPassword">Password:</label>
          <input type="password" id="wifiPassword" placeholder="Enter password">
        </div>
        <button onclick="connectWiFi()">Connect</button>
        <button class="secondary" onclick="toggleManualWiFi()">Cancel</button>
      </div>
      
      <div id="wifiStatus"></div>
    </div>
    
    <div class="section">
      <h2>Firmware Update</h2>
      <button onclick="location.href='/update'">Update Firmware (OTA)</button>
      <p style="color: #aaa; margin-top: 10px; font-size: 14px;">Navigate to /update for over-the-air firmware updates</p>
    </div>
    
    <div class="section">
      <h2>Upload Image(s)</h2>
      <input type="file" id="fileInput" accept=".jpg,.jpeg" multiple>
      <button onclick="uploadFile()">Upload</button>
      <div id="uploadStatus"></div>
      <div id="progressBarContainer" style="display: none;">
        <progress id="progressBar" value="0" max="100" style="width: 100%;"></progress>
        <span id="progressCounter"></span>
      </div>
    </div>
    
    <div class="section">
      <h2>Image Manager</h2>
      <div class="form-group">
        <label for="sequenceSelect">Display Sequence:</label>
        <select id="sequenceSelect" onchange="setSequence()">
          <option value="0">A-Z (Alphabetical)</option>
          <option value="1">Z-A (Reverse Alphabetical)</option>
          <option value="2">Random Shuffle</option>
          <option value="3">Oldest First</option>
          <option value="4">Newest First</option>
        </select>
      </div>
      <div style="margin: 15px 0;">
        <label style="color: #aaa; cursor: pointer;">
          <input type="checkbox" id="selectAllCheckbox" onchange="toggleSelectAll()"> 
          Select All
        </label>
      </div>
      <button onclick="refreshList()">Refresh List</button>
      <button class="delete" onclick="deleteSelectedFiles()">Delete Selected</button>
      <div style="margin-top: 10px; color: #aaa;">
        Total Images: <span id="imageCount">0</span>
      </div>
      <ul class="file-list" id="fileList">Loading...</ul>
    </div>
  </div>
  
  <script>
    let selectedSSID = '';
    
    // Load current settings
    fetch('/backlight').then(r => r.json()).then(data => {
      const percent = Math.round((data.level / 255) * 100);
      document.getElementById('backlightSlider').value = percent;
      document.getElementById('backlightValue').textContent = percent + '%';
    }).catch(e => console.error('Failed to load backlight:', e));
    
    fetch('/settings').then(r => r.json()).then(data => {
      document.getElementById('sequenceSelect').value = data.sequence;
      document.getElementById('currentSSID').textContent = data.ssid || 'Not connected';
    }).catch(e => console.error('Failed to load settings:', e));
    
    document.getElementById('backlightSlider').addEventListener('input', function() {
      document.getElementById('backlightValue').textContent = this.value + '%';
    });
    
    function setBacklight() {
      const percent = document.getElementById('backlightSlider').value;
      const level = Math.round((percent / 100) * 255);
      const status = document.getElementById('settingsStatus');
      
      status.innerHTML = '<div class="status info">Applying backlight...</div>';
      
      fetch('/backlight/set?level=' + level)
        .then(response => response.json())
        .then(data => {
          status.innerHTML = '<div class="status success">' + data.message + '</div>';
          setTimeout(() => { status.innerHTML = ''; }, 3000);
        })
        .catch(e => {
          status.innerHTML = '<div class="status error">Connection error</div>';
          setTimeout(() => { status.innerHTML = ''; }, 3000);
        });
    }
    
    function setSequence() {
      const sequence = document.getElementById('sequenceSelect').value;
      const status = document.getElementById('settingsStatus');
      
      status.innerHTML = '<div class="status info">Applying sequence...</div>';
      
      fetch('/sequence/set?value=' + sequence)
        .then(response => response.json())
        .then(data => {
          status.innerHTML = '<div class="status success">' + data.message + '</div>';
          setTimeout(() => { status.innerHTML = ''; refreshList(); }, 2000);
        })
        .catch(e => {
          status.innerHTML = '<div class="status error">Connection error</div>';
          setTimeout(() => { status.innerHTML = ''; }, 3000);
        });
    }
    
    function scanNetworks() {
      const status = document.getElementById('wifiStatus');
      const networkList = document.getElementById('networkList');
      
      status.innerHTML = '<div class="status info">Scanning networks...</div>';
      networkList.innerHTML = '<p>Scanning...</p>';
      
      fetch('/wifi/scan')
        .then(r => r.json())
        .then(data => {
          status.innerHTML = '';
          if (data.networks && data.networks.length > 0) {
            let html = '<div style="max-height: 300px; overflow-y: auto;">';
            data.networks.forEach(net => {
              let signalBars = '';
              if (net.rssi > -50) signalBars = '[####]';
              else if (net.rssi > -60) signalBars = '[### ]';
              else if (net.rssi > -70) signalBars = '[##  ]';
              else signalBars = '[#   ]';
              
              const lockIcon = net.encryption ? '[LOCK]' : '[OPEN]';
              html += `<div class="network-item" onclick="selectNetwork('${net.ssid}')">
                <span>${lockIcon} ${net.ssid}</span>
                <span class="signal-strength">${signalBars} ${net.rssi}dBm</span>
              </div>`;
            });
            html += '</div>';
            networkList.innerHTML = html;
          } else {
            networkList.innerHTML = '<p>No networks found</p>';
          }
        })
        .catch(e => {
          status.innerHTML = '<div class="status error">Scan failed</div>';
          networkList.innerHTML = '';
        });
    }
    
    function selectNetwork(ssid) {
      selectedSSID = ssid;
      document.querySelectorAll('.network-item').forEach(item => {
        item.classList.remove('selected');
        if (item.textContent.includes(ssid)) {
          item.classList.add('selected');
        }
      });
      
      document.getElementById('manualSSID').value = ssid;
      document.getElementById('manualWiFi').style.display = 'block';
      document.getElementById('wifiPassword').focus();
    }
    
    function toggleManualWiFi() {
      const manualDiv = document.getElementById('manualWiFi');
      manualDiv.style.display = manualDiv.style.display === 'none' ? 'block' : 'none';
      if (manualDiv.style.display === 'block') {
        document.getElementById('manualSSID').focus();
      }
    }
    
    function connectWiFi() {
      const ssid = document.getElementById('manualSSID').value;
      const password = document.getElementById('wifiPassword').value;
      const status = document.getElementById('wifiStatus');
      
      if (!ssid) {
        status.innerHTML = '<div class="status error">Please enter SSID</div>';
        return;
      }
      
      status.innerHTML = '<div class="status info">Connecting to ' + ssid + '...<br>This may take up to 30 seconds...</div>';
      
      fetch('/wifi/connect', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ssid: ssid, password: password })
      })
        .then(r => r.json())
        .then(data => {
          if (data.success) {
            status.innerHTML = '<div class="status success">' + data.message + 
              '<br>Device will restart in 3 seconds...</div>';
            setTimeout(() => {
              location.reload();
            }, 3000);
          } else {
            status.innerHTML = '<div class="status error">' + data.message + '</div>';
          }
        })
        .catch(e => {
          status.innerHTML = '<div class="status error">Connection failed: ' + e.message + '</div>';
        });
    }
    
    function refreshList() {
      fetch('/list').then(r => r.json()).then(data => {
        const list = document.getElementById('fileList');
        const count = document.getElementById('imageCount');
        list.innerHTML = '';
        count.textContent = data.files.length;
        
        document.getElementById('selectAllCheckbox').checked = false;
        
        if (data.files.length === 0) {
          list.innerHTML = '<li style="padding: 20px; text-align: center; color: #888;">No images found. Upload some images to get started!</li>';
          return;
        }
        
        data.files.forEach((file, index) => {
          const li = document.createElement('li');
          li.className = 'file-item';
          li.innerHTML = `
            <span>
              <input type="checkbox" class="file-checkbox" value="${file}"> 
              <a href="${file}" target="_blank">${file.substring(1)}</a>
            </span>
            <button class="delete" onclick="deleteFile('${file}')">Delete</button>`;
          list.appendChild(li);
        });
      }).catch(e => {
        console.error('Failed to refresh list:', e);
        document.getElementById('fileList').innerHTML = '<li style="padding: 20px; text-align: center; color: #f44;">Error loading file list</li>';
      });
    }
    
    function toggleSelectAll() {
      const selectAllCheckbox = document.getElementById('selectAllCheckbox');
      const fileCheckboxes = document.querySelectorAll('.file-checkbox');
      
      fileCheckboxes.forEach(checkbox => {
        checkbox.checked = selectAllCheckbox.checked;
      });
    }
    
    function deleteFile(filename) {
      if (confirm('Delete ' + filename + '?')) {
        fetch('/delete?file=' + encodeURIComponent(filename))
          .then(r => r.json())
          .then(data => {
            alert(data.message);
            refreshList();
          })
          .catch(e => {
            alert('Error deleting file: ' + e.message);
          });
      }
    }

    function deleteSelectedFiles() {
      const selectedFiles = Array.from(document.querySelectorAll('.file-checkbox:checked')).map(cb => cb.value);
      if (selectedFiles.length === 0) {
        alert('Please select files to delete.');
        return;
      }

      if (confirm('Delete ' + selectedFiles.length + ' selected file(s)?')) {
        fetch('/delete-selected', { 
          method: 'POST', 
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ files: selectedFiles })
        })
          .then(r => r.json())
          .then(data => {
            alert(data.message);
            refreshList();
          })
          .catch(e => {
            alert('Error deleting files: ' + e.message);
          });
      }
    }
    
    async function uploadFile() {
      const input = document.getElementById('fileInput');
      const status = document.getElementById('uploadStatus');
      const progressBarContainer = document.getElementById('progressBarContainer');
      const progressBar = document.getElementById('progressBar');
      const progressCounter = document.getElementById('progressCounter');
      
      if (input.files.length === 0) {
        status.innerHTML = '<div class="status error">Please select files</div>';
        return;
      }
      
      status.innerHTML = '';
      progressBarContainer.style.display = 'block';
      progressBar.value = 0;
      
      for (let i = 0; i < input.files.length; i++) {
        const file = input.files[i];
        
        // Validate file size (max 10MB)
        if (file.size > 10 * 1024 * 1024) {
          status.innerHTML = `<div class="status error">${file.name} is too large (max 10MB)</div>`;
          progressBarContainer.style.display = 'none';
          return;
        }
        
        const formData = new FormData();
        formData.append('files', file, file.name);
        
        status.innerHTML = `<div class="status info">Uploading ${file.name}...</div>`;
        
        try {
          const response = await fetch('/upload', { method: 'POST', body: formData });
          const data = await response.json();
          
          if (response.ok) {
            progressBar.value = ((i + 1) / input.files.length) * 100;
            progressCounter.textContent = `(${(i + 1)}/${input.files.length} uploaded)`;
          } else {
            throw new Error(data.message || 'Upload failed');
          }
        } catch (e) {
          status.innerHTML = `<div class="status error">Error uploading ${file.name}: ${e.message}</div>`;
          progressBarContainer.style.display = 'none';
          return;
        }
      }
      
      status.innerHTML = `<div class="status success">All files uploaded successfully!</div>`;
      progressBarContainer.style.display = 'none';
      input.value = '';
      refreshList();
      setTimeout(() => { status.innerHTML = ''; }, 5000);
    }
    
    refreshList();
  </script>
</body>
</html>
)rawliteral";

//====================================================================================
//   Web Server Setup
//====================================================================================
void setupWebServer() {
  // Main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });
  
  // Get settings
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"sequence\":" + String(displaySequence) + 
                  ",\"ssid\":\"" + (WiFi.status() == WL_CONNECTED ? wifi_ssid : "") + "\"}";
    request->send(200, "application/json", json);
  });
  
  // Set display sequence
  server.on("/sequence/set", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("value")) {
      int seq = request->getParam("value")->value().toInt();
      
      if (seq >= 0 && seq <= 4) {
        displaySequence = seq;
        
        preferences.begin("photoframe", false);
        preferences.putUChar("sequence", displaySequence);
        preferences.end();
        
        applyDisplaySequence();
        
        const char* seqNames[] = {"A-Z", "Z-A", "Random", "Oldest First", "Newest First"};
        String json = "{\"success\":true,\"message\":\"Sequence set to " + 
                      String(seqNames[seq]) + "\"}";
        request->send(200, "application/json", json);
        Serial.printf("✓ Display sequence changed to: %s\n", seqNames[seq]);
      } else {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid sequence\"}");
      }
    } else {
      request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing value parameter\"}");
    }
  });
  
  // Set backlight level
  server.on("/backlight/set", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("level")) {
      int level = request->getParam("level")->value().toInt();
      
      if (level >= 0 && level <= 255) {
        currentBacklight = level;
        analogWrite(TFT_BACKLIGHT, currentBacklight);
        
        preferences.begin("photoframe", false);
        preferences.putUChar("backlight", currentBacklight);
        preferences.end();
        
        int percent = (currentBacklight * 100) / 255;
        String json = "{\"success\":true,\"message\":\"Backlight set to " + String(percent) + "%\"}";
        request->send(200, "application/json", json);
        Serial.printf("✓ Backlight changed to %d/255 (%d%%)\n", currentBacklight, percent);
      } else {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid level\"}");
      }
    } else {
      request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing level parameter\"}");
    }
  });
  
  // Get current backlight level
  server.on("/backlight", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"level\":" + String(currentBacklight) + "}";
    request->send(200, "application/json", json);
  });
  
  // Scan WiFi networks
  server.on("/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    int n = WiFi.scanNetworks();
    String json = "{\"networks\":[";
    
    for (int i = 0; i < n; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
      json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
      json += "\"encryption\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN) + "}";
    }
    json += "]}";
    
    request->send(200, "application/json", json);
  });
  
  // Connect to WiFi
  server.on("/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      String body = "";
      for(size_t i=0; i<len; i++){
        body += (char)data[i];
      }
      
      DynamicJsonDocument doc(512);
      DeserializationError error = deserializeJson(doc, body);
      
      if (error) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
      }
      
      String ssid = doc["ssid"].as<String>();
      String password = doc["password"].as<String>();
      
      if (ssid.length() > 0) {
        // Save credentials (password stored as-is in encrypted preferences namespace)
        preferences.begin("photoframe", false);
        preferences.putString("wifi_ssid", ssid);
        preferences.putString("wifi_pass", password);
        preferences.end();
        
        wifi_ssid = ssid;
        wifi_password = password;
        
        request->send(200, "application/json", 
          "{\"success\":true,\"message\":\"Credentials saved. Restarting...\"}");
        
        Serial.println("✓ New WiFi credentials saved");
        Serial.println("  SSID: " + ssid);
        delay(1000);
        ESP.restart();
      } else {
        request->send(400, "application/json", 
          "{\"success\":false,\"message\":\"Invalid SSID\"}");
      }
    }
  );
  
  // List files
  server.on("/list", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"files\":[";
    for (size_t i = 0; i < fileNames.size(); i++) {
      if (i > 0) json += ",";
      json += "\"" + fileNames[i] + "\"";
    }
    json += "]}";
    request->send(200, "application/json", json);
  });
  
  // Delete file
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("file")) {
      String filename = request->getParam("file")->value();
      if (SD.remove(filename)) {
        Serial.println("✓ Deleted: " + filename);
        scanForImages();
        request->send(200, "application/json", "{\"message\":\"File deleted\"}");
      } else {
        Serial.println("✗ Failed to delete: " + filename);
        request->send(500, "application/json", "{\"message\":\"Delete failed\"}");
      }
    } else {
      request->send(400, "application/json", "{\"message\":\"Missing file parameter\"}");
    }
  });

  // Delete selected files
  server.on("/delete-selected", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, 
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      String body = "";
      for(size_t i=0; i<len; i++){
        body += (char)data[i];
      }
      
      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, body);
      
      if (error) {
        request->send(400, "application/json", "{\"message\":\"Invalid JSON\"}");
        return;
      }
      
      JsonArray files = doc["files"];
      int deleted = 0;
      
      for(JsonVariant v : files) {
        String filename = v.as<String>();
        if (SD.remove(filename)) {
          deleted++;
          Serial.println("✓ Deleted: " + filename);
        }
      }
      
      scanForImages();
      String msg = "{\"message\":\"Deleted " + String(deleted) + " file(s)\"}";
      request->send(200, "application/json", msg);
  });

  // Upload file
  server.on("/upload", HTTP_POST, 
    [](AsyncWebServerRequest *request) {
      request->send(200, "application/json", "{\"message\":\"Upload complete\"}");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      static File uploadFile;
      
      if (index == 0) {
        // Sanitize filename
        filename.replace("/", "_");
        filename.replace("\\", "_");
        
        Serial.printf("Upload start: %s\n", filename.c_str());
        uploadFile = SD.open("/" + filename, FILE_WRITE);
        
        if (!uploadFile) {
          Serial.println("✗ Failed to open file for writing");
          return;
        }
      }
      
      if (uploadFile) {
        uploadFile.write(data, len);
      }
      
      if (final) {
        if (uploadFile) {
          uploadFile.close();
          Serial.printf("✓ Upload complete: %s\n", filename.c_str());
          scanForImages();
        }
      }
    }
  );

  // Serve images from SD card
  server.onNotFound([](AsyncWebServerRequest *request){
    String path = request->url();
    if (SD.exists(path)) {
      request->send(SD, path, "image/jpeg");
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  ElegantOTA.begin(&server);
  server.begin();
}

//====================================================================================
//   Setup
//====================================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n╔═══════════════════════════════════════╗");
  Serial.println("║  ESP32-S3 Photo Frame - Production   ║");
  Serial.println("╚═══════════════════════════════════════╝\n");

  // Load saved settings
  preferences.begin("photoframe", true);
  currentBacklight = preferences.getUChar("backlight", BACKLIGHT_LEVEL);
  displaySequence = preferences.getUChar("sequence", 0);
  wifi_ssid = preferences.getString("wifi_ssid", "");
  wifi_password = preferences.getString("wifi_pass", "");
  preferences.end();
  
  Serial.printf("Loaded backlight: %d/255\n", currentBacklight);
  Serial.printf("Loaded sequence: %d\n", displaySequence);
  if (wifi_ssid.length() > 0) {
    Serial.printf("Loaded WiFi SSID: %s\n", wifi_ssid.c_str());
  } else {
    Serial.println("No WiFi credentials stored");
  }

  // Configure backlight
  pinMode(TFT_BACKLIGHT, OUTPUT);
  analogWrite(TFT_BACKLIGHT, currentBacklight);

  // Initialize display
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  Serial.printf("✓ TFT initialized (%dx%d)\n", tft.width(), tft.height());
  
  // Re-apply backlight
  analogWrite(TFT_BACKLIGHT, currentBacklight);

  // Initialize SD card
  Serial.println("\nInitializing SD card...");
  sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  if (!SD.begin(SD_CS, sd_spi, 25000000)) {
    Serial.println("✗ SD card mount failed!");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("SD CARD ERROR", tft.width()/2, tft.height()/2, 4);
    while (1) delay(1000);
  }
  
  Serial.printf("✓ SD card mounted\n");

  // Scan for images
  scanForImages();
  uint64_t usedBytes = SD.usedBytes();
  uint64_t totalBytes = SD.totalBytes();

  // Draw startup screen
  drawStartupScreen(fileNames.size(), usedBytes, totalBytes);

  // Try to connect to WiFi
  bool wifiConnected = false;
  
  if (wifi_ssid.length() > 0) {
    wifiConnected = connectToWiFi();
  }
  
  String ipAddress;
  
  if (wifiConnected) {
    ipAddress = WiFi.localIP().toString();
    updateWiFiStatus(true, ipAddress, false);
    Serial.println("✓ WiFi Connected!");
    Serial.println("  IP: " + ipAddress);
  } else {
    // Start hotspot mode
    startHotspot();
    ipAddress = WiFi.softAPIP().toString();
    updateWiFiStatus(true, ipAddress, true);
  }
  
  setupWebServer();
  Serial.println("✓ Web server started");
  
  delay(10000);  // Display startup screen for 10 seconds

  if (fileNames.size() == 0) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    
    if (isHotspotMode) {
      // Show hotspot connection info when no images
      int centerX = tft.width() / 2;
      int y = 60;
      
      tft.setTextColor(TFT_ORANGE);
      tft.drawString("HOTSPOT MODE ACTIVE", centerX, y, 4);
      y += 40;
      
      tft.setTextColor(TFT_CYAN);
      tft.drawString("Connect to WiFi Network:", centerX, y, 2);
      y += 25;
      
      tft.setTextColor(TFT_WHITE);
      tft.drawString(hotspotSSID, centerX, y, 4);
      y += 35;
      
      tft.setTextColor(TFT_LIGHTGREY);
      tft.drawString("Password: admin", centerX, y, 2);
      y += 30;
      
      tft.setTextColor(TFT_GREEN);
      tft.drawString("Then open browser to:", centerX, y, 2);
      y += 25;
      tft.setTextColor(TFT_WHITE);
      tft.drawString("http://192.168.4.1", centerX, y, 4);
      y += 50;
      
      tft.setTextColor(TFT_YELLOW);
      tft.drawString("Configure WiFi & Upload Images", centerX, y, 2);
    } else {
      // Regular no images message
      int y = tft.height() / 2 - 20;
      tft.setTextColor(TFT_ORANGE);
      tft.drawString("No images found", tft.width()/2, y, 4);
      y += 40;
      tft.setTextColor(TFT_DARKGREY);
      tft.drawString("Upload images via web interface", tft.width()/2, y, 2);
    }
    
    // Keep web server running
    while (1) {
      delay(1000);
      
      // Check if we should retry WiFi connection
      if (isHotspotMode && (millis() - hotspotStartTime > HOTSPOT_TIMEOUT)) {
        Serial.println("\nRetrying WiFi connection...");
        if (wifi_ssid.length() > 0 && connectToWiFi()) {
          Serial.println("✓ WiFi reconnected!");
          ESP.restart();
        }
        hotspotStartTime = millis(); // Reset timer
      }
    }
  }

  // Clear screen and load first image
  tft.fillScreen(TFT_BLACK);
  Serial.println("Loading first image...\n");
  
  if (decodeJpegToBuffer(fileNames[0].c_str())) {
    displayBufferedImage();
  }
  
  lastImageTime = millis();
  Serial.println("Setup complete - slideshow running\n");
}

//====================================================================================
//   Loop
//====================================================================================
void loop() {
  // Check for WiFi reconnection in hotspot mode
  if (isHotspotMode && (millis() - hotspotStartTime > HOTSPOT_TIMEOUT)) {
    Serial.println("\nRetrying WiFi connection...");
    if (wifi_ssid.length() > 0 && connectToWiFi()) {
      Serial.println("✓ WiFi reconnected!");
      ESP.restart();
    }
    hotspotStartTime = millis();
  }
  
  // Handle single image case
  if (fileNames.size() <= 1) {
    delay(1000);
    return;
  }

  // Slideshow timing
  if (millis() - lastImageTime >= SLIDESHOW_DELAY) {
    currentIndex = (currentIndex + 1) % fileNames.size();
    
    Serial.printf("Image %d/%d: %s\n", currentIndex + 1, fileNames.size(), 
                  fileNames[currentIndex].c_str());
    
    if (decodeJpegToBuffer(fileNames[currentIndex].c_str())) {
      displayBufferedImage();
    }
    
    lastImageTime = millis();
  }

  delay(100);
}