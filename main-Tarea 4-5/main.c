#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <MPU6050.h>
#include <LiquidCrystal_I2C.h>

// --- WiFi & MQTT ---
const char* ssid = "TU_SSID";
const char* password = "TU_PASSWORD";
const char* mqtt_server = "test.mosquitto.org";

WiFiClient espClient;
PubSubClient client(espClient);

// --- MPU6050 ---
MPU6050 mpu;

// --- LCD (dirección típica 0x27, 16x2) ---
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- Joysticks (según KiCad) ---
#define JOY0_MT 4
#define JOY0_MD 5
#define JOY1_MT 6
#define JOY1_MD 7

// --- Botones (según KiCad) ---
#define BTNL1 10
#define BTNL2 11
#define BTNL3 12
#define BTNL4 13

// --- I2C (LCD + MPU6050) ---
#define I2C_SDA 21
#define I2C_SCL 22

// --- Variables de calibración ---
int offsetJoy0X = 0;
int offsetJoy0Y = 0;
int offsetJoy1X = 0;
int offsetJoy1Y = 0;

// --- Función para mapear joystick ---
int mapJoystick(int value) {
  return map(value, 0, 4095, -100, 100);
}

// --- Función de calibración ---
void calibrarJoysticks() {
  offsetJoy0X = mapJoystick(analogRead(JOY0_MT));
  offsetJoy0Y = mapJoystick(analogRead(JOY0_MD));
  offsetJoy1X = mapJoystick(analogRead(JOY1_MT));
  offsetJoy1Y = mapJoystick(analogRead(JOY1_MD));

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Calibrado!");
  delay(1000);
}

void setup() {
  Serial.begin(115200);

  // WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi conectado");

  // MQTT
  client.setServer(mqtt_server, 1883);

  // I2C + MPU6050
  Wire.begin(I2C_SDA, I2C_SCL);
  mpu.initialize();
  if (!mpu.testConnection()) {
    Serial.println("MPU6050 no conectado!");
  }

  // Botones
  pinMode(BTNL1, INPUT_PULLUP);
  pinMode(BTNL2, INPUT_PULLUP);
  pinMode(BTNL3, INPUT_PULLUP);
  pinMode(BTNL4, INPUT_PULLUP);

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print("KAKATA-433 Ready");
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // --- Rutina de calibración ---
  static unsigned long pressStart = 0;
  if (!digitalRead(BTNL1) && !digitalRead(BTNL2)) {
    if (pressStart == 0) pressStart = millis();
    if (millis() - pressStart >= 3000) { // 3 segundos
      calibrarJoysticks();
      pressStart = 0;
    }
  } else {
    pressStart = 0;
  }

  // Lectura joystick con offset
  int joy0X = mapJoystick(analogRead(JOY0_MT)) - offsetJoy0X;
  int joy0Y = mapJoystick(analogRead(JOY0_MD)) - offsetJoy0Y;
  int joy1X = mapJoystick(analogRead(JOY1_MT)) - offsetJoy1X;
  int joy1Y = mapJoystick(analogRead(JOY1_MD)) - offsetJoy1Y;

  // Lectura botones
  bool b1 = !digitalRead(BTNL1);
  bool b2 = !digitalRead(BTNL2);
  bool b3 = !digitalRead(BTNL3);
  bool b4 = !digitalRead(BTNL4);

  // Lectura MPU6050
  int16_t ax, ay, az, gx, gy, gz;
  mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

  // Publicar por MQTT
  String payload = "{ \"joy0X\": " + String(joy0X) +
                   ", \"joy0Y\": " + String(joy0Y) +
                   ", \"joy1X\": " + String(joy1X) +
                   ", \"joy1Y\": " + String(joy1Y) +
                   ", \"btn1\": " + String(b1) +
                   ", \"btn2\": " + String(b2) +
                   ", \"btn3\": " + String(b3) +
                   ", \"btn4\": " + String(b4) +
                   ", \"ax\": " + String(ax) +
                   ", \"ay\": " + String(ay) +
                   ", \"az\": " + String(az) +
                   ", \"gx\": " + String(gx) +
                   ", \"gy\": " + String(gy) +
                   ", \"gz\": " + String(gz) + " }";

  client.publish("kakata/control", payload.c_str());

  // Mostrar en LCD
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("J0X:");
  lcd.print(joy0X);
  lcd.print(" J0Y:");
  lcd.print(joy0Y);

  lcd.setCursor(0,1);
  lcd.print("B1:");
  lcd.print(b1);
  lcd.print(" Ax:");
  lcd.print(ax/1000); // simplificado
  delay(500);
}

void reconnect() {
  while (!client.connected()) {
    if (client.connect("ESP32Client")) {
      client.subscribe("kakata/control");
    } else {
      delay(5000);
    }
  }
}
