/*
 * rc522.cpp — minimal MFRC522 driver (ISO 14443-A protocol)
 * ==========================================================
 * Written to be READ: every step of the contactless card dialogue is
 * visible — request (REQA), anticollision (UID byte by byte), selection,
 * then optional content read.
 *
 * Pinout: VSPI bus — SCK 18, MISO 19, MOSI 23, SS 5, RST 17.
 * 3.3 V power supply MANDATORY.
 *
 * Startup detection: if no reader answers (bare board), the firmware falls
 * back to the simulated badge — see screwstation.cpp.
 */
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "devices/rc522.h"
#include "devices/machine.h"

/* ---- MFRC522 registers (NXP datasheet, §9) ---- */
#define R_COMMAND 0x01
#define R_COMIRQ 0x04
#define R_ERROR 0x06
#define R_STATUS2 0x08
#define R_FIFODATA 0x09
#define R_FIFOLEVEL 0x0A
#define R_CONTROL 0x0C
#define R_BITFRAMING 0x0D
#define R_COLL 0x0E
#define R_MODE 0x11
#define R_TXCONTROL 0x14
#define R_TXASK 0x15
#define R_CRCRESULT_H 0x21
#define R_CRCRESULT_L 0x22
#define R_TMODE 0x2A
#define R_TPRESCALER 0x2B
#define R_TRELOAD_H 0x2C
#define R_TRELOAD_L 0x2D
#define R_VERSION 0x37

/* ---- MFRC522 commands ---- */
#define CMD_IDLE 0x00
#define CMD_CALCCRC 0x03
#define CMD_TRANSCEIVE 0x0C
#define CMD_MFAUTHENT 0x0E
#define CMD_SOFTRESET 0x0F

/* ---- commands SENT TO THE CARD (ISO 14443-A / MIFARE) ---- */
#define PICC_REQA 0x26
#define PICC_ANTICOLL_CL1 0x93
#define PICC_ANTICOLL_CL2 0x95
#define PICC_SELECT 0x70
#define PICC_AUTH_KEY_A 0x60
#define PICC_READ 0x30
#define PICC_HALT 0x50

static spi_device_handle_t s_dev;
static bool s_detected = false;

/* ------------------------------------------------------------------ */
/* register access: SPI frame = address (bit7 = read) then data         */
/* ------------------------------------------------------------------ */
static uint8_t reg_read(uint8_t reg)
{
    uint8_t tx[2] = {(uint8_t)(((reg << 1) & 0x7E) | 0x80), 0};
    uint8_t rx[2] = {0};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx, .rx_buffer = rx};
    spi_device_polling_transmit(s_dev, &t);
    return rx[1];
}

static void reg_write(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = {(uint8_t)((reg << 1) & 0x7E), val};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx};
    spi_device_polling_transmit(s_dev, &t);
}

static void reg_set(uint8_t reg, uint8_t bits)
{
    reg_write(reg, reg_read(reg) | bits);
}

static void reg_clear(uint8_t reg, uint8_t bits)
{
    reg_write(reg, reg_read(reg) & (uint8_t)~bits);
}

/* ------------------------------------------------------------------ */
/* CRC-A computed by the chip (CalcCRC command)                         */
/* ------------------------------------------------------------------ */
static bool calc_crc(const uint8_t *data, size_t n, uint8_t crc[2])
{
    reg_write(R_COMMAND, CMD_IDLE);
    reg_write(R_COMIRQ, 0x04); /* clears CRCIRq */
    reg_set(R_FIFOLEVEL, 0x80); /* empties FIFO */
    for (size_t i = 0; i < n; i++)
        reg_write(R_FIFODATA, data[i]);
    reg_write(R_COMMAND, CMD_CALCCRC);
    for (int i = 0; i < 30; i++) {
        if (reg_read(R_COMIRQ) & 0x04) {
            crc[0] = reg_read(R_CRCRESULT_L);
            crc[1] = reg_read(R_CRCRESULT_H);
            reg_write(R_COMMAND, CMD_IDLE);
            return true;
        }
        vTaskDelay(1);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* exchange with the card: sends `send`, receives the response          */
/* tx_last_bits: number of bits of the last byte (7 for REQA)           */
/* ------------------------------------------------------------------ */
static bool transceive(uint8_t command, const uint8_t *send, size_t slen,
                       uint8_t *recv, size_t *rlen, uint8_t tx_last_bits)
{
    uint8_t expected = (command == CMD_MFAUTHENT) ? 0x10 : 0x30; /* Idle|Rx */
    reg_write(R_COMMAND, CMD_IDLE);
    reg_write(R_COMIRQ, 0x7F); /* clears all interrupts */
    reg_set(R_FIFOLEVEL, 0x80);
    for (size_t i = 0; i < slen; i++)
        reg_write(R_FIFODATA, send[i]);
    reg_write(R_COMMAND, command);
    if (command == CMD_TRANSCEIVE)
        reg_write(R_BITFRAMING, 0x80 | (tx_last_bits & 0x07));

    bool ok = false;
    for (int i = 0; i < 25; i++) { /* software timeout ~25 ms */
        uint8_t irq = reg_read(R_COMIRQ);
        if (irq & expected) {
            ok = true;
            break;
        }
        if (irq & 0x01) /* TimerIrq: card did not answer */
            break;
        vTaskDelay(1);
    }
    reg_clear(R_BITFRAMING, 0x80);
    if (!ok)
        return false;
    if (reg_read(R_ERROR) & 0x13) /* BufferOvfl | ParityErr | ProtocolErr */
        return false;

    if (recv && rlen) {
        size_t n = reg_read(R_FIFOLEVEL);
        if (n > *rlen)
            n = *rlen;
        for (size_t i = 0; i < n; i++)
            recv[i] = reg_read(R_FIFODATA);
        *rlen = n;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* ISO 14443-A dialogue                                                 */
/* ------------------------------------------------------------------ */
static bool card_present(void)
{
    /* REQA: short 7-bit frame; expected response: ATQA (2 bytes) */
    uint8_t reqa = PICC_REQA, atqa[2];
    size_t n = sizeof(atqa);
    reg_write(R_BITFRAMING, 0x07);
    return transceive(CMD_TRANSCEIVE, &reqa, 1, atqa, &n, 7) && n == 2;
}

/* One anticollision + selection level. Returns the SAK (or -1). */
static int cascade_level(uint8_t sel, uint8_t uid4[4])
{
    uint8_t cmd[9], resp[5];
    size_t n = sizeof(resp);

    /* anticollision: card returns its 4 UID bytes + BCC */
    reg_clear(R_COLL, 0x80);
    cmd[0] = sel;
    cmd[1] = 0x20;
    if (!transceive(CMD_TRANSCEIVE, cmd, 2, resp, &n, 0) || n != 5)
        return -1;
    if ((resp[0] ^ resp[1] ^ resp[2] ^ resp[3]) != resp[4])
        return -1; /* bad BCC */
    memcpy(uid4, resp, 4);

    /* selection: we send back the full UID + CRC, card returns SAK */
    cmd[0] = sel;
    cmd[1] = PICC_SELECT;
    memcpy(&cmd[2], resp, 5);
    if (!calc_crc(cmd, 7, &cmd[7]))
        return -1;
    uint8_t sak[3];
    n = sizeof(sak);
    if (!transceive(CMD_TRANSCEIVE, cmd, 9, sak, &n, 0) || n < 1)
        return -1;
    return sak[0];
}

/* Full UID (4 or 7 bytes, CL1/CL2 cascade). */
static int read_uid(uint8_t uid[7])
{
    uint8_t cl1[4];
    int sak = cascade_level(PICC_ANTICOLL_CL1, cl1);
    if (sak < 0)
        return 0;
    if (!(sak & 0x04)) { /* full 4-byte UID */
        memcpy(uid, cl1, 4);
        return 4;
    }
    /* cl1[0] == 0x88 (cascade tag): 3 useful bytes + level 2 */
    uint8_t cl2[4];
    sak = cascade_level(PICC_ANTICOLL_CL2, cl2);
    if (sak < 0)
        return 0;
    memcpy(uid, &cl1[1], 3);
    memcpy(&uid[3], cl2, 4);
    return 7; /* 10-byte UIDs (rare) are not handled */
}

static void halt_card(void)
{
    uint8_t cmd[4] = {PICC_HALT, 0x00};
    calc_crc(cmd, 2, &cmd[2]);
    size_t n = 0;
    transceive(CMD_TRANSCEIVE, cmd, 4, NULL, &n, 0); /* no response */
    reg_clear(R_STATUS2, 0x08); /* disables Crypto1 if needed */
}

/* Read 16 bytes at address 4:
   - NTAG / Ultralight: direct READ allowed;
   - MIFARE Classic (4-byte UID): factory key A authentication first. */
static bool read_content(const uint8_t *uid, int uid_len, uint8_t data[16])
{
    uint8_t cmd[4] = {PICC_READ, 4};
    uint8_t resp[18];
    size_t n = sizeof(resp);
    if (calc_crc(cmd, 2, &cmd[2]) &&
        transceive(CMD_TRANSCEIVE, cmd, 4, resp, &n, 0) && n >= 16) {
        memcpy(data, resp, 16);
        return true;
    }
    if (uid_len != 4)
        return false;
    /* MIFARE Classic: sector 1 (block 4) authentication, factory key */
    uint8_t auth[12] = {PICC_AUTH_KEY_A, 4, 0xFF, 0xFF, 0xFF,
                        0xFF, 0xFF, 0xFF};
    memcpy(&auth[8], uid, 4);
    if (!transceive(CMD_MFAUTHENT, auth, 12, NULL, NULL, 0))
        return false;
    if (!(reg_read(R_STATUS2) & 0x08)) /* Crypto1 not engaged */
        return false;
    n = sizeof(resp);
    bool ok = calc_crc(cmd, 2, &cmd[2]) &&
              transceive(CMD_TRANSCEIVE, cmd, 4, resp, &n, 0) && n >= 16;
    if (ok)
        memcpy(data, resp, 16);
    reg_clear(R_STATUS2, 0x08); /* end of encrypted session */
    return ok;
}

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */
bool rc522_init(void)
{
    /* explicit field initialization (declaration order differs between
       ESP-IDF versions) */
    spi_bus_config_t bus = {};
    bus.mosi_io_num = PIN_RFID_MOSI;
    bus.miso_io_num = PIN_RFID_MISO;
    bus.sclk_io_num = PIN_RFID_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK)
        return false;
    spi_device_interface_config_t dev = {};
    dev.mode = 0;
    dev.clock_speed_hz = 8000000; /* 8 MHz (MFRC522 max: 10 MHz) */
    dev.spics_io_num = PIN_RFID_SS;
    dev.queue_size = 1;
    if (spi_bus_add_device(SPI3_HOST, &dev, &s_dev) != ESP_OK)
        return false;

    /* hardware then software reset */
    gpio_config_t rst = {};
    rst.pin_bit_mask = 1ULL << PIN_RFID_RST;
    rst.mode = GPIO_MODE_OUTPUT;
    gpio_config(&rst);
    gpio_set_level(PIN_RFID_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_RFID_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    reg_write(R_COMMAND, CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* card waiting timer (~15 ms) + 100 % ASK modulation + CRC-A */
    reg_write(R_TMODE, 0x8D);
    reg_write(R_TPRESCALER, 0x3E);
    reg_write(R_TRELOAD_H, 0);
    reg_write(R_TRELOAD_L, 30);
    reg_write(R_TXASK, 0x40);
    reg_write(R_MODE, 0x3D);
    reg_set(R_TXCONTROL, 0x03); /* antenna on */

    /* detection: known stable version and coherent re-read registers
       (on bare board, MISO floats: we must NOT trust a mirage) */
    uint8_t v1 = reg_read(R_VERSION);
    uint8_t v2 = reg_read(R_VERSION);
    bool version_ok = (v1 == v2) &&
                      (v1 == 0x88 || v1 == 0x90 || v1 == 0x91 ||
                       v1 == 0x92 || v1 == 0xB2);
    bool regs_ok = (reg_read(R_MODE) == 0x3D) &&
                   ((reg_read(R_TXCONTROL) & 0x03) == 0x03);
    s_detected = version_ok && regs_ok;
    if (s_detected)
        Serial.printf("[RC522] MFRC522 detected (version 0x%02X) — real badges.\n", v1);
    else
        Serial.printf("[RC522] No RFID reader (read version 0x%02X) — "
                      "simulated badge (bSimMode).\n", v1);
    return s_detected;
}

bool rc522_detected(void)
{
    return s_detected;
}

bool rc522_read_badge(char *uid_str, size_t uid_size,
                      char *data_str, size_t data_size)
{
    if (!s_detected)
        return false;
    if (data_str && data_size)
        data_str[0] = '\0';

    if (!card_present())
        return false;
    uint8_t uid[7];
    int n = read_uid(uid);
    if (n == 0)
        return false;

    /* UID in hexadecimal — the kit identifier */
    size_t pos = 0;
    for (int i = 0; i < n && pos + 2 < uid_size; i++)
        pos += snprintf(&uid_str[pos], uid_size - pos, "%02X", uid[i]);

    /* optional content: 16 bytes at address 4, in printable ASCII */
    uint8_t data[16];
    if (data_str && data_size > 16 && read_content(uid, n, data)) {
        for (int i = 0; i < 16; i++)
            data_str[i] = (data[i] >= 32 && data[i] < 127) ? (char)data[i] : '.';
        data_str[16] = '\0';
    }
    halt_card();
    return true;
}
