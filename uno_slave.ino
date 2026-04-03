#include <Arduino.h>
#include <SoftwareSerial.h>
#include <Servo.h>
#include <Wire.h>
#include <MPU6050.h>

#ifndef A1
#define A1 15
#endif
#ifndef A2
#define A2 16
#endif
#ifndef A3
#define A3 17
#endif

// --- Pin Definitions ---
// Communication with ESP32
#define ESP_RX 2
#define ESP_TX 13
SoftwareSerial espSerial(ESP_RX, ESP_TX);

// Motors (TB6612FNG)
#define PWMA 10
#define PWMB 11
#define AIN1 A0
#define AIN2 A1
#define BIN1 A2
#define BIN2 A3
#define STBY 12 // TB6612FNG standby pin (active HIGH = drivers enabled)

// Ultrasonics
#define TRIG_FRONT 4
#define ECHO_FRONT 5
#define TRIG_LEFT 6
#define ECHO_LEFT 7
#define TRIG_RIGHT 8
#define ECHO_RIGHT 9

// Servo Brake
#define SERVO_PIN 3

// --- Object Initialization ---
Servo brakeServo;
MPU6050 mpu;

// Constants
const int SAFE_DISTANCE = 30;           // cm - obstacle stop distance
const int OBSTACLE_STEER_DISTANCE = 50; // cm - start steering away from obstacles
const int TILT_THRESHOLD = 45;          // degrees - max safe pitch/roll
const int MOTOR_MAX_SPEED = 255;        // PWM max

// Current motion state
int lastMotorSpeedL = 0;
int lastMotorSpeedR = 0;
bool isMoving = false;
static unsigned long lastObstacleAlertTime = 0;

void setup()
{
    // Debug Serial
    Serial.begin(115200);

    // ESP32 Comms Serial
    espSerial.begin(9600);

    // Motor Pins
    pinMode(PWMA, OUTPUT);
    pinMode(PWMB, OUTPUT);
    pinMode(AIN1, OUTPUT);
    pinMode(AIN2, OUTPUT);
    pinMode(BIN1, OUTPUT);
    pinMode(BIN2, OUTPUT);
    pinMode(STBY, OUTPUT);
    digitalWrite(STBY, HIGH); // Enable TB6612FNG drivers

    // Ultrasonic Pins
    pinMode(TRIG_FRONT, OUTPUT);
    pinMode(ECHO_FRONT, INPUT);
    pinMode(TRIG_LEFT, OUTPUT);
    pinMode(ECHO_LEFT, INPUT);
    pinMode(TRIG_RIGHT, OUTPUT);
    pinMode(ECHO_RIGHT, INPUT);

    // Servo Init
    brakeServo.attach(SERVO_PIN);
    brakeServo.write(0); // Disengage brake

    // I2C for MPU6050
    Wire.begin();
    mpu.initialize();

    // Verify MPU6050 connection
    if (!mpu.testConnection())
    {
        Serial.println("ERROR: MPU6050 connection failed!");
        espSerial.println("ALERT:MPU_INIT_FAIL");
    }
    else
    {
        Serial.println("MPU6050 initialized successfully");
    }
}

void loop()
{
    // ===== SAFETY CHECKS (Before ANY Movement) =====

    // 1. Safety Check: Tilt Detection via MPU6050
    int16_t ax, ay, az;
    mpu.getAcceleration(&ax, &ay, &az);

    // Approximate pitch/roll from accelerometer (simplified)
    float pitchDeg = atan2(ay, az) * 180 / PI;
    float rollDeg = atan2(ax, az) * 180 / PI;

    // Check if tilt exceeds safe threshold
    if (abs(pitchDeg) > TILT_THRESHOLD || abs(rollDeg) > TILT_THRESHOLD)
    {
        if (isMoving)
        {
            Serial.print("TILT ALERT - Pitch: ");
            Serial.print(pitchDeg);
            Serial.print(" Roll: ");
            Serial.println(rollDeg);
            executeStop();
            espSerial.println("ALERT:TILT");
            delay(100);
            return; // Stop processing commands
        }
    }

    // 2. Safety Check: Obstacle Detection with Smart Avoidance
    long frontDist = readUltrasonic(TRIG_FRONT, ECHO_FRONT);
    long leftDist = readUltrasonic(TRIG_LEFT, ECHO_LEFT);
    long rightDist = readUltrasonic(TRIG_RIGHT, ECHO_RIGHT);

    // Critical obstacle - stop immediately
    if (frontDist > 0 && frontDist < SAFE_DISTANCE)
    {
        if (isMoving)
        {
            executeStop();
            // Prevent serial spam
            if (millis() - lastObstacleAlertTime > 500)
            {
                espSerial.println("ALERT:OBSTACLE_FRONT");
                lastObstacleAlertTime = millis();
            }
            delay(100);
            return;
        }
    }

    // Approaching obstacle - start steering away
    if (isMoving && frontDist > 0 && frontDist < OBSTACLE_STEER_DISTANCE)
    {
        // Decide which side to steer
        if (rightDist > leftDist)
        {
            steerRight(lastMotorSpeedL, 30); // Steer right (slower right motor)
        }
        else
        {
            steerLeft(lastMotorSpeedL, 30); // Steer left (slower left motor)
        }
        return;
    }

    // ===== PROCESS COMMANDS FROM ESP32 =====
    if (espSerial.available())
    {
        String command = espSerial.readStringUntil('\n');
        command.trim();

        if (command.startsWith("<MOVE"))
        {
            // Parse "<MOVE,speedL,speedR>"
            int commaIdx1 = command.indexOf(',');
            int commaIdx2 = command.lastIndexOf(',');

            if (commaIdx1 > 0 && commaIdx2 > commaIdx1)
            {
                int speedL = command.substring(commaIdx1 + 1, commaIdx2).toInt();
                int speedR = command.substring(commaIdx2 + 1).toInt();

                // Clamp speeds to valid range
                speedL = constrain(speedL, 0, MOTOR_MAX_SPEED);
                speedR = constrain(speedR, 0, MOTOR_MAX_SPEED);

                moveForward(speedL, speedR);
                isMoving = (speedL > 0 || speedR > 0);
            }
        }
        else if (command == "<STOP>")
        {
            executeStop();
            isMoving = false;
        }
    }
}

long readUltrasonic(int trigPin, int echoPin)
{
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    long duration = pulseIn(echoPin, HIGH, 30000); // 30ms timeout
    if (duration == 0)
        return 999; // No echo
    return duration * 0.034 / 2;
}

void moveForward(int speedL, int speedR)
{
    brakeServo.write(0); // Ensure brake is off

    // Set Directions Forward
    digitalWrite(AIN1, HIGH);
    digitalWrite(AIN2, LOW);
    digitalWrite(BIN1, HIGH);
    digitalWrite(BIN2, LOW);

    // Set Speeds
    analogWrite(PWMA, speedL);
    analogWrite(PWMB, speedR);

    // Remember last commanded speeds for avoidance logic
    lastMotorSpeedL = speedL;
    lastMotorSpeedR = speedR;
}

// Steer left by reducing left motor speed
void steerLeft(int baseSpeed, int speedReduction)
{
    int leftSpeed = constrain(baseSpeed - speedReduction, 0, MOTOR_MAX_SPEED);
    int rightSpeed = constrain(baseSpeed, 0, MOTOR_MAX_SPEED);

    Serial.print("Steering LEFT: L");
    Serial.print(leftSpeed);
    Serial.print(" R");
    Serial.println(rightSpeed);

    moveForward(leftSpeed, rightSpeed);
}

// Steer right by reducing right motor speed
void steerRight(int baseSpeed, int speedReduction)
{
    int leftSpeed = constrain(baseSpeed, 0, MOTOR_MAX_SPEED);
    int rightSpeed = constrain(baseSpeed - speedReduction, 0, MOTOR_MAX_SPEED);

    Serial.print("Steering RIGHT: L");
    Serial.print(leftSpeed);
    Serial.print(" R");
    Serial.println(rightSpeed);

    moveForward(leftSpeed, rightSpeed);
}

void executeStop()
{
    // Electronic Brake via TB6612FNG (Both IN pins HIGH)
    digitalWrite(AIN1, HIGH);
    digitalWrite(AIN2, HIGH);
    digitalWrite(BIN1, HIGH);
    digitalWrite(BIN2, HIGH);
    analogWrite(PWMA, 0);
    analogWrite(PWMB, 0);

    // Engage Physical Servo Brake
    brakeServo.write(90);
}