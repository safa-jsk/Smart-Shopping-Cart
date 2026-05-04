#include <SoftwareSerial.h>

// --- UART Link to ESP32 ---
SoftwareSerial espSerial(2, 3); // RX=D2, TX=D3

// --- Sonar Pins ---
#define TRIG_F 10
#define ECHO_F 11
#define TRIG_R A0
#define ECHO_R A1
#define TRIG_L A3
#define ECHO_L A2

// --- Motor Pins (Dual L298N Setup) ---
#define RIGHT_PWM 5
#define RIGHT_IN1 4
#define RIGHT_IN2 6

#define LEFT_PWM 9
#define LEFT_IN1 8
#define LEFT_IN2 7

// --- System Variables ---
int targetRSSI = -100;
unsigned long lastPacketTime = 0;
const int TIMEOUT_MS = 1000;

void setup()
{
    Serial.begin(115200);
    espSerial.begin(9600);

    pinMode(TRIG_L, OUTPUT);
    pinMode(ECHO_L, INPUT);
    pinMode(TRIG_F, OUTPUT);
    pinMode(ECHO_F, INPUT);
    pinMode(TRIG_R, OUTPUT);
    pinMode(ECHO_R, INPUT);

    pinMode(RIGHT_IN1, OUTPUT);
    pinMode(RIGHT_IN2, OUTPUT);
    pinMode(RIGHT_PWM, OUTPUT);
    pinMode(LEFT_IN1, OUTPUT);
    pinMode(LEFT_IN2, OUTPUT);
    pinMode(LEFT_PWM, OUTPUT);

    stopMotors();
    Serial.println("4WD Autonomous Coprocessor Ready.");
}

int readSonar(int trigPin, int echoPin)
{
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    long duration = pulseIn(echoPin, HIGH, 30000);
    if (duration == 0)
        return 999;
    return duration * 0.034 / 2;
}

// --- Direction Logic Fixed to Match your "Working" code ---
void moveForward(int speed)
{
    digitalWrite(LEFT_IN1, LOW);
    digitalWrite(LEFT_IN2, HIGH);
    digitalWrite(RIGHT_IN1, LOW);
    digitalWrite(RIGHT_IN2, HIGH);
    analogWrite(LEFT_PWM, speed);
    analogWrite(RIGHT_PWM, speed);
}

void nudgeLeft(int speed)
{
    digitalWrite(LEFT_IN1, LOW);
    digitalWrite(LEFT_IN2, HIGH);
    digitalWrite(RIGHT_IN1, LOW);
    digitalWrite(RIGHT_IN2, HIGH);
    analogWrite(LEFT_PWM, speed / 2); // Slow left
    analogWrite(RIGHT_PWM, speed);
}

void nudgeRight(int speed)
{
    digitalWrite(LEFT_IN1, LOW);
    digitalWrite(LEFT_IN2, HIGH);
    digitalWrite(RIGHT_IN1, LOW);
    digitalWrite(RIGHT_IN2, HIGH);
    analogWrite(LEFT_PWM, speed);
    analogWrite(RIGHT_PWM, speed / 2); // Slow right
}

void stopMotors()
{
    digitalWrite(LEFT_IN1, LOW);
    digitalWrite(LEFT_IN2, LOW);
    digitalWrite(RIGHT_IN1, LOW);
    digitalWrite(RIGHT_IN2, LOW);
    analogWrite(LEFT_PWM, 0);
    analogWrite(RIGHT_PWM, 0);
}

void loop()
{
    // 1. READ SONARS
    int leftDist = readSonar(TRIG_L, ECHO_L);
    int frontDist = readSonar(TRIG_F, ECHO_F);
    int rightDist = readSonar(TRIG_R, ECHO_R);

    // 2. LISTEN FOR ESP32
    if (espSerial.available())
    {
        String incoming = espSerial.readStringUntil('\n');
        incoming.trim();
        if (incoming.startsWith("<") && incoming.endsWith(">"))
        {
            targetRSSI = incoming.substring(1, incoming.length() - 1).toInt();
            lastPacketTime = millis(); // Reset Deadman stopwatch
        }
    }

    // 3. DEADMAN'S SWITCH
    if (millis() - lastPacketTime > TIMEOUT_MS)
    {
        targetRSSI = -100;
    }

    // 4. FILTERED LOGIC
    bool obstacleFront = (frontDist > 0 && frontDist < 30);
    bool obstacleLeft = (leftDist > 0 && leftDist < 20);
    bool obstacleRight = (rightDist > 0 && rightDist < 20);

    if (targetRSSI <= -95 || targetRSSI >= -50)
    {
        stopMotors(); // Target lost, too far, or too close
    }
    else
    {
        // --- CART IS IN TRACKING RANGE ---

        // Proportional speed based on distance (RSSI)
        int dynamicSpeed = map(targetRSSI, -50, -85, 120, 230);
        dynamicSpeed = constrain(dynamicSpeed, 120, 230);

        if (obstacleFront)
        {
            stopMotors(); // Safety first
        }
        else if (obstacleLeft)
        {
            nudgeRight(dynamicSpeed);
        }
        else if (obstacleRight)
        {
            nudgeLeft(dynamicSpeed);
        }
        else
        {
            moveForward(dynamicSpeed);
        }
    }

    delay(50);
}