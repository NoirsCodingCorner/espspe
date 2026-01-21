#include "adin1110_regs.h"
#include "driver/spi_master.h"
#include "freertos/semphr.h"
#include <string.h>

// Globaler Context für SPI (vereinfacht für Single-Instance)
static spi_device_handle_t s_spi_handle;
static SemaphoreHandle_t s_spi_lock;

// Dein CRC Algorithmus (Portiert auf C)
static uint8_t calc_crc(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x07; else crc <<= 1;
        }
    }
    return crc;
}

// SPI Init Helper
esp_err_t adin_spi_init(spi_host_device_t host, int cs_pin, int freq) {
    s_spi_lock = xSemaphoreCreateMutex();
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = freq,
        .mode = 0,
        .spics_io_num = cs_pin,
        .queue_size = 10,
    };
    return spi_bus_add_device(host, &devcfg, &s_spi_handle);
}

uint32_t adin_read_reg(uint16_t reg) {
    uint8_t tx[4] = {0}; // Header(2) + CRC(1) + Turnaround(1)
    uint16_t cmd = 0x8000 | (reg & 0x1FFF); // Generic SPI Read
    tx[0] = (cmd >> 8); 
    tx[1] = (cmd & 0xFF);
    tx[2] = calc_crc(tx, 2);
    tx[3] = 0x00; // Turnaround

    uint8_t rx[8] = {0}; // Empfangspuffer
    
    spi_transaction_t t = {
        .length = 8 * 8, // 4 Byte Header/TA + 4 Byte Data
        .tx_buffer = tx,
        .rx_buffer = rx
    };

    xSemaphoreTake(s_spi_lock, portMAX_DELAY);
    spi_device_transmit(s_spi_handle, &t);
    xSemaphoreGive(s_spi_lock);

    // Daten stehen ab Byte 4 (Big Endian)
    return (rx[4] << 24) | (rx[5] << 16) | (rx[6] << 8) | rx[7];
}

void adin_write_reg(uint16_t reg, uint32_t val) {
    uint8_t buf[8];
    uint16_t cmd = 0xA000 | (reg & 0x1FFF); // Generic SPI Write
    buf[0] = cmd >> 8; buf[1] = cmd & 0xFF;
    buf[2] = calc_crc(buf, 2);
    buf[3] = val >> 24; buf[4] = val >> 16; buf[5] = val >> 8; buf[6] = val & 0xFF;
    buf[7] = calc_crc(&buf[3], 4);

    spi_transaction_t t = { .length = 64, .tx_buffer = buf };
    
    xSemaphoreTake(s_spi_lock, portMAX_DELAY);
    spi_device_transmit(s_spi_handle, &t);
    xSemaphoreGive(s_spi_lock);
}

// Burst Write für Frame Data (KEIN CRC auf Payload!)
void adin_write_fifo(const uint8_t *data, uint32_t len) {
    uint16_t cmd = 0xA000 | (ADIN1110_TX_REG & 0x1FFF);
    uint8_t header[2] = {cmd >> 8, cmd & 0xFF};
    uint8_t crc = calc_crc(header, 2);

    // Wir müssen Header und Daten in einer Transaktion senden (CS active low)
    // Wir nutzen hier polling für Einfachheit bei < 64 Bytes, sonst DMA
    spi_transaction_t t = {0};
    
    // Für ESP-IDF ist es am besten, einen temporären Buffer zu nutzen, 
    // um Header + Data zusammenzuhängen, oder `flags = SPI_TRANS_USE_TXDATA` bei kurzen Paketen.
    // Hier allokieren wir temporär (Performance-Optimierung wäre statischer Buffer).
    uint8_t *send_buf = heap_caps_malloc(len + 3 + 1, MALLOC_CAP_DMA); // +3 Header/CRC, +1 Padding
    
    send_buf[0] = header[0];
    send_buf[1] = header[1];
    send_buf[2] = crc;
    memcpy(&send_buf[3], data, len);
    
    // Padding auf 4 Bytes (optional aber empfohlen beim ADIN)
    uint32_t total_len = len + 3;
    if (total_len % 4 != 0) total_len += (4 - (total_len % 4));

    t.length = total_len * 8;
    t.tx_buffer = send_buf;

    xSemaphoreTake(s_spi_lock, portMAX_DELAY);
    spi_device_transmit(s_spi_handle, &t);
    xSemaphoreGive(s_spi_lock);
    
    heap_caps_free(send_buf);
}

// Burst Read FIFO
void adin_read_fifo(uint8_t *dest, uint32_t len) {
    uint16_t cmd = 0x8000 | (ADIN1110_RX_REG & 0x1FFF);
    uint8_t head[3] = {cmd >> 8, cmd & 0xFF, 0};
    head[2] = calc_crc(head, 2); 
    
    // Transaktion: Header(2)+CRC(1)+TA(1) senden, dann Daten lesen
    // Wir machen Full-Duplex: Sende CMD, empfange Müll. Sende Müll, empfange Daten.
    size_t total = 4 + len; // 4 Byte Overhead
    uint8_t *tx = heap_caps_calloc(1, total, MALLOC_CAP_DMA);
    uint8_t *rx = heap_caps_malloc(total, MALLOC_CAP_DMA);
    
    memcpy(tx, head, 3);
    // tx[3] ist Turnaround (0)
    
    spi_transaction_t t = { .length = total * 8, .tx_buffer = tx, .rx_buffer = rx };
    
    xSemaphoreTake(s_spi_lock, portMAX_DELAY);
    spi_device_transmit(s_spi_handle, &t);
    xSemaphoreGive(s_spi_lock);
    
    memcpy(dest, &rx[4], len); // Daten ab Byte 4
    
    heap_caps_free(tx);
    heap_caps_free(rx);
}