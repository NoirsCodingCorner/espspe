#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "MAIN";

void app_main(void) {
    int i = 0;
    while (1) {
        // ESP_LOGI ist besser als printf, da es Zeitstempel und Tags hinzufügt
        ESP_LOGI(TAG, "Hello World aus ESP-IDF! Zähler: %d", i++);
        
        // Nicht blockieren! 1000ms warten
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}