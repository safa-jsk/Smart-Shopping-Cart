#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <HX711.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#if defined(__INTELLISENSE__)
#include <HardwareSerial.h>
extern HardwareSerial Serial2;
#endif

// --- Pin Definitions ---
// Communication with UNO
#define RXD2 16
#define TXD2 17

// RFID RC522
#define RST_PIN 4
#define SS_PIN 5

// HX711 Load Cell
#define LOADCELL_DOUT_PIN 26
#define LOADCELL_SCK_PIN 27

// UI & Alerts
#define BUZZER 15
#define LED_RED 32
#define LED_GREEN 33
#define LED_BLUE 0 // Blue status LED (GPIO0 — safe for output after boot)

// Button Control Panel
#define BTN_START 2  // Start shopping / lock-on
#define BTN_STOP 25  // Emergency stop (will be mapped to trigger alarm)
#define BTN_TARE 12  // Tare scale (zero weight)
#define BTN_MODE 13  // Toggle mode (manual/follow)
#define BTN_ESTOP 14 // Hard emergency stop

// --- Object Initialization ---
LiquidCrystal_I2C lcd(0x27, 16, 2); // Adjust I2C address if needed in Wokwi
MFRC522 mfrc522(SS_PIN, RST_PIN);
HX711 scale;

// Variables for Cart Status
float totalWeight = 0.0;
float totalCost = 0.0;
int totalItems = 0;
bool isFollowing = false;

// --- Bluetooth & Follow State Machine ---
enum CartState
{
  IDLE,      // Waiting for lock-on
  LOCKED,    // User paired, starting follow
  FOLLOWING, // Active follow mode
  LOST       // User signal lost
};

CartState currentState = IDLE;

// Bluetooth Configuration
struct BluetoothTarget
{
  String deviceName = "TARGET_PHONE"; // Target phone name (change as needed)
  String deviceMAC = "";              // Will store MAC of locked device
  int minLockRSSI = -70;              // RSSI threshold to consider "in range"
  int maxLossTime = 5000;             // ms before marking user as lost
  unsigned long lastSignalTime = 0;
} btTarget;

// RSSI-based Speed Control
struct RSSIBands
{
  int veryClose = -60; // Distance: very close (<1m)
  int close = -70;     // Distance: close (1-2m)
  int medium = -80;    // Distance: medium (2-4m)
  int far = -90;       // Distance: far (4+ m)
} rssiThresholds;

struct FollowSpeeds
{
  int veryCloseSpeed = 20; // Slow: user too close
  int closeSpeed = 80;     // Moderate speed
  int mediumSpeed = 150;   // Normal follow speed
  int farSpeed = 200;      // Fast: catch up
  int stopSpeed = 0;
} speeds;

// Bluetooth scan and RSSI tracking
int currentRSSI = 0;
bool deviceFound = false;

// --- Button State Management ---
struct ButtonState
{
  bool startPressed = false;
  bool tarePressed = false;
  bool modePressed = false;
  unsigned long lastDebounceTime = 0;
  const int DEBOUNCE_DELAY = 200; // ms debounce
} btnState;

// --- RFID Item Database ---
struct RFIDItem
{
  uint8_t uid[4];       // RFID UID (4 bytes - adjust if using longer UIDs)
  String itemName;      // Product name
  float price;          // Price in USD
  float expectedWeight; // Optional expected weight (grams)
};

// Database of registered RFID cards with product information
// Add your actual card UIDs here from calibration/registration
const int MAX_ITEMS = 10;
RFIDItem rfidDatabase[MAX_ITEMS] = {
    // UID bytes (adjust to match your actual cards), Name, Price, Expected Weight
    {.uid = {0xAA, 0xBB, 0xCC, 0xDD}, .itemName = "Apple (Pack)", .price = 3.99, .expectedWeight = 200.0},
    {.uid = {0x11, 0x22, 0x33, 0x44}, .itemName = "Bread", .price = 2.49, .expectedWeight = 500.0},
    {.uid = {0x55, 0x66, 0x77, 0x88}, .itemName = "Milk (1L)", .price = 3.50, .expectedWeight = 1020.0},
    {.uid = {0x99, 0xAA, 0xBB, 0xCC}, .itemName = "Cheese", .price = 5.99, .expectedWeight = 250.0},
    {.uid = {0xDD, 0xEE, 0xFF, 0x00}, .itemName = "Orange Juice", .price = 4.29, .expectedWeight = 480.0},
};
int registeredItemCount = 5; // Update this when adding items
unsigned long lastRSSIUpdate = 0;

// --- Cart Weight Limit ---
const float MAX_WEIGHT_GRAMS = 15000.0; // 15 kg max cart load

// --- Cart Inventory (quantity tracking per unique item) ---
struct CartEntry
{
  int dbIndex; // Index into rfidDatabase
  int quantity;
};
CartEntry cartItems[MAX_ITEMS];
int cartItemCount = 0;

// --- BLE Scan Callback ---
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    // Check if this is our target device (by name or MAC)
    String deviceName = advertisedDevice.getName().c_str();
    String deviceMAC = advertisedDevice.getAddress().toString().c_str();

    // Match by name or stored MAC
    if (deviceName == btTarget.deviceName ||
        (!btTarget.deviceMAC.isEmpty() && deviceMAC == btTarget.deviceMAC))
    {

      currentRSSI = advertisedDevice.getRSSI();
      lastRSSIUpdate = millis();
      deviceFound = true;

      // Store MAC on first lock
      if (currentState == IDLE && btTarget.deviceMAC.isEmpty())
      {
        btTarget.deviceMAC = deviceMAC;
      }

      btTarget.lastSignalTime = millis();

      Serial.print("Target device found: ");
      Serial.print(deviceName.c_str());
      Serial.print(" | RSSI: ");
      Serial.println(currentRSSI);
    }
  }
};

void setup()
{
  // Initialize USB Serial for Debugging
  Serial.begin(115200);

  // Initialize UART2 for UNO Communication
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);

  // Initialize UI components
  pinMode(BUZZER, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);

  // Initialize Buttons
  pinMode(BTN_START, INPUT_PULLUP);
  pinMode(BTN_STOP, INPUT_PULLUP);
  pinMode(BTN_TARE, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BTN_ESTOP, INPUT_PULLUP);
  Serial.println("Button GPIO initialized");

  // Initialize LCD
  lcd.begin(16, 2);
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Cart Init");

  // Initialize SPI & RFID
  SPI.begin(18, 19, 23, 5); // SCK, MISO, MOSI, SS
  mfrc522.PCD_Init();

  // Initialize HX711
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(2280.f); // Calibration factor
  scale.tare();            // Reset scale to 0

  // Initialize BLE
  BLEDevice::init("");
  BLEScan *pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true); // Active scan uses more power
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  Serial.println("BLE initialized - searching for user phone...");

  delay(2000);
  updateLCD("Ready to Shop", "Press Start");
}

void loop()
{
  unsigned long loopTime = millis();
  static unsigned long lastLCDUpdateTime = 0;

  // 1. Check for RFID Scans
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial())
  {
    handleRFIDScan();
  }

  // 2. Update Weight
  if (scale.is_ready())
  {
    totalWeight = scale.get_units(5); // Average of 5 readings
  }

  // 3. Continuous BLE Scan for User Phone
  performBLEScan();

  // 4. Update Follow State Machine
  updateFollowStateMachine();

  // 5. Check Button Inputs
  checkButtonInputs();

  // 6. Periodic LCD Status Update (every 500ms for smooth display)
  if (loopTime - lastLCDUpdateTime >= 500)
  {
    lastLCDUpdateTime = loopTime;
    updateLCDWithStatus(); // Show comprehensive status
  }

  // 7. Listen for Emergency Alerts from UNO
  if (Serial2.available())
  {
    String unoMessage = Serial2.readStringUntil('\n');
    if (unoMessage.indexOf("ALERT") >= 0)
    {
      triggerAlarm("Obstacle/Tilt!");
    }
  }

  delay(100); // Small loop delay for stability
}

// --- BLUETOOTH & FOLLOW LOGIC ---

// Perform periodic BLE scan for target device
void performBLEScan()
{
  static unsigned long lastScanTime = 0;
  unsigned long now = millis();

  // Scan every 1 second
  if (now - lastScanTime >= 1000)
  {
    lastScanTime = now;
    deviceFound = false; // Reset flag before scan

    BLEScan *pBLEScan = BLEDevice::getScan();
    pBLEScan->start(1); // 1 second scan duration
  }
}

// Update the state machine based on Bluetooth signal
void updateFollowStateMachine()
{
  unsigned long now = millis();

  switch (currentState)
  {
  case IDLE:
    // Waiting for BTN_START to be pressed — see handleStartButton()
    break;

  case LOCKED:
    // Device found and locked, start following
    if (deviceFound)
    {
      Serial.println("Following user...");
      currentState = FOLLOWING;
      // LCD will update via periodic updateLCDWithStatus() call
    }
    else
    {
      // Lost signal immediately
      currentState = IDLE;
      // LCD will update via periodic updateLCDWithStatus() call
    }
    break;

  case FOLLOWING:
    if (deviceFound)
    {
      // Update last signal time
      btTarget.lastSignalTime = now;

      // Calculate speed based on RSSI and send to motor
      int motorSpeed = calculateFollowSpeed(currentRSSI);
      sendMotorCommand(motorSpeed);

      // LCD status is updated periodically by updateLCDWithStatus() in main loop
    }
    else
    {
      // Check if signal lost for too long
      if (now - btTarget.lastSignalTime > btTarget.maxLossTime)
      {
        Serial.println("User lost - entering LOST state");
        currentState = LOST;
        sendMotorCommand(0);        // Stop moving
        triggerAlarm("User Lost!"); // This updates LCD and triggers alarm
      }
    }
    break;

  case LOST:
    // User signal lost - stop and alert
    sendMotorCommand(0);

    // Try to find user again every 2 seconds
    if (deviceFound)
    {
      Serial.println("User found again!");
      currentState = FOLLOWING;
      btTarget.lastSignalTime = now;
      tone(BUZZER, 1500, 300); // Recovery sound
    }
    break;
  }
}

// Calculate motor speed based on RSSI signal strength
// RSSI ranges from -120 (very weak) to 0 (very strong)
int calculateFollowSpeed(int rssi)
{
  if (rssi >= rssiThresholds.veryClose)
  {
    // Too close - slow down to avoid collision
    return speeds.veryCloseSpeed;
  }
  else if (rssi >= rssiThresholds.close)
  {
    // Close distance - moderate speed
    return speeds.closeSpeed;
  }
  else if (rssi >= rssiThresholds.medium)
  {
    // Medium distance - normal follow speed
    return speeds.mediumSpeed;
  }
  else if (rssi >= rssiThresholds.far)
  {
    // Far distance - speed up to catch up
    return speeds.farSpeed;
  }
  else
  {
    // Very far or out of range - stop
    return speeds.stopSpeed;
  }
}

// Send motor command to UNO with dynamic speed
void sendMotorCommand(int motorSpeed)
{
  if (motorSpeed > 0)
  {
    // Move forward with calculated speed
    String command = "<MOVE," + String(motorSpeed) + "," + String(motorSpeed) + ">";
    Serial2.println(command);
  }
  else
  {
    // Stop
    Serial2.println("<STOP>");
  }
}

// Convert RFID UID bytes to hex string for comparison and logging
String getUIDString(uint8_t *uid, uint8_t uidSize)
{
  String uidStr = "";
  for (int i = 0; i < uidSize; i++)
  {
    if (uid[i] < 0x10)
      uidStr += "0"; // Add leading zero
    uidStr += String(uid[i], HEX);
  }
  return uidStr;
}

// Look up RFID item in database by UID
// Returns pointer to RFIDItem if found, NULL if not found
RFIDItem *lookupRFIDItem(uint8_t *scannedUID, uint8_t uidSize)
{
  // Check UID size - expect 4 bytes for standard RFID cards
  if (uidSize != 4)
  {
    Serial.print("WARNING: Unexpected UID size: ");
    Serial.println(uidSize);
    return NULL;
  }

  // Search database for matching UID
  for (int i = 0; i < registeredItemCount; i++)
  {
    bool match = true;
    for (int j = 0; j < 4; j++)
    {
      if (rfidDatabase[i].uid[j] != scannedUID[j])
      {
        match = false;
        break;
      }
    }
    if (match)
    {
      return &rfidDatabase[i];
    }
  }
  return NULL; // No matching item found
}

// Handle RFID card scan with database lookup and validation
void handleRFIDScan()
{
  uint8_t *uid = mfrc522.uid.uidByte;
  uint8_t uidSize = mfrc522.uid.size;

  // Log scanned UID for debugging
  String uidStr = getUIDString(uid, uidSize);
  Serial.print("Card scanned - UID: ");
  Serial.println(uidStr);

  // Look up item in database
  RFIDItem *scannedItem = lookupRFIDItem(uid, uidSize);

  if (scannedItem != NULL)
  {
    // Find this item's index in rfidDatabase
    int dbIdx = scannedItem - rfidDatabase;

    // Quantity tracking: check if already in cart
    int cartIdx = -1;
    for (int i = 0; i < cartItemCount; i++)
    {
      if (cartItems[i].dbIndex == dbIdx)
      {
        cartIdx = i;
        break;
      }
    }

    if (cartIdx >= 0)
    {
      // Item already in cart - increment quantity
      cartItems[cartIdx].quantity++;
    }
    else if (cartItemCount < MAX_ITEMS)
    {
      // New item - add entry
      cartItems[cartItemCount].dbIndex = dbIdx;
      cartItems[cartItemCount].quantity = 1;
      cartItemCount++;
      cartIdx = cartItemCount - 1;
    }

    totalItems++;
    totalCost += scannedItem->price;
    totalWeight += scannedItem->expectedWeight;

    // Success feedback: GREEN LED + high beep
    digitalWrite(LED_GREEN, HIGH);
    tone(BUZZER, 1500, 200);
    delay(100);
    digitalWrite(LED_GREEN, LOW);
    delay(100);
    tone(BUZZER, 1500, 150);
    delay(150);

    // Show item name and running quantity on LCD
    int qty = cartItems[cartIdx].quantity;
    String line1 = scannedItem->itemName.substring(0, 16);
    String line2 = "x" + String(qty) + " $" + String(scannedItem->price, 2) + " T:$" + String(totalCost, 2);
    updateLCD(line1, line2);
    delay(1500);

    // Overweight threshold check
    if (totalWeight > MAX_WEIGHT_GRAMS)
    {
      digitalWrite(LED_RED, HIGH);
      tone(BUZZER, 600, 300);
      delay(350);
      tone(BUZZER, 600, 300);
      delay(350);
      updateLCD("OVERWEIGHT!", String(totalWeight / 1000.0, 1) + "kg > 15kg max");
      delay(2000);
      digitalWrite(LED_RED, LOW);
    }

    updateLCDWithStatus();

    Serial.print("Item added: ");
    Serial.print(scannedItem->itemName);
    Serial.print(" | Qty: ");
    Serial.print(qty);
    Serial.print(" | Price: $");
    Serial.print(scannedItem->price, 2);
    Serial.print(" | Total: $");
    Serial.println(totalCost, 2);
  }
  else
  {
    // Invalid/unregistered card - reject with error feedback
    Serial.println("ERROR: Card not registered in database!");

    // Error feedback: RED LED + low beeps
    digitalWrite(LED_RED, HIGH);
    tone(BUZZER, 400, 100); // Low frequency for error
    delay(100);
    tone(BUZZER, 400, 100);
    delay(100);
    tone(BUZZER, 400, 100);
    delay(200);
    digitalWrite(LED_RED, LOW);

    // Show error on LCD
    updateLCD("Invalid Card!", "Not in database");
    delay(2000); // Show error for 2 seconds

    // Revert to comprehensive status display
    updateLCDWithStatus();
  }

  mfrc522.PICC_HaltA(); // Stop reading this card
}

void updateLCD(String line1, String line2)
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// Comprehensive LCD status display showing items, cost, weight, and mode
// Called periodically from main loop to show live cart status
void updateLCDWithStatus()
{
  String line1 = "";
  String line2 = "";

  // Format totals for display (truncate/round to fit 16-char display)
  String itemsStr = String(totalItems);
  String costStr = String(totalCost, 2);
  String weightStr = String(totalWeight / 1000.0, 1); // Convert grams to kg, 1 decimal

  // Display format depends on current state
  switch (currentState)
  {
  case IDLE:
    // Waiting/Shopping mode - show totals
    line1 = "Ready to Shop";
    line2 = "Items:" + itemsStr + " $" + costStr;
    break;

  case LOCKED:
    // User locked on - show lock confirmation
    line1 = "USER: LOCKED ON";
    line2 = "Items:" + itemsStr + " W:" + weightStr + "kg";
    break;

  case FOLLOWING:
    // Active follow mode - show follow status and RSSI
    // Format: "FOLLOW|RSSI:XXX" fits exactly 16 chars
    line1 = "FOLLOW|RSSI:" + String(currentRSSI);
    // Line 2: Show items, weight, and (if space) cost
    line2 = "I:" + itemsStr + " W:" + weightStr + "kg";
    break;

  case LOST:
    // User lost - show alert state
    line1 = "LOST! Return to";
    line2 = "cart start point";
    break;
  }

  // Write to LCD
  updateLCD(line1, line2);
}

// Alternative: Display obstacle detection state on LCD
void updateLCDObstacle(String obstacleType)
{
  String line1 = "OBSTACLE: " + obstacleType;
  String line2 = "Items:" + String(totalItems) + " W:" + String(totalWeight / 1000.0, 1) + "kg";
  updateLCD(line1, line2);
}

void triggerAlarm(String reason)
{
  currentState = LOST; // Enter lost state on alarm
  sendMotorCommand(0); // Stop immediately
  updateLCD("EMERGENCY STOP", reason);
  digitalWrite(LED_RED, HIGH);
  tone(BUZZER, 500, 1000); // Continuous tone for 1 second

  // Flash red LED
  for (int i = 0; i < 5; i++)
  {
    digitalWrite(LED_RED, HIGH);
    delay(200);
    digitalWrite(LED_RED, LOW);
    delay(200);
  }
  digitalWrite(LED_RED, LOW);
}

// --- BUTTON HANDLERS ---

void handleTareButton()
{
  if (!btnState.tarePressed && millis() - btnState.lastDebounceTime > btnState.DEBOUNCE_DELAY)
  {
    if (digitalRead(BTN_TARE) == LOW)
    {
      scale.tare();
      totalWeight = 0.0;
      tone(BUZZER, 800, 100);
      delay(100);
      tone(BUZZER, 800, 100);
      updateLCD("Weight Tared!", "Scale zeroed");
      delay(1500);
      btnState.tarePressed = true;
      btnState.lastDebounceTime = millis();
    }
  }
  if (btnState.tarePressed && digitalRead(BTN_TARE) == HIGH)
  {
    btnState.tarePressed = false;
  }
}

void handleModeButton()
{
  if (!btnState.modePressed && millis() - btnState.lastDebounceTime > btnState.DEBOUNCE_DELAY)
  {
    if (digitalRead(BTN_MODE) == LOW)
    {
      // Mode button toggles follow OFF while actively following
      if (currentState == FOLLOWING)
      {
        currentState = IDLE;
        sendMotorCommand(0);
        tone(BUZZER, 600, 150);
      }
      btnState.modePressed = true;
      btnState.lastDebounceTime = millis();
    }
  }
  if (btnState.modePressed && digitalRead(BTN_MODE) == HIGH)
  {
    btnState.modePressed = false;
  }
}

void handleStopButton()
{
  if (digitalRead(BTN_STOP) == LOW)
  {
    if (currentState == FOLLOWING)
    {
      currentState = IDLE;
      sendMotorCommand(0);
      tone(BUZZER, 700, 150);
    }
  }
}

void handleEstopButton()
{
  if (digitalRead(BTN_ESTOP) == LOW)
  {
    triggerAlarm("E-STOP Button!");
  }
}

void handleStartButton()
{
  if (!btnState.startPressed && millis() - btnState.lastDebounceTime > btnState.DEBOUNCE_DELAY)
  {
    if (digitalRead(BTN_START) == LOW)
    {
      if (currentState == IDLE && deviceFound)
      {
        currentState = LOCKED;
        btTarget.lastSignalTime = millis();
        tone(BUZZER, 1000, 200);
        Serial.println("START pressed - entering LOCKED state");
      }
      btnState.startPressed = true;
      btnState.lastDebounceTime = millis();
    }
  }
  if (btnState.startPressed && digitalRead(BTN_START) == HIGH)
  {
    btnState.startPressed = false;
  }
}

void checkButtonInputs()
{
  handleStartButton();
  handleTareButton();
  handleModeButton();
  handleStopButton();
  handleEstopButton();
}