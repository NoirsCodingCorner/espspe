#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include "adin1110_regs.h" 

static const char *TAG = "ADIN_SPI";
static spi_device_handle_t spi_handle;
static SemaphoreHandle_t spi_mutex = NULL; // Globaler Mutex

// --- CRC BERECHNUNG ---
static uint8_t calculate_crc(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x07; else crc <<= 1;
        }
    }
    return crc;
}

esp_err_t adin_spi_init(spi_host_device_t host, int cs_pin, int freq) {
    esp_err_t ret;
    
    if (spi_mutex == NULL) {
        spi_mutex = xSemaphoreCreateMutex();
        if (!spi_mutex) return ESP_ERR_NO_MEM;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = freq,
        .mode = 0,
        .spics_io_num = cs_pin,
        .queue_size = 1,
        .flags = 0,
    };

    ret = spi_bus_add_device(host, &devcfg, &spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device");
        return ret;
    }
    ESP_LOGI(TAG, "SPI Initialized (Mode 0, %d Hz, CS=%d) with Mutex", freq, cs_pin);
    return ESP_OK;
}

// --- REGISTER READ (Generic SPI with 8-bit CRC + Turnaround) ---
uint32_t adin_read_reg(uint16_t reg) {
    uint32_t val = 0;
    
    xSemaphoreTake(spi_mutex, portMAX_DELAY);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    // Frame: [H1, H2, H_CRC, TA, D1, D2, D3, D4, D_CRC] = 9 Bytes
    uint8_t tx_buf[9] = {0};
    uint8_t rx_buf[9] = {0};

    uint16_t cmd = 0x8000 | (reg & 0x1FFF);
    tx_buf[0] = (cmd >> 8) & 0xFF;
    tx_buf[1] = cmd & 0xFF;
    tx_buf[2] = calculate_crc(tx_buf, 2);
    tx_buf[3] = 0x00; // Turnaround (Hi-Z)

    t.length = 9 * 8;
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;

    if (spi_device_transmit(spi_handle, &t) == ESP_OK) {
        val = ((uint32_t)rx_buf[4] << 24) |
              ((uint32_t)rx_buf[5] << 16) |
              ((uint32_t)rx_buf[6] << 8)  |
              (uint32_t)rx_buf[7];
    }

    xSemaphoreGive(spi_mutex);
    return val;
}

// --- REGISTER WRITE ---
void adin_write_reg(uint16_t reg, uint32_t val) {
    xSemaphoreTake(spi_mutex, portMAX_DELAY);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    uint8_t tx_buf[8] = {0};

    uint16_t cmd = 0xA000 | (reg & 0x1FFF);
    tx_buf[0] = (cmd >> 8) & 0xFF;
    tx_buf[1] = cmd & 0xFF;
    tx_buf[2] = calculate_crc(tx_buf, 2);

    tx_buf[3] = (val >> 24) & 0xFF;
    tx_buf[4] = (val >> 16) & 0xFF;
    tx_buf[5] = (val >> 8)  & 0xFF;
    tx_buf[6] = val & 0xFF;
    tx_buf[7] = calculate_crc(&tx_buf[3], 4); 

    t.length = 8 * 8;
    t.tx_buffer = tx_buf;

    spi_device_transmit(spi_handle, &t);
    
    xSemaphoreGive(spi_mutex);
}

// --- FIFO WRITE (Burst + Bus Locking) ---
void adin_write_fifo(const uint8_t *data, uint32_t len) {
    xSemaphoreTake(spi_mutex, portMAX_DELAY);
    spi_device_acquire_bus(spi_handle, portMAX_DELAY);

    // 1. Header
    uint16_t cmd = 0xA000 | (ADIN1110_TX_REG & 0x1FFF);
    uint8_t header[3];
    header[0] = (cmd >> 8) & 0xFF;
    header[1] = cmd & 0xFF;
    header[2] = calculate_crc(header, 2);

    spi_transaction_t t_head;
    memset(&t_head, 0, sizeof(t_head));
    t_head.length = 3 * 8;
    t_head.tx_buffer = header;
    t_head.flags = SPI_TRANS_CS_KEEP_ACTIVE;
    spi_device_transmit(spi_handle, &t_head);

    // 2. Data
    spi_transaction_t t_data;
    memset(&t_data, 0, sizeof(t_data));
    t_data.length = len * 8;
    t_data.tx_buffer = data;
    t_data.flags = SPI_TRANS_CS_KEEP_ACTIVE;
    spi_device_transmit(spi_handle, &t_data);

    // 3. Footer
    uint8_t crc = 0x00; 
    spi_transaction_t t_crc;
    memset(&t_crc, 0, sizeof(t_crc));
    t_crc.length = 8;
    t_crc.tx_buffer = &crc;
    spi_device_transmit(spi_handle, &t_crc);

    spi_device_release_bus(spi_handle);
    xSemaphoreGive(spi_mutex);
}

// --- FIFO READ (Burst + Bus Locking) ---
void adin_read_fifo(uint8_t *dest, uint32_t len) {
    xSemaphoreTake(spi_mutex, portMAX_DELAY);
    spi_device_acquire_bus(spi_handle, portMAX_DELAY);

    // 1. Header
    uint16_t cmd = 0x8000 | (ADIN1110_RX_REG & 0x1FFF);
    uint8_t header[4]; 
    header[0] = (cmd >> 8) & 0xFF;
    header[1] = cmd & 0xFF;
    header[2] = calculate_crc(header, 2);
    header[3] = 0x00;

    spi_transaction_t t_head;
    memset(&t_head, 0, sizeof(t_head));
    t_head.length = 4 * 8;
    t_head.tx_buffer = header;
    t_head.flags = SPI_TRANS_CS_KEEP_ACTIVE;
    spi_device_transmit(spi_handle, &t_head);

    // 2. Data
    spi_transaction_t t_data;
    memset(&t_data, 0, sizeof(t_data));
    t_data.length = len * 8;
    t_data.rx_buffer = dest;
    t_data.flags = SPI_TRANS_CS_KEEP_ACTIVE;
    spi_device_transmit(spi_handle, &t_data);

    // 3. Footer
    uint8_t recv_crc;
    spi_transaction_t t_crc;
    memset(&t_crc, 0, sizeof(t_crc));
    t_crc.length = 8;
    t_crc.rx_buffer = &recv_crc;
    spi_device_transmit(spi_handle, &t_crc);

    spi_device_release_bus(spi_handle);
    xSemaphoreGive(spi_mutex);
}