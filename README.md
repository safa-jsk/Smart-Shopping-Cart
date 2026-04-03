# 🛒 Smart Auto-Following Shopping Cart (w/ RFID)

---

## 🎯 Purpose

**Objective:**
Design and build an autonomous shopping cart robot that locks onto a specific user, follows them safely, avoids obstacles, and provides a self-scan system using RFID that shows total cost, total items, and total weight in real time.

**Scope:**
Small stores, Libraries, anywhere a shopping cart can be used.

**Significance:**
Easy budgeting, shopping checklisting, ease of mind while shopping.

---

## ⚙️ Components

### Microcontrollers

- Arduino UNO R3
- ESP32

### Sensors

- Ultrasonic sensor HC-SR04
- IMU MPU6050
- RFID reader RC522
- Load cell

### Actuators

- DC Gear Motors (4WD)
- TB6612FNG Dual Motor Driver
- Servo wheel-lock
- Buzzer (active)
- LEDs (R/G/B)

### Body / Chassis

- 4WD chassis
- Cart frame

### Additional Components

- HX711 amplifier
- LCD 16x2 with I2C module
- Buttons (Start/Stop, Mode, Tare, E-Stop)
- Battery case 4xAA
- Battery 3.7V
- Voltage Converter

---

## 💰 Cost Breakdown (Approx.)

| Component                    | Qty | Unit Cost | Total    |
| :--------------------------- | :-- | :-------- | :------- |
| Arduino UNO R3               | 1   | 990       | 990      |
| ESP32 DevKit                 | 1   | 650       | 650      |
| Smart Phone                  | 1   | 0         | 0        |
| TB6612FNG Dual Motor Driver  | 2   | 220       | 440      |
| DC Gear Motors (4WD)         | 1   | 220       | 220      |
| Servo wheel-lock / brake pad | 2   | 90        | 180      |
| Ultrasonic sensor HC‑SR04    | 1   | 110       | 110      |
| IMU MPU6050                  | 1   | 360       | 360      |
| RFID reader RC522            | 1   | 230       | 230      |
| RFID NFC Smart Card          | 5   | 30        | 150      |
| Load cell                    | 1   | 380       | 380      |
| HX711 amplifier              | 1   | 130       | 130      |
| LCD 16x2 with I2C module     | 1   | 195       | 195      |
| Buttons                      | 5   | 2         | 10       |
| Buzzer (active)              | 1   | 10        | 10       |
| LEDs (R/G/B)                 | 10  | 1         | 10       |
| Battery case 4xAA            | 1   | 100       | 100      |
| Battery 3.7V                 | 4   | 80        | 320      |
| Voltage Converter            | 1   | 180       | 180      |
| Jumper Wires (M-M, M-F, F-F) | 3   | 60        | 180      |
| Resistors                    | 1   | 50        | 50       |
| **Grand Total**              |     |           | **4895** |

---

## 🚀 Functionality Breakdown

### Functionality 1: User Lock-On + Auto Follow

**Overview:** The cart pairs with a specific user (phone) and follows them at a safe distance. It slows down if too close, speeds up if too far, and stops if the user is lost.

**Working Procedure:**

- **Pairing / Lock-on:**
  - ESP32 scans for the user’s phone Bluetooth identity (name/MAC or a stored token).
  - Press Start to lock and begin follow mode.
- **Follow Logic:**
  - ESP32 estimates “near/far” using Bluetooth signal strength (RSSI) bands.
  - ESP32 computes a speed target and sends commands to UNO via UART (with level shifting from UNO TX to ESP32 RX).
- **Motion Execution:**
  - UNO generates PWM and direction signals to the TB6612FNG, driving the 4 motors.
  - Electronic brake mode is applied if stop is required.
- **Lost User Safety:**
  - If RSSI drops below a threshold for N seconds → cart enters LOST state: stop, buzzer alert, flashing LED.

### Functionality 2: Obstacle Avoidance + Safety Braking

**Overview:** The cart avoids collisions using front/side ultrasonic sensors and adds safety shutdown using an emergency stop and tilt monitoring.

**Working Procedure:**

- **Obstacle Sensing:**
  - UNO continuously reads 3 ultrasonic sensors (front/left/right) with short sampling intervals.
  - If front distance < threshold → brake immediately.
  - If left/right indicates obstruction → steer away (slow turn) or stop and wait.
- **Safety Braking:**
  - **Primary stop:** TB6612FNG electronic brake (fast response).
  - **Secondary stop:** Servo wheel-lock / brake pad engages when E-Stop or tilt event triggers.
- **Tilt / Anti-tip Safety (MPU6050):**
  - UNO or ESP32 monitors pitch/roll.
  - If tilt exceeds safe angle → stop motors + engage servo brake + alarm.

### Functionality 3: RFID Self-Scan + Live Cart Totals Display

**Overview:** User scans items as they place them into the cart. The system shows total items, total price, and total weight on the LCD.

**Working Procedure:**

- **Item Scanning:**
  - RFID RC522 reads card UID.
  - ESP32 matches UID to an internal item table (name, price, optional expected weight).
  - On each scan: Add item to cart list (or increment quantity if repeated).
- **Weight Measurement:**
  - Load cell + HX711 provides real-time weight.
  - Press the Tare button to zero the scale (empty cart calibration).
  - ESP32 updates LCD with live measured weight.
- **LCD UI (16x2 I2C):**
  - Shows: total items, total cost, total weight, and current mode/status (FOLLOW/STOP/LOST/OBSTACLE).
- **Alerts & Feedback:**
  - **Successful scan:** short beep + green LED flash.
  - **Invalid / unregistered tag:** buzzer pattern + red LED.
  - **Overweight threshold:** warning on LCD + buzzer.
