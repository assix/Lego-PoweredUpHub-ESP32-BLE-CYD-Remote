#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <XPT2046_Touchscreen.h>
#include <BLEDevice.h>

// --- Pins ---
#define TFT_BL   21
#define TFT_DC    2
#define TFT_CS   15
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_MISO 12
#define TFT_RST  -1

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// --- Colors ---
#define COLOR_BG      ST77XX_BLACK
#define COLOR_INACTIVE 0x2124 
#define COLOR_ACTIVE_M ST77XX_CYAN
#define COLOR_FWD     ST77XX_GREEN
#define COLOR_STOP    ST77XX_RED
#define COLOR_RWD     ST77XX_ORANGE

// --- Hardware Objects ---
SPIClass tftSPI(VSPI);
Adafruit_ST7789 tft(&tftSPI, TFT_CS, TFT_DC, TFT_RST);
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

// --- LEGO BLE Variables ---
static BLEUUID serviceUUID("00001623-1212-efde-1623-785feabcd123");
static BLEUUID charUUID("00001624-1212-efde-1623-785feabcd123");

BLEScan* pBLEScan;
BLEAdvertisedDevice* myDevice;
BLERemoteCharacteristic* pRemoteCharacteristic;

bool doConnect = false;
bool connected = false;
unsigned long lastTouchTime = 0;
unsigned long lastSliderSend = 0;

// Battery One-Time Fetch
bool batteryFetched = false;
unsigned long lastBatReq = 0;

// --- State Variables ---
int selectedMotor = 2; // 0 = A, 1 = B, 2 = Both
int activeDrive = 0;   // 1 = FWD, 0 = STOP, -1 = RWD
int targetSpeed = 50;  // 0 to 100
int lastBattery = -1;

// --- Callbacks ---
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

void drawBattery(int level) {
  if (level == lastBattery) return;
  lastBattery = level;
  
  tft.fillRect(250, 5, 60, 20, COLOR_BG);
  tft.drawRect(255, 8, 30, 14, ST77XX_WHITE);
  tft.fillRect(285, 11, 3, 8, ST77XX_WHITE);
  
  uint16_t batColor = level > 20 ? ST77XX_GREEN : ST77XX_RED;
  int fillWidth = map(level, 0, 100, 0, 26);
  tft.fillRect(257, 10, fillWidth, 10, batColor);
  
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(292, 11);
  tft.print(level); tft.print("%");
}

static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  // Only process if we haven't successfully fetched a valid reading yet
  if (!batteryFetched && length >= 6 && pData[2] == 0x01 && pData[3] == 0x06 && pData[4] == 0x06) {
    int batLevel = pData[5];
    // Must be >= 5% to be considered a true reading (avoids 0% motor sag readings)
    if (batLevel >= 5) {
      drawBattery(batLevel);
      batteryFetched = true;
    }
  }
}

void drawMotorSelection() {
  tft.setTextSize(2);
  
  tft.fillRoundRect(10, 35, 90, 50, 8, selectedMotor == 0 ? COLOR_ACTIVE_M : COLOR_INACTIVE);
  tft.setTextColor(selectedMotor == 0 ? ST77XX_BLACK : ST77XX_WHITE);
  tft.setCursor(25, 52); tft.print("MOT A");

  tft.fillRoundRect(115, 35, 90, 50, 8, selectedMotor == 2 ? COLOR_ACTIVE_M : COLOR_INACTIVE);
  tft.setTextColor(selectedMotor == 2 ? ST77XX_BLACK : ST77XX_WHITE);
  tft.setCursor(135, 52); tft.print("BOTH");

  tft.fillRoundRect(220, 35, 90, 50, 8, selectedMotor == 1 ? COLOR_ACTIVE_M : COLOR_INACTIVE);
  tft.setTextColor(selectedMotor == 1 ? ST77XX_BLACK : ST77XX_WHITE);
  tft.setCursor(235, 52); tft.print("MOT B");
}

void drawDriveButtons() {
  tft.setTextSize(2);

  tft.fillRoundRect(10, 170, 90, 60, 8, activeDrive == 1 ? COLOR_FWD : COLOR_INACTIVE);
  tft.setTextColor(activeDrive == 1 ? ST77XX_BLACK : ST77XX_WHITE);
  tft.setCursor(35, 192); tft.print("FWD");

  tft.fillRoundRect(115, 170, 90, 60, 8, activeDrive == 0 ? COLOR_STOP : COLOR_INACTIVE);
  tft.setTextColor(activeDrive == 0 ? ST77XX_BLACK : ST77XX_WHITE);
  tft.setCursor(135, 192); tft.print("STOP");

  tft.fillRoundRect(220, 170, 90, 60, 8, activeDrive == -1 ? COLOR_RWD : COLOR_INACTIVE);
  tft.setTextColor(activeDrive == -1 ? ST77XX_BLACK : ST77XX_WHITE);
  tft.setCursor(245, 192); tft.print("RWD");
}

void drawSlider() {
  tft.fillRect(80, 105, 160, 20, COLOR_BG); 
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  
  int textOffset = targetSpeed < 100 ? 115 : 110;
  tft.setCursor(textOffset, 105);
  tft.print("SPEED: "); tft.print(targetSpeed); tft.print("%");

  tft.fillRect(10, 130, 300, 30, COLOR_BG);
  tft.fillRoundRect(30, 140, 260, 10, 4, COLOR_INACTIVE);
  
  int fillW = map(targetSpeed, 0, 100, 0, 260);
  tft.fillRoundRect(30, 140, fillW, 10, 4, ST77XX_YELLOW);
  
  int knobX = map(targetSpeed, 0, 100, 30, 290);
  tft.fillCircle(knobX, 145, 14, ST77XX_WHITE);
}

void drawUI() {
  tft.fillScreen(COLOR_BG);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.print("Train Remote");

  drawMotorSelection();
  drawSlider();
  drawDriveButtons();
}

void updateStatus(const char* status, uint16_t color) {
  tft.fillRect(160, 5, 85, 20, COLOR_BG);
  tft.setTextColor(color);
  tft.setTextSize(1);
  tft.setCursor(160, 11);
  tft.print(status);
}

void requestBatteryUpdate() {
  if (!connected || pRemoteCharacteristic == nullptr) return;
  // Operation 0x05 = Request one-time update
  uint8_t reqBat[5] = {0x05, 0x00, 0x01, 0x06, 0x05};
  pRemoteCharacteristic->writeValue(reqBat, 5, false);
}

bool connectToHub() {
  BLEClient* pClient = BLEDevice::createClient();
  if (!pClient->connect(myDevice)) return false;
  
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) return false;
  
  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr) return false;

  if(pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
    
    // Explicitly Disable Auto-Updates (Operation 0x03)
    uint8_t disableAutoBat[5] = {0x05, 0x00, 0x01, 0x06, 0x03};
    pRemoteCharacteristic->writeValue(disableAutoBat, 5, false);
  }

  connected = true;
  updateStatus("CONNECTED", ST77XX_GREEN);
  return true;
}

void sendMotorCommand() {
  if (!connected || pRemoteCharacteristic == nullptr) return;
  
  int actualSpeed = targetSpeed * activeDrive; 
  
  if (selectedMotor == 0 || selectedMotor == 2) {
    uint8_t payloadA[8] = {0x08, 0x00, 0x81, 0x00, 0x00, 0x51, 0x00, (uint8_t)actualSpeed};
    pRemoteCharacteristic->writeValue(payloadA, sizeof(payloadA), false);
    delay(20); 
  }
  
  if (selectedMotor == 1 || selectedMotor == 2) {
    uint8_t payloadB[8] = {0x08, 0x00, 0x81, 0x01, 0x00, 0x51, 0x00, (uint8_t)actualSpeed};
    pRemoteCharacteristic->writeValue(payloadB, sizeof(payloadB), false);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tftSPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setRotation(3);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);

  drawUI();
  updateStatus("SCANNING...", ST77XX_YELLOW);

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->start(0, false); 
}

void loop() {
  if (doConnect) {
    connectToHub();
    doConnect = false;
  }

  // Fetch battery continuously until a valid reading >= 5% is secured
  if (connected && !batteryFetched && (millis() - lastBatReq > 1000)) {
    requestBatteryUpdate();
    lastBatReq = millis();
  }

  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    
    int x = map(p.x, 200, 3900, 0, 320);
    int y = map(p.y, 400, 3600, 0, 240);
    x = constrain(x, 0, 320);
    y = constrain(y, 0, 240);

    // 1. Slider Area (Y: 95 to 160)
    if (y > 95 && y < 160) {
      if (millis() - lastSliderSend > 100) { 
        targetSpeed = map(constrain(x, 30, 290), 30, 290, 0, 100);
        targetSpeed = (targetSpeed / 5) * 5; 
        
        // Snap-to-center logic: If pressed near the middle, lock to 50%
        if (targetSpeed >= 45 && targetSpeed <= 55) {
          targetSpeed = 50;
        }

        drawSlider();
        if (activeDrive != 0) sendMotorCommand(); 
        
        lastSliderSend = millis();
        lastTouchTime = millis();
      }
    }
    
    // 2. Button Areas
    else if (millis() - lastTouchTime > 300) {
      
      // Motor Selection (Y: 35 to 85)
      if (y > 35 && y < 85) {
        if (x > 10 && x < 100)       selectedMotor = 0;
        else if (x > 115 && x < 205) selectedMotor = 2;
        else if (x > 220 && x < 310) selectedMotor = 1;
        
        drawMotorSelection();
        if (activeDrive != 0) sendMotorCommand(); 
      }

      // Drive Controls (Y: 170 to 230)
      else if (y > 170 && y < 230) {
        if (x > 10 && x < 100) {
          activeDrive = 1; 
        }
        else if (x > 115 && x < 205) {
          activeDrive = 0;  
        }
        else if (x > 220 && x < 310) {
          activeDrive = -1; 
        }
        
        drawDriveButtons();
        sendMotorCommand();
      }
      lastTouchTime = millis();
    }
  }
}