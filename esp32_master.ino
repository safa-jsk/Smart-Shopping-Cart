#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include "HX711.h"
#include <SPI.h>
#include <MFRC522.h>
#include <MPU6050_light.h>

// --- System Pins ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
#define RXD2 16
#define TXD2 17

#define REMOVE_BTN 25
#define RESET_BTN 33
#define CHECKOUT_BTN 32

// --- Scale Setup ---
const int LOADCELL_DOUT_PIN = 22;
const int LOADCELL_SCK_PIN = 23;
HX711 scale;
float calibration_factor = 16.6;
int currentWeight = 0;

// --- BLE Tracker ---
String targetDeviceName = "CART_REMOTE";
BLEScan *pBLEScan;
bool targetFound = false;
int globalRSSI = -100;

// --- RFID Database ---
#define RST_PIN 26
#define SS_PIN 27
MFRC522 mfrc522(SS_PIN, RST_PIN);

struct Product
{
  String uid;
  String name;
  float price;
  float expectedWeight;
};

Product productDatabase[5] = {
    {"AD8C9002", "Coke Can", 45.00, 350.0},
    {"8A822A03", "Lays Chips", 25.00, 50.0},
    {"3A728F02", "Notebook", 120.00, 200.0},
    {"8A1C9002", "Gel Pen", 15.00, 15.0},
    {"AC278F02", "Water Bottle", 20.00, 500.0}};

// --- MPU6050 Setup (Bus 2) ---
TwoWire I2CMPU = TwoWire(1);
MPU6050 mpu(I2CMPU);

// --- Cart POS State Machine ---
float cartTotal = 0.00;
int itemCount = 0;
bool checkoutMode = false;
bool removeNextItem = false;
bool cartTipped = false;

// --- Bluetooth Callback ---
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    if (advertisedDevice.haveName())
    {
      if (advertisedDevice.getName() == targetDeviceName.c_str())
      {
        targetFound = true;
        globalRSSI = advertisedDevice.getRSSI();
      }
    }
  }
};

// --- Helper to safely halt the cart during UI popups ---
void haltCartForUI()
{
  globalRSSI = -100;
  Serial2.println("<-100>");
}

void setup()
{
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);

  pinMode(REMOVE_BTN, INPUT_PULLUP);
  pinMode(RESET_BTN, INPUT_PULLUP);
  pinMode(CHECKOUT_BTN, INPUT_PULLUP);

  Wire.begin(18, 19);
  lcd.begin(); // Updated as requested!
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Cart POS");
  lcd.setCursor(0, 1);
  lcd.print("System Check...");

  I2CMPU.begin(21, 15, 100000);
  byte status = mpu.begin();
  if (status != 0)
  {
    Serial.println("MPU6050 not found!");
  }
  else
  {
    lcd.setCursor(0, 1);
    lcd.print("Calibrating IMU ");
    delay(1000);
    mpu.calcOffsets(true, true);
  }

  // SPI for RFID (MOSI on D5)
  SPI.begin(14, 13, 5, 27);
  mfrc522.PCD_Init();

  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(calibration_factor);
  scale.tare();

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);

  delay(1500);
  lcd.clear();
}

void loop()
{
  // ==========================================
  // 0. SAFETY FIRST: MPU6050 Anti-Tip Check
  // ==========================================
  mpu.update();

  if (abs(mpu.getAngleX()) > 35.0 || abs(mpu.getAngleY()) > 35.0)
  {
    cartTipped = true;
    haltCartForUI(); // Stop motors instantly

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("! WARNING !");
    lcd.setCursor(0, 1);
    lcd.print("CART TIPPED OVER");
    delay(500);
    return;
  }
  else if (cartTipped)
  {
    cartTipped = false;
    lcd.clear();
  }

  // ==========================================
  // 1. BUTTON LOGIC
  // ==========================================
  if (digitalRead(CHECKOUT_BTN) == LOW && cartTotal > 0)
  {
    haltCartForUI();
    checkoutMode = true;
    removeNextItem = false;
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Items: ");
    lcd.print(itemCount);
    lcd.print(" Tk:");
    lcd.print(cartTotal, 0);
    lcd.setCursor(0, 1);
    lcd.print("Tap Credit Card!");
    delay(500);
  }

  if (digitalRead(REMOVE_BTN) == LOW && !checkoutMode)
  {
    haltCartForUI();
    removeNextItem = true;
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Remove Mode:");
    lcd.setCursor(0, 1);
    lcd.print("Scan to Subtract");
    delay(1000);
    lcd.clear();
  }

  if (digitalRead(RESET_BTN) == LOW)
  {
    haltCartForUI();
    checkoutMode = false;
    removeNextItem = false;
    cartTotal = 0.00;
    itemCount = 0;
    scale.tare();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(" Cart Emptied!  ");
    delay(1500);
    lcd.clear();
  }

  // ==========================================
  // 2. POS RFID LOGIC
  // ==========================================
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial())
  {
    haltCartForUI(); // Prevent movement while processing scan

    String scannedUID = "";
    for (byte i = 0; i < mfrc522.uid.size; i++)
    {
      scannedUID += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
      scannedUID += String(mfrc522.uid.uidByte[i], HEX);
    }
    scannedUID.toUpperCase();

    int foundIndex = -1;
    for (int i = 0; i < 5; i++)
    {
      if (scannedUID == productDatabase[i].uid)
      {
        foundIndex = i;
        break;
      }
    }

    if (checkoutMode)
    {
      if (foundIndex == -1)
      {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Processing...");
        delay(1500);
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("   PAYMENT OK!  ");
        lcd.setCursor(0, 1);
        lcd.print("   Thank You!   ");
        delay(3000);

        cartTotal = 0.00;
        itemCount = 0;
        checkoutMode = false;
        scale.tare();
        lcd.clear();
      }
      else
      {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Error: Use");
        lcd.setCursor(0, 1);
        lcd.print("Credit Card!");
        delay(1500);
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Items: ");
        lcd.print(itemCount);
        lcd.print(" Tk:");
        lcd.print(cartTotal, 0);
        lcd.setCursor(0, 1);
        lcd.print("Tap Credit Card!");
      }
    }
    else
    {
      if (foundIndex != -1)
      {
        lcd.clear();
        if (removeNextItem)
        {
          cartTotal -= productDatabase[foundIndex].price;
          itemCount--;
          if (cartTotal < 0)
            cartTotal = 0;
          if (itemCount < 0)
            itemCount = 0;
          lcd.setCursor(0, 0);
          lcd.print("Voided: ");
          lcd.setCursor(0, 1);
          lcd.print(productDatabase[foundIndex].name);
          removeNextItem = false;
        }
        else
        {
          cartTotal += productDatabase[foundIndex].price;
          itemCount++;
          lcd.setCursor(0, 0);
          lcd.print("Added: ");
          lcd.setCursor(0, 1);
          lcd.print(productDatabase[foundIndex].name);
        }
        delay(1500);
        lcd.clear();
      }
      else
      {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(" Unknown Item! ");
        delay(1500);
        lcd.clear();
      }
    }
    mfrc522.PICC_HaltA();
  }

  // ==========================================
  // 3. TELEMETRY & MOVEMENT LOGIC
  // ==========================================
  if (checkoutMode || removeNextItem)
    return;

  if (scale.is_ready())
  {
    currentWeight = round(scale.get_units(1));
    if (currentWeight < 0)
      currentWeight = 0;
  }

  targetFound = false;
  pBLEScan->start(1, false);
  pBLEScan->clearResults();

  if (!targetFound)
  {
    globalRSSI = -100;
  }

  // Command UNO over UART
  Serial2.print("<");
  Serial2.print(globalRSSI);
  Serial2.println(">");

  // 4. Update the Standard UI Screen
  char topRow[17], bottomRow[17];

  // Fit: "Tk:XXXX W:XXX C:XX"
  sprintf(topRow, "Tk:%.0f W:%d C:%d", cartTotal, currentWeight, itemCount);

  if (globalRSSI == -100)
  {
    sprintf(bottomRow, "Target Lost!    ");
  }
  else if (globalRSSI < -85)
  {
    sprintf(bottomRow, "Device Too Far! ");
  }
  else
  {
    sprintf(bottomRow, "Sig: %-4d dBm   ", globalRSSI);
  }

  // Ensure the string fits exactly 16 characters to overwrite old artifacts
  String tRow = String(topRow);
  while (tRow.length() < 16)
    tRow += " ";

  lcd.setCursor(0, 0);
  lcd.print(tRow);
  lcd.setCursor(0, 1);
  lcd.print(bottomRow);
}