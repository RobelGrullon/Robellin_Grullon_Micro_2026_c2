#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

// Definir pines (ajusta según tu placa)
#define LED_PIN 2       // GPIO donde está conectado el LED
#define BUTTON_PIN 0    // GPIO donde está el botón

// Estados
typedef enum {
    STATE_OFF,
    STATE_ON
} state_t;

state_t current_state = STATE_OFF;

void app_main(void) {
    // Configurar LED como salida
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    // Configurar botón como entrada con pull-up
    gpio_reset_pin(BUTTON_PIN);
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_pullup_en(BUTTON_PIN);

    while (1) {
        // Leer botón
        int button_state = gpio_get_level(BUTTON_PIN);

        if (button_state == 0) { // Botón presionado
            // Cambiar estado
            current_state = (current_state == STATE_OFF) ? STATE_ON : STATE_OFF;
            vTaskDelay(pdMS_TO_TICKS(300)); // Anti-rebote
        }

        // Actualizar LED según estado
        if (current_state == STATE_ON) {
            gpio_set_level(LED_PIN, 1);
        } else {
            gpio_set_level(LED_PIN, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // Pequeño delay
    }
}
