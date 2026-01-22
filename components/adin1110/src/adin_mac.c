#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "adin1110.h"
#include "adin1110_regs.h"
#include "esp_eth_netif_glue.h"
#include "esp_netif.h" 

static const char *TAG = "ADIN_MAC";

// --- EXTERNS ---
extern void adin_write_reg(uint16_t reg, uint32_t val);
extern uint32_t adin_read_reg(uint16_t reg);
extern void adin_write_fifo(const uint8_t *data, uint32_t len);
extern void adin_read_fifo(uint8_t *dest, uint32_t len);
extern esp_err_t adin_spi_init(spi_host_device_t host, int cs_pin, int freq);
extern esp_eth_phy_t* adin_phy_new(void);

// --- MISSING REGISTER DEFINITIONS (Backup) ---
#ifndef ADIN1110_PHY_SUBSYS_IRQ_STATUS_REG
#define ADIN1110_PHY_SUBSYS_IRQ_STATUS_REG  0x001F
#endif
#ifndef ADIN1110_CRSM_IRQ_STATUS_REG
#define ADIN1110_CRSM_IRQ_STATUS_REG        0x0020
#endif
#ifndef ADIN1110_IMASK0_REG
#define ADIN1110_IMASK0_REG                 0x000C
#endif

// --- STATISTICS CONTAINER ---
typedef struct {
    uint32_t irq_count;         // Hardware ISRs triggered
    uint32_t task_wakeups;      // Task loop cycles (IRQ + Timeout)
    uint32_t rx_pkg;            // Successfully passed to LwIP
    uint32_t rx_err;            // LwIP / Malloc errors
    uint32_t phy_clears;        // How often "Stuck 0x80" was cleared
    uint32_t spi_err;           // STATUS0 error flags
    uint32_t last_status0;      // Last read value of STATUS0
    uint32_t last_status1;      // Last read value of STATUS1
} adin_stats_t;

typedef struct {
    esp_eth_mac_t parent;
    esp_eth_phy_t *phy;
    esp_eth_mediator_t *eth; 
    int int_pin;
    TaskHandle_t rx_task_hdl;
    esp_netif_t *netif;
    uint8_t addr[6];
    adin_stats_t stats;
} adin_mac_t;

// --- ISR HANDLER ---
static void IRAM_ATTR adin1110_isr_handler(void *arg) {
    adin_mac_t *emac = (adin_mac_t *)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    // Increment atomic counter (for debug)
    emac->stats.irq_count++;

    if (emac->rx_task_hdl != NULL) {
        vTaskNotifyGiveFromISR(emac->rx_task_hdl, &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

// --- HELPER: CLEAN SWEEP (Deep Research Fix) ---
// Clears the underlying source of the 0x80 Status Bit
static void adin_clear_pending_irqs(adin_mac_t *emac) {
    uint32_t phy_status = adin_read_reg(ADIN1110_PHY_SUBSYS_IRQ_STATUS_REG);
    uint32_t crsm_status = adin_read_reg(ADIN1110_CRSM_IRQ_STATUS_REG);
    
    if (phy_status || crsm_status) {
        emac->stats.phy_clears++;
    }
}

// --- RX TASK (With Periodic Stats) ---
static void rx_task(void *arg) {
    adin_mac_t *emac = (adin_mac_t *)arg;
    ESP_LOGI(TAG, "RX Task Started (Stats Monitor Active)");

    TickType_t last_stat_time = xTaskGetTickCount();
    const TickType_t stat_interval = pdMS_TO_TICKS(5000); // Print stats every 5s

    while (1) {
        // Hybrid Polling: Wait for IRQ, but timeout every 50ms to check anyway
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        emac->stats.task_wakeups++;

        // --- STATS MONITORING ---
        if ((xTaskGetTickCount() - last_stat_time) > stat_interval) {
            ESP_LOGI(TAG, "[STATS] IRQ: %lu | Wakeups: %lu | RX_OK: %lu | RX_Err: %lu | PHY_Clr: %lu | SPI_Err: %lu | S0: 0x%02lX | S1: 0x%02lX",
                     emac->stats.irq_count,
                     emac->stats.task_wakeups,
                     emac->stats.rx_pkg,
                     emac->stats.rx_err,
                     emac->stats.phy_clears,
                     emac->stats.spi_err,
                     emac->stats.last_status0,
                     emac->stats.last_status1);
            last_stat_time = xTaskGetTickCount();
        }

        // 1. STATUS0 Logic (PHY Interrupts & Errors)
        uint32_t status0 = adin_read_reg(ADIN1110_STATUS0_REG);
        emac->stats.last_status0 = status0;

        if (status0 & 0x80) { // PHYINT (Bit 7)
            adin_clear_pending_irqs(emac);
        }
        
        // Check for other errors in STATUS0 (Bits 0-5)
        if (status0 & 0x3F) {
            emac->stats.spi_err++;
            adin_write_reg(ADIN1110_STATUS0_REG, status0); // W1C
            ESP_LOGW(TAG, "STATUS0 Error detected: 0x%08lX", status0);
        }

        // 2. STATUS1 Logic (MAC Events)
        uint32_t status1 = adin_read_reg(ADIN1110_STATUS1_REG);
        emac->stats.last_status1 = status1;
        
        if (status1) {
            adin_write_reg(ADIN1110_STATUS1_REG, status1); // Clear MAC IRQs
        }

        // 3. RX Processing Loop
        while (1) {
            uint32_t fsize = adin_read_reg(ADIN1110_RX_FSIZE_REG);
            
            if (fsize < 2) break; // Empty

            // Sanity Check for corrupted Frame Size (SPI Garbage)
            if (fsize > 1530) {
                ESP_LOGE(TAG, "Corrupt Frame Size: %lu (Flushing)", fsize);
                uint8_t *dummy = malloc(fsize);
                if(dummy) { adin_read_fifo(dummy, fsize); free(dummy); }
                break;
            }

            uint8_t *raw_buf = malloc(fsize);
            if (raw_buf) {
                adin_read_fifo(raw_buf, fsize);
                
                if (emac->netif) {
                    uint32_t payload_len = fsize - 2;
                    void *payload_buf = malloc(payload_len);
                    if (payload_buf) {
                        memcpy(payload_buf, &raw_buf[2], payload_len);
                        
                        // Pass to LwIP
                        esp_err_t err = esp_netif_receive(emac->netif, payload_buf, payload_len, NULL);
                        if (err == ESP_OK) {
                            emac->stats.rx_pkg++;
                        } else {
                            // LwIP full or error
                            free(payload_buf);
                            emac->stats.rx_err++;
                        }
                    } else {
                        emac->stats.rx_err++;
                    }
                }
                free(raw_buf);
            } else {
                emac->stats.rx_err++;
                break; 
            }
        }
    }
}

static esp_err_t emac_init(esp_eth_mac_t *mac) {
    adin_mac_t *emac = (adin_mac_t *)mac;
    ESP_LOGI(TAG, "Initializing ADIN1110 MAC...");

    // 1. Reset
    adin_write_reg(ADIN1110_RESET_REG, ADIN1110_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2. Clear STATUS0 initial state
    adin_write_reg(ADIN1110_STATUS0_REG, 0xFFFFFFFF);

    // 3. Config
    adin_write_reg(ADIN1110_CONFIG1_REG, ADIN1110_CONFIG1_SYNC);
    // Enable CRC on Ethernet Frames (not SPI) + Fwd Unknown
    uint32_t cfg2 = ADIN1110_CRC_APPEND | ADIN1110_FWD_UNK2HOST_MASK;
    adin_write_reg(ADIN1110_CONFIG2_REG, cfg2);
    
    // 4. Promiscuous Mode (Allow ARP)
    adin_write_reg(ADIN1110_MAC_ADDR_MASK_UPR_REG, 0x00000000); 
    adin_write_reg(ADIN1110_MAC_ADDR_MASK_LWR_REG, 0x00000000);

    // 5. INTERRUPT SETUP (Deep Research Fix)
    // A) Mask Everything (1 = Masked)
    adin_write_reg(ADIN1110_IMASK0_REG, 0xFFFFFFFF);
    adin_write_reg(ADIN1110_IMASK1_REG, 0xFFFFFFFF);
    
    // B) Clean Sweep pending PHY IRQs
    adin_clear_pending_irqs(emac);

    // C) Enable RX_RDY (Bit 0) and LINK_CHANGE (Bit 4)
    // 0 = Enable. We need to write ~0x11.
    uint32_t enable_mask = ~((uint32_t)(ADIN1110_RX_RDY_IRQ | ADIN1110_LINK_STATE_MASK));
    adin_write_reg(ADIN1110_IMASK1_REG, enable_mask);
    
    // D) Verify Readback (Log Warning if Garbage, but proceed)
    uint32_t imask_read = adin_read_reg(ADIN1110_IMASK1_REG);
    if ((imask_read & 0xFFFF0000) != 0) {
        ESP_LOGW(TAG, "IMASK1 Readback suspicious (Garbage?): 0x%08lX. Assuming Write worked.", imask_read);
    } else {
        ESP_LOGI(TAG, "IMASK1 Configured: 0x%08lX", imask_read);
    }

    // E) Final Check
    uint32_t status0 = adin_read_reg(ADIN1110_STATUS0_REG);
    if (status0 & 0x80) {
        ESP_LOGW(TAG, "STATUS0 0x80 active after init. Clearing again...");
        adin_clear_pending_irqs(emac);
    }

    return ESP_OK;
}

static esp_err_t emac_transmit(esp_eth_mac_t *mac, uint8_t *buf, uint32_t length) {
    uint32_t fsize = length + 2; 
    uint8_t *tx_buf = malloc(fsize);
    if (!tx_buf) return ESP_ERR_NO_MEM;
    
    tx_buf[0] = 0; tx_buf[1] = 0;
    memcpy(&tx_buf[2], buf, length);
    
    adin_write_fifo(tx_buf, fsize);
    adin_write_reg(ADIN1110_TX_FSIZE_REG, fsize);
    
    free(tx_buf);
    return ESP_OK;
}

// --- BOILERPLATE ---
static esp_err_t emac_dummy_handler(void) { return ESP_OK; }
static esp_err_t emac_set_mediator(esp_eth_mac_t *mac, esp_eth_mediator_t *eth) {
    adin_mac_t *emac = (adin_mac_t *)mac; emac->eth = eth; return ESP_OK;
}
static esp_err_t emac_set_addr(esp_eth_mac_t *mac, uint8_t *addr) {
    adin_mac_t *emac = (adin_mac_t *)mac; memcpy(emac->addr, addr, 6); return ESP_OK;
}
static esp_err_t emac_get_addr(esp_eth_mac_t *mac, uint8_t *addr) {
    adin_mac_t *emac = (adin_mac_t *)mac; memcpy(addr, emac->addr, 6); return ESP_OK;
}
static esp_err_t emac_start(esp_eth_mac_t *mac) { return ESP_OK; }
static esp_err_t emac_stop(esp_eth_mac_t *mac) { return ESP_OK; }
static esp_err_t emac_deinit(esp_eth_mac_t *mac) { return ESP_OK; }
static esp_err_t emac_del(esp_eth_mac_t *mac) { free(mac); return ESP_OK; }
static esp_err_t emac_receive(esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length) { return ESP_OK; }
static esp_err_t emac_read_phy_reg(esp_eth_mac_t *mac, uint32_t phy, uint32_t reg, uint32_t *val) { *val=0; return ESP_OK; }
static esp_err_t emac_write_phy_reg(esp_eth_mac_t *mac, uint32_t phy, uint32_t reg, uint32_t val) { return ESP_OK; }
static esp_err_t emac_set_speed(esp_eth_mac_t *mac, eth_speed_t speed) { return ESP_OK; }
static esp_err_t emac_set_duplex(esp_eth_mac_t *mac, eth_duplex_t duplex) { return ESP_OK; }
static esp_err_t emac_set_link(esp_eth_mac_t *mac, eth_link_t link) { return ESP_OK; }
static esp_err_t emac_set_promiscuous(esp_eth_mac_t *mac, bool enable) { return ESP_OK; }
static esp_err_t emac_enable_flow_ctrl(esp_eth_mac_t *mac, bool enable) { return ESP_OK; } 
static esp_err_t emac_custom_ioctl(esp_eth_mac_t *mac, int cmd, void *data) { return ESP_OK; }
static esp_err_t emac_set_peer_pause_ability(esp_eth_mac_t *mac, uint32_t ability) { return ESP_OK; }

esp_err_t adin1110_init(const adin_config_t *config, esp_netif_t **netif_out) {
    adin_mac_t *emac = calloc(1, sizeof(adin_mac_t));
    if (!emac) return ESP_ERR_NO_MEM;

    emac->int_pin = config->int_pin;
    memcpy(emac->addr, config->mac_addr, 6);
    
    adin_spi_init(config->spi_host, config->cs_pin, config->spi_freq_hz);
    
    emac->parent.init = emac_init;
    emac->parent.transmit = emac_transmit;
    emac->parent.set_mediator = emac_set_mediator;
    emac->parent.set_addr = emac_set_addr;
    emac->parent.get_addr = emac_get_addr;
    emac->parent.deinit = emac_deinit;
    emac->parent.start = emac_start;
    emac->parent.stop = emac_stop;
    emac->parent.del = emac_del;
    emac->parent.receive = emac_receive;
    emac->parent.read_phy_reg = emac_read_phy_reg;
    emac->parent.write_phy_reg = emac_write_phy_reg;
    emac->parent.set_speed = emac_set_speed;
    emac->parent.set_duplex = emac_set_duplex;
    emac->parent.set_link = emac_set_link;
    emac->parent.set_promiscuous = emac_set_promiscuous;
    emac->parent.enable_flow_ctrl = emac_enable_flow_ctrl;
    emac->parent.custom_ioctl = emac_custom_ioctl; 
    emac->parent.set_peer_pause_ability = emac_set_peer_pause_ability;

    void **func_table = (void**)&emac->parent;
    int num_slots = sizeof(esp_eth_mac_t) / sizeof(void*);
    for (int i = 0; i < num_slots; i++) {
        if (func_table[i] == NULL) func_table[i] = (void*)emac_dummy_handler;
    }

    esp_eth_phy_t *phy = adin_phy_new();
    emac->phy = phy;

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(&emac->parent, phy);
    esp_eth_handle_t eth_handle;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    xTaskCreate(rx_task, "adin_rx", 4096, emac, configMAX_PRIORITIES - 2, &emac->rx_task_hdl);

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.pin_bit_mask = (1ULL << emac->int_pin);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1; 
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(emac->int_pin, adin1110_isr_handler, (void*)emac);

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    void *glue = esp_eth_new_netif_glue(eth_handle);
    esp_netif_attach(eth_netif, glue);
    emac->netif = eth_netif;

    esp_eth_start(eth_handle);
    *netif_out = eth_netif;
    return ESP_OK;
}