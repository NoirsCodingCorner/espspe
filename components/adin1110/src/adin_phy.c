/* components/adin1110/src/adin_phy.c */
#include "esp_eth_phy.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ADIN_PHY";

typedef struct {
    esp_eth_phy_t parent;
    // Hier könnten wir eigene Variablen speichern
} adin_phy_t;

static esp_err_t adin_phy_set_addr(esp_eth_phy_t *phy, uint32_t addr) { return ESP_OK; }
static esp_err_t adin_phy_get_addr(esp_eth_phy_t *phy, uint32_t *addr) { *addr = 1; return ESP_OK; }
static esp_err_t adin_phy_reset(esp_eth_phy_t *phy) { return ESP_OK; }
// negotiate existiert in v5 nicht mehr!
static esp_err_t adin_phy_pwr(esp_eth_phy_t *phy, bool enable) { return ESP_OK; }
static esp_err_t adin_phy_set_link(esp_eth_phy_t *phy, eth_link_t link) { return ESP_OK; }

static esp_err_t adin_phy_get_link(esp_eth_phy_t *phy) {
    // Da wir keine MDIO Verbindung haben, faken wir "Link Up",
    // damit der ESP-Netif Stack weitermacht.
    // In einer echten Implementierung würden wir hier ein Flag prüfen,
    // das vom MAC-Interrupt gesetzt wurde.
    return ESP_OK; // Return OK = Link Up (meistens)
}

static esp_err_t adin_phy_init(esp_eth_phy_t *phy) { 
    ESP_LOGI(TAG, "PHY Init");
    return ESP_OK; 
}
static esp_err_t adin_phy_deinit(esp_eth_phy_t *phy) { return ESP_OK; }
static esp_err_t adin_phy_del(esp_eth_phy_t *phy) { free(phy); return ESP_OK; }

static esp_err_t adin_phy_autonego_ctrl(esp_eth_phy_t *phy, eth_phy_autoneg_cmd_t cmd, bool *autonego_en_stat) {
    if (autonego_en_stat) *autonego_en_stat = true; 
    return ESP_OK;
}

static esp_err_t adin_phy_loopback(esp_eth_phy_t *phy, bool enable) { return ESP_OK; }
static esp_err_t adin_phy_set_speed(esp_eth_phy_t *phy, eth_speed_t speed) { return ESP_OK; }
static esp_err_t adin_phy_set_duplex(esp_eth_phy_t *phy, eth_duplex_t duplex) { return ESP_OK; }

esp_eth_phy_t* adin_phy_new(void) {
    adin_phy_t *adin_phy = calloc(1, sizeof(adin_phy_t));
    if (!adin_phy) return NULL;

    adin_phy->parent.set_addr = adin_phy_set_addr;
    adin_phy->parent.get_addr = adin_phy_get_addr;
    adin_phy->parent.reset = adin_phy_reset;
    adin_phy->parent.init = adin_phy_init;
    adin_phy->parent.deinit = adin_phy_deinit;
    adin_phy->parent.del = adin_phy_del;
    // adin_phy->parent.negotiate = ...; // ENTFERNT für ESP-IDF v5 compatibility
    adin_phy->parent.get_link = adin_phy_get_link;
    adin_phy->parent.pwrctl = adin_phy_pwr;
    adin_phy->parent.set_link = adin_phy_set_link;
    adin_phy->parent.autonego_ctrl = adin_phy_autonego_ctrl;
    adin_phy->parent.loopback = adin_phy_loopback;
    adin_phy->parent.set_speed = adin_phy_set_speed;
    adin_phy->parent.set_duplex = adin_phy_set_duplex;

    return &adin_phy->parent;
}