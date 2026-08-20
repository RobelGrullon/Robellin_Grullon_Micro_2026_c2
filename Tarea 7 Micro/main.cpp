extern "C" {
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "esp_random.h"
}

// ======================== PINES ========================
#define PB1_GPIO        GPIO_NUM_4
#define PB2_GPIO        GPIO_NUM_5
#define LED_GPIO        GPIO_NUM_2
#define I2C_SDA_PIN     GPIO_NUM_21
#define I2C_SCL_PIN     GPIO_NUM_22

// ======================== MPU6050 ========================
#define MPU6050_ADDR         0x68
#define MPU6050_WHO_AM_I     0x75
#define MPU6050_PWR_MGMT_1   0x6B
#define MPU6050_ACCEL_XOUT_H 0x3B

// ======================== WIFI / MQTT ========================
#define WIFI_SSID        CONFIG_ESP_WIFI_SSID
#define WIFI_PASS        CONFIG_ESP_WIFI_PASSWORD
#define MQTT_BROKER_URI  "mqtt://test.mosquitto.org"
#define MQTT_TOPIC       "esp32/reaction"

// ======================== EVENTOS ========================
#define WIFI_CONNECTED_BIT  BIT0
#define MQTT_CONNECTED_BIT  BIT1

static const char *TAG = "REACTION";
static EventGroupHandle_t s_event_group;
static esp_mqtt_client_handle_t s_mqtt_client;
static bool s_mqtt_connected = false;

// ======================== I2C HANDLES ========================
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_mpu_dev;

// ======================== MPU6050 ========================
typedef struct {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
} mpu6050_data_t;

static void i2c_init(void)
{
    i2c_master_bus_config_t bus = {};
    bus.i2c_port   = I2C_NUM_0;
    bus.sda_io_num = I2C_SDA_PIN;
    bus.scl_io_num = I2C_SCL_PIN;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_i2c_bus));
    ESP_LOGI(TAG, "I2C listo");
}

static void mpu6050_init(void)
{
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = MPU6050_ADDR;
    dev.scl_speed_hz    = 100000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev, &s_mpu_dev));

    uint8_t reg = MPU6050_WHO_AM_I;
    uint8_t who = 0;
    if (i2c_master_transmit_receive(s_mpu_dev, &reg, 1, &who, 1, pdMS_TO_TICKS(100)) != ESP_OK || who != MPU6050_ADDR) {
        ESP_LOGW(TAG, "MPU6050 no encontrado (0x%02X)", who);
        return;
    }
    uint8_t cmd[2] = { MPU6050_PWR_MGMT_1, 0x00 };
    i2c_master_transmit(s_mpu_dev, cmd, 2, pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "MPU6050 listo");
}

static mpu6050_data_t mpu6050_read(void)
{
    mpu6050_data_t d = {};
    uint8_t reg = MPU6050_ACCEL_XOUT_H;
    uint8_t buf[14];
    if (i2c_master_transmit_receive(s_mpu_dev, &reg, 1, buf, 14, pdMS_TO_TICKS(100)) == ESP_OK) {
        d.ax = (int16_t)((buf[0]  << 8) | buf[1]);
        d.ay = (int16_t)((buf[2]  << 8) | buf[3]);
        d.az = (int16_t)((buf[4]  << 8) | buf[5]);
        d.gx = (int16_t)((buf[8]  << 8) | buf[9]);
        d.gy = (int16_t)((buf[10] << 8) | buf[11]);
        d.gz = (int16_t)((buf[12] << 8) | buf[13]);
    }
    return d;
}

// ======================== GPIO ========================
static uint32_t millis_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void gpio_init(void)
{
    gpio_config_t io = {};

    io.pin_bit_mask = (1ULL << PB1_GPIO) | (1ULL << PB2_GPIO);
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io);

    io.pin_bit_mask = (1ULL << LED_GPIO);
    io.mode         = GPIO_MODE_OUTPUT;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);

    gpio_set_level(LED_GPIO, 0);
}

// ======================== WIFI ========================
static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi OK — IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t a, b;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_handler, NULL, &a));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_handler, NULL, &b));

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ======================== MQTT ========================
static void mqtt_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    if (ev->event_id == MQTT_EVENT_CONNECTED) {
        s_mqtt_connected = true;
        xEventGroupSetBits(s_event_group, MQTT_CONNECTED_BIT);
        ESP_LOGI(TAG, "MQTT conectado");
    } else if (ev->event_id == MQTT_EVENT_DISCONNECTED) {
        s_mqtt_connected = false;
        xEventGroupClearBits(s_event_group, MQTT_CONNECTED_BIT);
    }
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t mc = {};
    mc.broker.address.uri = MQTT_BROKER_URI;
    mc.session.keepalive = 60;
    s_mqtt_client = esp_mqtt_client_init(&mc);
    esp_mqtt_client_register_event(s_mqtt_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

static void mqtt_publish(uint32_t reaction, uint32_t transition, const mpu6050_data_t *mpu)
{
    char json[300];
    snprintf(json, sizeof(json),
             "{\"reaction_time_ms\":%lu,\"transition_time_ms\":%lu,"
             "\"accel\":{\"x\":%d,\"y\":%d,\"z\":%d},"
             "\"gyro\":{\"x\":%d,\"y\":%d,\"z\":%d}}",
             (unsigned long)reaction, (unsigned long)transition,
             mpu->ax, mpu->ay, mpu->az,
             mpu->gx, mpu->gy, mpu->gz);

    if (s_mqtt_connected) {
        esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC, json, 0, 1, pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Publicado: %s", json);
    } else {
        ESP_LOGW(TAG, "MQTT offline, no publicado");
    }
}

// ======================== MAIN ========================
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Medidor de Reaccion Humana ===");

    gpio_init();
    i2c_init();
    mpu6050_init();

    nvs_flash_init();
    wifi_init();
    xEventGroupWaitBits(s_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));

    mqtt_init();
    xEventGroupWaitBits(s_event_group, MQTT_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));

    esp_random();
    ESP_LOGI(TAG, "Listo — mantenga PB1 presionado");

    while (true) {

        // 1. Esperar que el usuario PRESIONE PB1
        while (gpio_get_level(PB1_GPIO) != 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGI(TAG, "PB1 presionado — esperando senal...");

        // 2. Tiempo aleatorio 2-7 s
        uint32_t delay_ms = 2000 + (esp_random() % 5001);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));

        // 3. Encender LED — cronometro arranca
        gpio_set_level(LED_GPIO, 1);
        uint32_t t_start = millis_now();
        ESP_LOGI(TAG, "LED ON — reaccione!");

        // 4. Esperar que SUELTE PB1
        while (gpio_get_level(PB1_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        uint32_t reaction_time = millis_now() - t_start;
        gpio_set_level(LED_GPIO, 0);
        ESP_LOGI(TAG, "PB1 soltado — reaccion: %lu ms", (unsigned long)reaction_time);

        // 5. Esperar que PRESIONE PB2
        ESP_LOGI(TAG, "Ahora presione PB2...");
        while (gpio_get_level(PB2_GPIO) != 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        uint32_t transition_time = millis_now() - t_start - reaction_time;
        ESP_LOGI(TAG, "PB2 presionado — transicion: %lu ms", (unsigned long)transition_time);

        // 6. Leer MPU6050 y publicar
        mpu6050_data_t mpu = mpu6050_read();
        mqtt_publish(reaction_time, transition_time, &mpu);

        ESP_LOGI(TAG, "Ronda completa. Siguiente en 3s...\n");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
