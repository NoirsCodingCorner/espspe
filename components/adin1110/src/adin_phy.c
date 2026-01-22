#include "esp_eth_phy.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
// [NEU] Wir binden die echten Register ein
#include "adin1110_regs.h" 

static const char *TAG = "ADIN_PHY";

// Zugriff auf SPI-Read (muss in adin_mac.c oder adin_spi.c definiert sein)
extern uint32_t adin_read_reg(uint16_t reg);

typedef struct {
    esp_eth_phy_t parent;
    esp_eth_mediator_t *eth;
    bool link_status; 
} adin_phy_t;

// Dummy Handler gegen Crashes
static esp_err_t phy_dummy_handler(void) { return ESP_OK; }

static esp_err_t adin_phy_set_mediator(esp_eth_phy_t *phy, esp_eth_mediator_t *eth) {
    adin_phy_t *adin_phy = (adin_phy_t *)phy;
    adin_phy->eth = eth;
    return ESP_OK;
}

// --- ECHTE LINK CHECK FUNKTION ---
static esp_err_t adin_phy_get_link(esp_eth_phy_t *phy) {
    adin_phy_t *adin_phy = (adin_phy_t *)phy;
    esp_eth_mediator_t *eth = adin_phy->eth;

    // Wir nutzen jetzt die offiziellen Defines aus der .h Datei
    uint32_t status1 = adin_read_reg(ADIN1110_STATUS1_REG);
    bool is_up = (status1 & ADIN1110_LINK_STATE_MASK) ? true : false;

    if (is_up != adin_phy->link_status) {
        adin_phy->link_status = is_up;
        if (eth) {
            eth_link_t link_evt = is_up ? ETH_LINK_UP : ETH_LINK_DOWN;
            eth->on_state_changed(eth, ETH_STATE_LINK, (void *)link_evt);
        }
        ESP_LOGI(TAG, "PHY Status Change: %s (Reg 0x%02XVal: 0x%04lX)", 
                 is_up ? "UP" : "DOWN", ADIN1110_STATUS1_REG, status1);
    }
    return ESP_OK;
}

static esp_err_t adin_phy_set_addr(esp_eth_phy_t *phy, uint32_t addr) { return ESP_OK; }
static esp_err_t adin_phy_get_addr(esp_eth_phy_t *phy, uint32_t *addr) { *addr = 1; return ESP_OK; }
static esp_err_t adin_phy_reset(esp_eth_phy_t *phy) { return ESP_OK; }
static esp_err_t adin_phy_pwr(esp_eth_phy_t *phy, bool enable) { return ESP_OK; }
static esp_err_t adin_phy_set_link(esp_eth_phy_t *phy, eth_link_t link) { 
    adin_phy_t *adin_phy = (adin_phy_t *)phy;
    adin_phy->link_status = (link == ETH_LINK_UP);
    return ESP_OK; 
}
static esp_err_t adin_phy_init(esp_eth_phy_t *phy) { return ESP_OK; }
static esp_err_t adin_phy_deinit(esp_eth_phy_t *phy) { return ESP_OK; }
static esp_err_t adin_phy_del(esp_eth_phy_t *phy) { free(phy); return ESP_OK; }
static esp_err_t adin_phy_loopback(esp_eth_phy_t *phy, bool enable) { return ESP_OK; }
static esp_err_t adin_phy_set_speed(esp_eth_phy_t *phy, eth_speed_t speed) { return ESP_OK; }
static esp_err_t adin_phy_set_duplex(esp_eth_phy_t *phy, eth_duplex_t duplex) { return ESP_OK; }
static esp_err_t adin_phy_autonego_ctrl(esp_eth_phy_t *phy, eth_phy_autoneg_cmd_t cmd, bool *autonego_en_stat) {
    if (autonego_en_stat) *autonego_en_stat = true; 
    return ESP_OK;
}
static esp_err_t adin_phy_advertise_pause_ability(esp_eth_phy_t *phy, uint32_t ability) { return ESP_OK; }
static esp_err_t adin_phy_custom_ioctl(esp_eth_phy_t *phy, int cmd, void *data) { return ESP_OK; }

esp_eth_phy_t* adin_phy_new(void) {
    adin_phy_t *adin_phy = calloc(1, sizeof(adin_phy_t));
    if (!adin_phy) return NULL;

    adin_phy->link_status = false;

    adin_phy->parent.set_addr = adin_phy_set_addr;
    adin_phy->parent.get_addr = adin_phy_get_addr;
    adin_phy->parent.reset = adin_phy_reset;
    adin_phy->parent.init = adin_phy_init;
    adin_phy->parent.deinit = adin_phy_deinit;
    adin_phy->parent.del = adin_phy_del;
    adin_phy->parent.get_link = adin_phy_get_link;
    adin_phy->parent.pwrctl = adin_phy_pwr;
    adin_phy->parent.set_link = adin_phy_set_link;
    adin_phy->parent.autonego_ctrl = adin_phy_autonego_ctrl;
    adin_phy->parent.loopback = adin_phy_loopback;
    adin_phy->parent.set_speed = adin_phy_set_speed;
    adin_phy->parent.set_duplex = adin_phy_set_duplex;
    adin_phy->parent.set_mediator = adin_phy_set_mediator;
    adin_phy->parent.advertise_pause_ability = adin_phy_advertise_pause_ability;
    adin_phy->parent.custom_ioctl = adin_phy_custom_ioctl;

    // Loch Patcher
    void **func_table = (void**)&adin_phy->parent;
    int num_slots = sizeof(esp_eth_phy_t) / sizeof(void*);
    for (int i = 0; i < num_slots; i++) {
        if (func_table[i] == NULL) func_table[i] = (void*)phy_dummy_handler;
    }

    return &adin_phy->parent;
}