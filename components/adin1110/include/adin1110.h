#pragma once
#include "esp_eth.h"
#include "esp_netif.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t spi_host;
    int cs_pin;
    int int_pin; // GPIO 32
    int spi_freq_hz;
    uint8_t mac_addr[6]; // Wir übergeben die MAC explizit
} adin_config_t;

/**
 * @brief Erstellt den Treiber und registriert ihn im System
 */
esp_err_t adin1110_init(const adin_config_t *config, esp_netif_t **netif_out);

#ifdef __cplusplus
}
#endif