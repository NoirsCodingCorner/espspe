#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "adin1110.h"

static const char *TAG = "APP";

// --- ROLLE WÄHLEN ---
// Setze dies auf 1 für den ersten ESP (Empfänger)
// Setze dies auf 0 für den zweiten ESP (Sender)
#define IS_SERVER  1

// --- KONFIGURATION ---
#if IS_SERVER
    #define DEVICE_IP       "192.168.1.101"
    #define TARGET_IP       "192.168.1.102" // Egal für Server
    #define MY_ROLE         "SERVER (Receiver)"
#else
    #define DEVICE_IP       "192.168.1.102"
    #define TARGET_IP       "192.168.1.101" // Ziel-Adresse des Servers
    #define MY_ROLE         "CLIENT (Sender)"
#endif

#define PORT            3333
#define DEVICE_GW       "192.168.1.1"
#define DEVICE_NETMASK  "255.255.255.0"

// Pinout
#define PIN_CS   5
#define PIN_INT  32
#define PIN_RST  4
#define PIN_MOSI 23
#define PIN_MISO 19
#define PIN_SCK  18

// --- UDP TASKS ---

void udp_server_task(void *pvParameters) {
    char rx_buffer[128];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;

    while (1) {
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PORT);

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        } else {
            ESP_LOGI(TAG, "Socket bound, waiting for data on Port %d...", PORT);
            
            struct sockaddr_storage source_addr;
            socklen_t socklen = sizeof(source_addr);

            while (1) {
                int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
                if (len < 0) {
                    ESP_LOGE(TAG, "Recvfrom failed: errno %d", errno);
                    break;
                } else {
                    rx_buffer[len] = 0; // Null-terminate
                    ESP_LOGI(TAG, "RECEIVED %d bytes: '%s'", len, rx_buffer);
                    // Hier könntest du eine LED blinken lassen!
                }
            }
        }

        if (sock != -1) {
            shutdown(sock, 0);
            close(sock);
        }
    }
}

void udp_client_task(void *pvParameters) {
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    int count = 0;
    
    // Anti-Spam Variablen
    int last_errno = 0;
    int errno_counter = 0;

    while (1) {
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = inet_addr(TARGET_IP);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PORT);

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        while (1) {
            char payload[64];
            snprintf(payload, sizeof(payload), "Hello SPE #%d", count++);
            
            int err = sendto(sock, payload, strlen(payload), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            
            if (err < 0) {
                // ANTI-SPAM LOGIC
                if (errno != last_errno || errno_counter > 5) {
                    ESP_LOGE(TAG, "Send Error: errno %d (%s)", errno, strerror(errno));
                    last_errno = errno;
                    errno_counter = 0;
                } else {
                    errno_counter++;
                    // Kleiner Punkt im Log um zu zeigen "es lebt noch, aber Fehler"
                    printf("."); 
                    fflush(stdout);
                }
            } else {
                // Erfolg! Reset Fehlerzähler
                if (last_errno != 0) {
                    printf("\n"); // Neue Zeile nach den Punkten
                    last_errno = 0;
                }
                ESP_LOGI(TAG, "Message sent to %s: %s", TARGET_IP, payload);
            }
            
            vTaskDelay(1000 / portTICK_PERIOD_MS); 
        }

        if (sock != -1) {
            shutdown(sock, 0);
            close(sock);
        }
    }
}

// --- SETUP ---

void set_static_ip(esp_netif_t *netif) {
    if (esp_netif_dhcpc_stop(netif) != ESP_OK) {
        ESP_LOGW(TAG, "DHCP client not running or failed to stop");
    }

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));
    esp_netif_str_to_ip4(DEVICE_IP, &ip_info.ip);
    esp_netif_str_to_ip4(DEVICE_GW, &ip_info.gw);
    esp_netif_str_to_ip4(DEVICE_NETMASK, &ip_info.netmask);

    esp_netif_set_ip_info(netif, &ip_info);
    ESP_LOGI(TAG, "Static IP configured: %s", DEVICE_IP);
}

void app_main(void) {
    printf("\n\n=== ROLE: %s ===\n", MY_ROLE);
    vTaskDelay(pdMS_TO_TICKS(2000));

    // NVS & Netif
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // SPI Init
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Reset Sequence
    gpio_reset_pin(PIN_RST);
    gpio_set_direction(PIN_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    // ADIN Config
    adin_config_t adin_cfg = {
        .spi_host = SPI2_HOST,
        .cs_pin = PIN_CS,
        .int_pin = PIN_INT,
        .spi_freq_hz = 15000000,
    };
    
    // MAC Address Generation (Simpel)
    esp_read_mac(adin_cfg.mac_addr, ESP_MAC_WIFI_STA);
    #if IS_SERVER
        adin_cfg.mac_addr[5] = 0xAA; // Endung AA für Server
    #else
        adin_cfg.mac_addr[5] = 0xBB; // Endung BB für Client
    #endif

    // Driver Start
    esp_netif_t *eth_netif = NULL;
    ESP_ERROR_CHECK(adin1110_init(&adin_cfg, &eth_netif));

    // IP Config
    set_static_ip(eth_netif);

    // Warten bis alles stabil ist
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Task Starten
    #if IS_SERVER
        xTaskCreate(udp_server_task, "udp_server", 4096, NULL, 5, NULL);
    #else
        xTaskCreate(udp_client_task, "udp_client", 4096, NULL, 5, NULL);
    #endif
}