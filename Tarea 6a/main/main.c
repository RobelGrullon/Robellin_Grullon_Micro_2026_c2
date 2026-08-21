#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_random.h"

#include "nvs_flash.h"

#include "driver/gpio.h"

#include "mqtt_client.h"

// ==========================================================
// CONFIGURACION DE RED
// ==========================================================

#define WIFI_SSID       "micro"
#define WIFI_PASSWORD   "micro9898"

// ==========================================================
// CONFIGURACION MQTT
// ==========================================================

#define MQTT_BROKER     "mqtt://broker.emqx.io:1883"

#define MQTT_TOPIC_CONTROL    "reaction_game/control"
#define MQTT_TOPIC_RESULTS    "reaction_game/times"
#define MQTT_TOPIC_MESSAGE    "topic/qos0"

// ==========================================================
// CONFIGURACION DE GPIO
// ==========================================================

#define PIN_BOTON_A   GPIO_NUM_4
#define PIN_BOTON_B   GPIO_NUM_13

#define PIN_LED_A     GPIO_NUM_2
#define PIN_LED_B     GPIO_NUM_15

// ==========================================================
// EVENTOS DE WIFI
// ==========================================================

#define WIFI_CONECTADO BIT0

static EventGroupHandle_t eventos_wifi;

static const char *TAG = "JUEGO_REACCION";

// ==========================================================
// ESTADOS DEL JUEGO
// ==========================================================

typedef enum
{
    INICIO,
    ESPERA_A,
    LED_ACTIVO,
    ESPERA_B,
    FINALIZADO

} EstadoJuego;

static EstadoJuego estado_actual = INICIO;

// ==========================================================
// VARIABLES PARA MEDIR LOS TIEMPOS
// ==========================================================

static int64_t tiempo_encendido = 0;
static int64_t tiempo_liberacion = 0;

static uint32_t resultado_a = 0;
static uint32_t resultado_b = 0;

static int64_t inicio_espera = 0;
static int64_t retraso_generado = 0;

// ==========================================================
// ESTRUCTURA PARA ANTIRREBOTE
// ==========================================================

typedef struct
{
    bool lectura_anterior;
    bool estado_filtrado;
    bool estado_previo;
    int64_t ultimo_cambio;

} Boton;

static Boton boton_a = {
    false,
    false,
    false,
    0
};

static Boton boton_b = {
    false,
    false,
    false,
    0
};

#define TIEMPO_FILTRO_US 2000

// ==========================================================
// CLIENTE MQTT
// ==========================================================

static esp_mqtt_client_handle_t cliente_mqtt = NULL;

static bool mqtt_conectado = false;

// ==========================================================
// LECTURA DE BOTONES
// ==========================================================

static bool leer_boton(gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

// ==========================================================
// CONTROL DE LED A
// ==========================================================

static void encender_led_a(void)
{
    gpio_set_level(PIN_LED_A, 1);
}

static void apagar_led_a(void)
{
    gpio_set_level(PIN_LED_A, 0);
}

// ==========================================================
// CONTROL DE LED B
// ==========================================================

static void encender_led_b(void)
{
    gpio_set_level(PIN_LED_B, 1);
}

static void apagar_led_b(void)
{
    gpio_set_level(PIN_LED_B, 0);
}

// ==========================================================
// FILTRO DE BOTON
// ==========================================================

static bool procesar_boton(
    gpio_num_t pin,
    Boton *datos,
    int64_t reloj_actual)
{
    bool lectura = leer_boton(pin);

    if (lectura != datos->lectura_anterior)
    {
        datos->ultimo_cambio = reloj_actual;
        datos->lectura_anterior = lectura;
    }

    if ((reloj_actual - datos->ultimo_cambio)
        >= TIEMPO_FILTRO_US)
    {
        datos->estado_filtrado = lectura;
    }

    return datos->estado_filtrado;
}

// ==========================================================
// EVENTOS WIFI
// ==========================================================

static void evento_wifi(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();

        ESP_LOGI(TAG, "Conectando al punto de acceso...");
    }

    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(
            eventos_wifi,
            WIFI_CONECTADO
        );

        ESP_LOGW(
            TAG,
            "WiFi desconectado. Intentando nuevamente..."
        );

        esp_wifi_connect();
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(
            eventos_wifi,
            WIFI_CONECTADO
        );

        ESP_LOGI(
            TAG,
            "Conexion WiFi establecida."
        );
    }
}

// ==========================================================
// CONFIGURACION WIFI
// ==========================================================

static void iniciar_wifi(void)
{
    eventos_wifi = xEventGroupCreate();

    ESP_ERROR_CHECK(
        esp_netif_init()
    );

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t configuracion_wifi =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&configuracion_wifi)
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &evento_wifi,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &evento_wifi,
            NULL
        )
    );

    wifi_config_t configuracion = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN
        }
    };

    strcpy(
        (char *)configuracion.sta.ssid,
        WIFI_SSID
    );

    strcpy(
        (char *)configuracion.sta.password,
        WIFI_PASSWORD
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &configuracion
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    xEventGroupWaitBits(
        eventos_wifi,
        WIFI_CONECTADO,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );
}

// ==========================================================
// EVENTOS MQTT
// ==========================================================

static void evento_mqtt(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data)
{
    esp_mqtt_event_handle_t evento =
        event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:

            mqtt_conectado = true;

            ESP_LOGI(
                TAG,
                "Conexion MQTT establecida."
            );

            esp_mqtt_client_subscribe(
                cliente_mqtt,
                MQTT_TOPIC_CONTROL,
                0
            );

            esp_mqtt_client_publish(
                cliente_mqtt,
                MQTT_TOPIC_MESSAGE,
                "Sistema preparado para iniciar",
                0,
                0,
                0
            );

            break;

        case MQTT_EVENT_DISCONNECTED:

            mqtt_conectado = false;

            ESP_LOGW(
                TAG,
                "Conexion MQTT perdida."
            );

            break;

        case MQTT_EVENT_ERROR:

            ESP_LOGE(
                TAG,
                "Error en la conexion MQTT."
            );

            break;

        default:
            break;
    }

    (void)evento;
}

// ==========================================================
// INICIAR MQTT
// ==========================================================

static void iniciar_mqtt(void)
{
    esp_mqtt_client_config_t configuracion = {
        .broker.address.uri = MQTT_BROKER
    };

    cliente_mqtt =
        esp_mqtt_client_init(&configuracion);

    ESP_ERROR_CHECK(
        esp_mqtt_client_register_event(
            cliente_mqtt,
            ESP_EVENT_ANY_ID,
            evento_mqtt,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_mqtt_client_start(cliente_mqtt)
    );
}

// ==========================================================
// ENVIAR RESULTADOS
// ==========================================================

static void enviar_resultados(void)
{
    char paquete[100];

    snprintf(
        paquete,
        sizeof(paquete),
        "{\"reaction1_ms\":%" PRIu32
        ",\"reaction2_ms\":%" PRIu32 "}",
        resultado_a,
        resultado_b
    );

    if (mqtt_conectado)
    {
        esp_mqtt_client_publish(
            cliente_mqtt,
            MQTT_TOPIC_RESULTS,
            paquete,
            0,
            0,
            0
        );

        char mensaje[120];

        snprintf(
            mensaje,
            sizeof(mensaje),
            "Primer resultado: %" PRIu32
            " ms - Segundo resultado: %" PRIu32
            " ms",
            resultado_a,
            resultado_b
        );

        esp_mqtt_client_publish(
            cliente_mqtt,
            MQTT_TOPIC_MESSAGE,
            mensaje,
            0,
            0,
            0
        );

        ESP_LOGI(
            TAG,
            "Resultados enviados mediante MQTT."
        );
    }
}

// ==========================================================
// CONFIGURAR GPIO
// ==========================================================

static void configurar_gpio(void)
{
    gpio_config_t botones = {
        .pin_bit_mask =
            (1ULL << PIN_BOTON_A) |
            (1ULL << PIN_BOTON_B),

        .mode = GPIO_MODE_INPUT,

        .pull_up_en = GPIO_PULLUP_ENABLE,

        .pull_down_en = GPIO_PULLDOWN_DISABLE,

        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(
        gpio_config(&botones)
    );

    gpio_config_t leds = {
        .pin_bit_mask =
            (1ULL << PIN_LED_A) |
            (1ULL << PIN_LED_B),

        .mode = GPIO_MODE_OUTPUT,

        .pull_up_en = GPIO_PULLUP_DISABLE,

        .pull_down_en = GPIO_PULLDOWN_DISABLE,

        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(
        gpio_config(&leds)
    );

    apagar_led_a();
    apagar_led_b();
}

// ==========================================================
// PROGRAMA PRINCIPAL
// ==========================================================

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "----- JUEGO DE VELOCIDAD DE REACCION -----"
    );

    // Inicializar memoria no volatil
    esp_err_t resultado_nvs =
        nvs_flash_init();

    if (resultado_nvs ==
            ESP_ERR_NVS_NO_FREE_PAGES ||
        resultado_nvs ==
            ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        ESP_ERROR_CHECK(
            nvs_flash_init()
        );
    }

    // Configurar entradas y salidas
    configurar_gpio();

    // Conectar a WiFi
    iniciar_wifi();

    // Iniciar MQTT
    iniciar_mqtt();

    // Semilla aleatoria
    uint32_t semilla =
        esp_random();

    srand(semilla);

    while (1)
    {
        int64_t reloj_actual =
            esp_timer_get_time();

        // ----------------------------------------------
        // Leer ambos botones
        // ----------------------------------------------

        bool boton_a_activo =
            procesar_boton(
                PIN_BOTON_A,
                &boton_a,
                reloj_actual
            );

        bool boton_b_activo =
            procesar_boton(
                PIN_BOTON_B,
                &boton_b,
                reloj_actual
            );

        // ----------------------------------------------
        // Detectar pulsacion del boton A
        // ----------------------------------------------

        bool pulso_a =
            boton_a_activo &&
            !boton_a.estado_previo;

        bool liberar_a =
            !boton_a_activo &&
            boton_a.estado_previo;

        boton_a.estado_previo =
            boton_a_activo;

        // ----------------------------------------------
        // Detectar pulsacion del boton B
        // ----------------------------------------------

        bool pulso_b =
            boton_b_activo &&
            !boton_b.estado_previo;

        boton_b.estado_previo =
            boton_b_activo;

        // ==============================================
        // MAQUINA DE ESTADOS
        // ==============================================

        switch (estado_actual)
        {
            // ------------------------------------------
            // ESTADO INICIAL
            // ------------------------------------------

            case INICIO:

                apagar_led_a();
                apagar_led_b();

                if (pulso_a)
                {
                    ESP_LOGI(
                        TAG,
                        "Boton A detectado. No lo sueltes."
                    );

                    inicio_espera =
                        reloj_actual;

                    // Entre 1.5 y 4.5 segundos
                    retraso_generado =
                        1500000 +
                        (esp_random() % 3000001);

                    estado_actual =
                        ESPERA_A;
                }

                break;

            // ------------------------------------------
            // ESPERANDO LA SEÑAL
            // ------------------------------------------

            case ESPERA_A:

                if (liberar_a)
                {
                    ESP_LOGI(
                        TAG,
                        "El boton fue liberado antes de la señal."
                    );

                    estado_actual =
                        INICIO;

                    break;
                }

                if ((reloj_actual -
                     inicio_espera)
                    >= retraso_generado)
                {
                    encender_led_a();

                    tiempo_encendido =
                        reloj_actual;

                    ESP_LOGI(
                        TAG,
                        "SEÑAL ACTIVADA. Suelta el boton A."
                    );

                    estado_actual =
                        LED_ACTIVO;
                }

                break;

            // ------------------------------------------
            // LED A ENCENDIDO
            // ------------------------------------------

            case LED_ACTIVO:

                if (liberar_a)
                {
                    tiempo_liberacion =
                        reloj_actual;

                    resultado_a =
                        (uint32_t)(
                            (tiempo_liberacion -
                             tiempo_encendido) / 1000
                        );

                    apagar_led_a();

                    ESP_LOGI(
                        TAG,
                        "Tiempo de respuesta 1: %" PRIu32
                        " ms",
                        resultado_a
                    );

                    ESP_LOGI(
                        TAG,
                        "Continua presionando el boton B."
                    );

                    estado_actual =
                        ESPERA_B;
                }

                break;

            // ------------------------------------------
            // ESPERANDO BOTON B
            // ------------------------------------------

            case ESPERA_B:

                if (pulso_b)
                {
                    int64_t instante_b =
                        reloj_actual;

                    resultado_b =
                        (uint32_t)(
                            (instante_b -
                             tiempo_liberacion) / 1000
                        );

                    encender_led_b();

                    ESP_LOGI(
                        TAG,
                        "Tiempo de respuesta 2: %" PRIu32
                        " ms",
                        resultado_b
                    );

                    estado_actual =
                        FINALIZADO;
                }

                break;

            // ------------------------------------------
            // FINAL DEL JUEGO
            // ------------------------------------------

            case FINALIZADO:

                enviar_resultados();

                vTaskDelay(
                    pdMS_TO_TICKS(2000)
                );

                apagar_led_b();

                ESP_LOGI(
                    TAG,
                    "Prueba completada. Regresando al inicio."
                );

                estado_actual =
                    INICIO;

                break;
        }

        // Pequeña pausa para no saturar la tarea
        vTaskDelay(
            pdMS_TO_TICKS(1)
        );
    }
}