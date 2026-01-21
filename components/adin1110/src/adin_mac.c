#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_driver.h" 
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "adin1110.h"
#include "adin1110_regs.h"
#include "esp_eth_netif_glue.h"

static const char *TAG = "ADIN_MAC";

// --- EXTERNE FUNKTIONEN ---
extern void adin_write_reg(uint16_t reg, uint32_t val);
extern uint32_t adin_read_reg(uint16_t reg);
extern void adin_write_fifo(const uint8_t *data, uint32_t len);
extern void adin_read_fifo(uint8_t *dest, uint32_t len);
extern esp_err_t adin_spi_init(spi_host_device_t host, int cs_pin, int freq);
extern esp_eth_phy_t* adin_phy_new(void);

// --- WORKAROUND FÜR COMPILER SICHTBARKEIT ---
// Manchmal wird der Header esp_eth_driver.h nicht korrekt geparst.
// Wir sagen dem Compiler manuell, dass diese Funktion existiert.
esp_err_t esp_eth_process_frame(esp_eth_handle_t eth_handle);


// --- STRUKTUREN ---
typedef struct {
    esp_eth_mac_t parent;
    esp_eth_phy_t *phy;
    int int_pin;
    TaskHandle_t rx_task_hdl; 
} adin_mac_t;

// --- ISR HANDLER ---
static void IRAM_ATTR isr_handler(void *arg) {
    TaskHandle_t task = (TaskHandle_t)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (task) {
        vTaskNotifyGiveFromISR(task, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
    }
}

// --- RX TASK ---
static void rx_task(void *arg) {
    esp_eth_handle_t eth_handle = (esp_eth_handle_t)arg;
    
    while (1) {
        // Warte auf Benachrichtigung vom ISR
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // 1. Interrupt Clear am Chip
        uint32_t status = adin_read_reg(ADIN1110_STATUS1_REG);
        adin_write_reg(ADIN1110_STATUS1_REG, status); // Write 1 to clear

        // 2. Prüfen ob Daten da sind
        uint32_t fsize = adin_read_reg(ADIN1110_RX_FSIZE_REG);
        if (fsize > 2) {
            // Sagt dem ESP-IDF Stack, dass er emac_receive aufrufen soll
            esp_eth_process_frame(eth_handle);
        }
    }
}

// --- MAC IMPLEMENTIERUNG ---

static esp_err_t emac_transmit(esp_eth_mac_t *mac, uint8_t *buf, uint32_t length) {
    uint32_t fsize = length + 2;
    adin_write_reg(ADIN1110_TX_FSIZE_REG, fsize);
    
    uint8_t *tx_buf = malloc(fsize);
    if (!tx_buf) return ESP_ERR_NO_MEM;

    tx_buf[0] = 0; tx_buf[1] = 0; // Internal Header
    memcpy(&tx_buf[2], buf, length);
    
    adin_write_fifo(tx_buf, fsize);
    free(tx_buf);
    return ESP_OK;
}

static esp_err_t emac_receive(esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length) {
    uint32_t fsize = adin_read_reg(ADIN1110_RX_FSIZE_REG);
    if (fsize < 2) return ESP_ERR_INVALID_SIZE;
    
    uint8_t *temp = malloc(fsize);
    if (!temp) return ESP_ERR_NO_MEM;

    adin_read_fifo(temp, fsize);
    
    uint32_t real_len = fsize - 2;
    if (*length < real_len) { 
        free(temp); 
        *length = real_len;
        return ESP_ERR_INVALID_SIZE; 
    }
    
    memcpy(buf, &temp[2], real_len);
    *length = real_len;
    free(temp);
    
    return ESP_OK;
}

static esp_err_t emac_init(esp_eth_mac_t *mac) {
    // adin_mac_t *priv = (adin_mac_t *)mac; // [FIX] Variable entfernt, da unbenutzt
    ESP_LOGI(TAG, "Initializing ADIN1110 MAC...");

    // Reset Sequence
    adin_write_reg(ADIN1110_RESET_REG, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Config: CRC Append + Promiscuous
    uint32_t cfg2 = adin_read_reg(ADIN1110_CONFIG2_REG);
    cfg2 |= ADIN1110_CRC_APPEND;
    cfg2 |= ADIN1110_FWD_UNK2HOST_MASK; 
    adin_write_reg(ADIN1110_CONFIG2_REG, cfg2);
    
    // Interrupt Unmask (Enable)
    // 0 = unmasked (enabled)
    uint32_t mask = 0xFFFFFFFF;
    mask &= ~ADIN1110_RX_RDY_IRQ;
    adin_write_reg(ADIN1110_IMASK1_REG, mask);

    return ESP_OK;
}

static esp_err_t emac_stop(esp_eth_mac_t *mac) { return ESP_OK; }
static esp_err_t emac_del(esp_eth_mac_t *mac) { free(mac); return ESP_OK; }
static esp_err_t emac_set_addr(esp_eth_mac_t *mac, uint8_t *addr) { return ESP_OK; }
static esp_err_t emac_get_addr(esp_eth_mac_t *mac, uint8_t *addr) { return ESP_OK; }
static esp_err_t emac_set_link(esp_eth_mac_t *mac, eth_link_t link) { return ESP_OK; }
static esp_err_t emac_set_speed(esp_eth_mac_t *mac, eth_speed_t speed) { return ESP_OK; }
static esp_err_t emac_set_duplex(esp_eth_mac_t *mac, eth_duplex_t duplex) { return ESP_OK; }
static esp_err_t emac_set_promiscuous(esp_eth_mac_t *mac, bool enable) { return ESP_OK; }

// --- INIT MAIN ---

esp_err_t adin1110_init(const adin_config_t *config, esp_netif_t **netif_out) {
    adin_mac_t *emac = calloc(1, sizeof(adin_mac_t));
    emac->int_pin = config->int_pin;
    
    adin_spi_init(config->spi_host, config->cs_pin, config->spi_freq_hz);
    
    emac->parent.init = emac_init;
    emac->parent.transmit = emac_transmit;
    emac->parent.receive = emac_receive;
    emac->parent.stop = emac_stop;
    emac->parent.del = emac_del;
    emac->parent.set_addr = emac_set_addr;
    emac->parent.get_addr = emac_get_addr;
    emac->parent.set_link = emac_set_link;
    emac->parent.set_speed = emac_set_speed;
    emac->parent.set_duplex = emac_set_duplex;
    emac->parent.set_promiscuous = emac_set_promiscuous;

    esp_eth_phy_t *phy = adin_phy_new();
    emac->phy = phy;

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(&emac->parent, phy);
    esp_eth_handle_t eth_handle;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    // RX Task erstellen
    xTaskCreate(rx_task, "adin_rx", 4096, (void*)eth_handle, 15, &emac->rx_task_hdl);

    // ISR
    gpio_install_isr_service(0);
    gpio_set_intr_type(emac->int_pin, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(emac->int_pin, isr_handler, (void*)emac->rx_task_hdl);

    // Netif
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    void *glue = esp_eth_new_netif_glue(eth_handle);
    esp_netif_attach(eth_netif, glue);

    esp_eth_start(eth_handle);

    *netif_out = eth_netif;
    return ESP_OK;
}