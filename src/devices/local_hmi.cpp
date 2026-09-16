/**
 * @file local_hmi.cpp
 * @brief Local HMI: LCD 1602 I2C + navigation encoder.
 * ===================================================================
 * The HMI is treated as an OPERATOR PANEL: an independent task (core 0,
 * low priority) that reads the machine image (g_plant) and writes the
 * Settings — exactly like an HMI panel connected to a PLC. The 10 ms
 * polling cycle is never delayed by the ~30 ms of an LCD refresh.
 *
 * Arduino port: the I2C bus uses the Wire library (the driver/i2c_master
 * API does not exist in the installed ESP32 core 2.0.17).
 *
 * 6 screens, navigation by rotating encoder #2, press = enter / browse
 * the settings:
 *   STA  STATE        machine state, alarm / production order
 *   PRO  PRODUCTION   raw OK/NOK counters — no OEE
 *   NET  NETWORK      SSID / IP / OPC UA port (alternating)
 *   SET  SETTINGS     target, tolerance, slip, speed (editable)
 *   CYC  LAST CYCLE   indexing angles of the 4 legs + verdict
 *   BAD  BADGE        UID / optional content
 *
 * The board remains fully functional without a wired display: I2C
 * detection (0x27 then 0x3F), retry every 5 s. The encoder is only armed
 * once the LCD is detected (GPIO 36/39 floating on bare board).
 *
 * Critical-section rule respected: snapshot of g_plant copied UNDER the
 * lock, formatting snprintf OUTSIDE the lock.
 */
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <Wire.h>

#include "driver/gpio.h"
#include "esp_timer.h"

#include "config.h"
#include "devices/local_hmi.h"
#include "devices/machine.h"
#include "services/wifi_manager.h"
#include <WiFi.h>
#include <esp_wifi.h>

/* ------------------------------------------------------------------ */
/* LCD 1602 driver via PCF8574 I2C backpack (4-bit mode)                */
/* standard backpack mapping: P0=RS P1=RW P2=EN P3=backlight P4-7=D4-7 */
/* ------------------------------------------------------------------ */
#define LCD_RS 0x01
#define LCD_EN 0x04
#define LCD_BL 0x08

static uint8_t s_addr = 0;
static bool s_lcd_ok = false;

static esp_err_t pcf_write(uint8_t v)
{
    Wire.beginTransmission(s_addr);
    Wire.write(v);
    return (Wire.endTransmission() == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t lcd_nibble(uint8_t nib, uint8_t flags)
{
    /* EN pulse: the slowness of I2C (>100 µs/byte) covers the HD44780
       hold times (~µs) */
    uint8_t v = (uint8_t)((nib << 4) | flags | LCD_BL);
    esp_err_t e = pcf_write(v | LCD_EN);
    if (e == ESP_OK)
        e = pcf_write(v & ~LCD_EN);
    return e;
}

static esp_err_t lcd_byte(uint8_t b, uint8_t flags)
{
    esp_err_t e = lcd_nibble(b >> 4, flags);
    if (e == ESP_OK)
        e = lcd_nibble(b & 0x0F, flags);
    return e;
}

static esp_err_t lcd_cmd(uint8_t c)
{
    esp_err_t e = lcd_byte(c, 0);
    if (c <= 0x03)
        vTaskDelay(pdMS_TO_TICKS(2)); /* clear/home: 1.52 ms */
    return e;
}

static esp_err_t lcd_init_display(void)
{
    vTaskDelay(pdMS_TO_TICKS(50)); /* HD44780 power-up */
    for (int i = 0; i < 3; i++) {  /* 8-bit wake-up sequence */
        lcd_nibble(0x03, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    lcd_nibble(0x02, 0);         /* switch to 4-bit */
    esp_err_t e = lcd_cmd(0x28); /* 4 bits, 2 lines, 5x8 */
    lcd_cmd(0x08);               /* display off */
    lcd_cmd(0x01);               /* clear */
    lcd_cmd(0x06);               /* increment, no shift */
    lcd_cmd(0x0C);               /* display on, cursor off */
    return e;
}

/* writes a full line (16 characters, padded with spaces) */
static esp_err_t lcd_line(int row, const char *txt)
{
    esp_err_t e = lcd_cmd(row == 0 ? 0x80 : 0xC0);
    for (int i = 0; i < 16 && e == ESP_OK; i++)
        e = lcd_byte((uint8_t)(txt[i] ? txt[i] : ' '), LCD_RS);
    return e;
}

/* ------------------------------------------------------------------ */
/* encoder #2: rotation by interrupt (falling edge on CLK)             */
/* ------------------------------------------------------------------ */
static volatile int32_t s_enc_delta = 0;
static volatile int64_t s_enc_last_us = 0;
static portMUX_TYPE s_enc_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR enc2_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (now - s_enc_last_us < 2000) /* hardware debounce 2 ms */
        return;
    s_enc_last_us = now;
    if (gpio_get_level(PIN_ENC2_DT)) /* direction: reverse +/- if needed */
        s_enc_delta++;
    else
        s_enc_delta--;
}

static void encoder_arm(void)
{
    static bool done = false;
    if (done)
        return;
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_ENC2_CLK) | (1ULL << PIN_ENC2_DT),
        .mode = GPIO_MODE_INPUT, /* GPIO 36/39: KY-040 module pull-ups */
    };
    gpio_config(&in);
    gpio_config_t sw = {
        .pin_bit_mask = (1ULL << PIN_ENC2_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&sw);
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(e);
    gpio_set_intr_type(PIN_ENC2_CLK, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(PIN_ENC2_CLK, enc2_isr, NULL);
    done = true;
}

/* ------------------------------------------------------------------ */
/* screens                                                             */
/* ------------------------------------------------------------------ */
typedef enum { SCR_STATE, SCR_PROD, SCR_NET, SCR_SETTINGS, SCR_CYCLE, SCR_BADGE,
               SCR_NB } screen_t;
static const char *SCREEN_TAGS[SCR_NB] = {"STA", "PRO", "NET", "SET", "CYC", "BAD"};
static const char *STATES8[] = {"INIT",    "STOP",    "READY",    "WAIT",
                                "SCREW",   "END CYC", "SUSPND",   "FAULT"};

/* machine image snapshot (copied under lock, formatted outside) */
typedef struct {
    int32_t iState, iCurrentLeg, iAlarmCode;
    uint32_t nGood, nBad;
    double rAngleLeg[4];
    bool bPartOk;
    char sOrderId[24];
    char sRfidUid[24];
    char sRfidData[20];
    double rTargetDeg, rTolDeg, rSlipPct;
    int32_t iSpeed;
} snapshot_t;

static void take_snapshot(snapshot_t *s)
{
    PLANT_LOCK();
    s->iState = g_plant.iState;
    s->iCurrentLeg = g_plant.iCurrentLeg;
    s->iAlarmCode = g_plant.iAlarmCode;
    s->nGood = g_plant.nGood;
    s->nBad = g_plant.nBad;
    for (int i = 0; i < 4; i++)
        s->rAngleLeg[i] = g_plant.rAngleLeg[i];
    s->bPartOk = g_plant.bPartOk;
    strlcpy(s->sOrderId, g_plant.sOrderId, sizeof(s->sOrderId));
    strlcpy(s->sRfidUid, g_plant.sRfidUid, sizeof(s->sRfidUid));
    strlcpy(s->sRfidData, g_plant.sRfidData, sizeof(s->sRfidData));
    s->rTargetDeg = g_plant.rTargetDeg;
    s->rTolDeg = g_plant.rTolDeg;
    s->rSlipPct = g_plant.rSlipPct;
    s->iSpeed = g_plant.iSpeed;
    PLANT_UNLOCK();
}

/* editable settings: short name, step, bounds (unit in the format) */
typedef struct {
    const char *name;
    double step, min, max;
    bool integer; /* iSpeed */
} param_t;
static const param_t PARAMS[] = {
    {"Target", 1.0, 45.0, 180.0, false}, {"Tol", 0.5, 1.0, 45.0, false},
    {"Slip", 1.0, 0.0, 100.0, false},  {"Speed", 25, 100, 900, true},
};
#define NPARAMS (int)(sizeof(PARAMS) / sizeof(PARAMS[0]))

static double param_read(const snapshot_t *s, int i)
{
    switch (i) {
    case 0: return s->rTargetDeg;
    case 1: return s->rTolDeg;
    case 2: return s->rSlipPct;
    default: return (double)s->iSpeed;
    }
}

static void param_write(int i, double v)
{
    const param_t *p = &PARAMS[i];
    if (v < p->min)
        v = p->min;
    if (v > p->max)
        v = p->max;
    PLANT_LOCK();
    switch (i) {
    case 0: g_plant.rTargetDeg = v; break;
    case 1: g_plant.rTolDeg = v; break;
    case 2: g_plant.rSlipPct = v; break;
    default: g_plant.iSpeed = (int32_t)v; break;
    }
    PLANT_UNLOCK();
}

/* line 1, common: machine state + current leg + screen tag */
static void format_l1(char *l1, const snapshot_t *s, screen_t scr)
{
    char leg[5] = "";
    if (s->iState == STATE_SCREWING && s->iCurrentLeg > 0)
        snprintf(leg, sizeof(leg), "%ld/4", (long)s->iCurrentLeg);
    int st = (s->iState >= 0 && s->iState <= 7) ? s->iState : 0;
    snprintf(l1, 17, "%-8.8s%4.4s %3.3s", STATES8[st], leg, SCREEN_TAGS[scr]);
}

/* line 2, contextual */
static void format_l2(char *l2, const snapshot_t *s, screen_t scr, int edit_idx,
                      int network_phase, bool badge_content)
{
    switch (scr) {
    case SCR_STATE:
        if (s->iState == STATE_FAULT)
            snprintf(l2, 17, "ALARM %s",
                     s->iAlarmCode == ALARM_POSITION_LONG ? "LONG" : "SHORT");
        else
            snprintf(l2, 17, "%-16.16s", s->sOrderId);
        break;
    case SCR_PROD: /* raw counters only — no OEE */
        snprintf(l2, 17, "OK%6lu NOK%4lu",
                 (unsigned long)(s->nGood > 999999 ? 999999 : s->nGood),
                 (unsigned long)(s->nBad > 9999 ? 9999 : s->nBad));
        break;
    case SCR_NET:
        if (network_phase == 0) {
            /* low-level access to avoid Arduino String allocation */
            wifi_ap_record_t ap_info;
            char ssid[33] = "";
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                memcpy(ssid, ap_info.ssid, 32);
                ssid[32] = '\0';
            }
            snprintf(l2, 17, "%-16.16s", ssid[0] ? ssid : "?");
        } else if (network_phase == 1) {
            char ipBuffer[16];
            wifi_get_ip(ipBuffer, sizeof(ipBuffer));
            snprintf(l2, 17, "%-16.16s", ipBuffer);
        } else {
            snprintf(l2, 17, "OPC UA p%d", (int)OPCUA_PORT);
        }
        break;
    case SCR_SETTINGS:
        if (edit_idx < 0) {
            snprintf(l2, 17, "Press = settings");
        } else {
            const param_t *p = &PARAMS[edit_idx];
            snapshot_t sn = *s;
            double v = param_read(&sn, edit_idx);
            if (p->integer)
                snprintf(l2, 17, "%-5.5s%6d s/s *", p->name, (int)v);
            else
                snprintf(l2, 17, "%-5.5s%6.1f %s *", p->name, v,
                         edit_idx == 2 ? "%  " : "deg");
        }
        break;
    case SCR_CYCLE: /* indexing angles of the 4 legs [°] + verdict */
        snprintf(l2, 17, "%3.0f %3.0f %3.0f %3.0f%c", s->rAngleLeg[0],
                 s->rAngleLeg[1], s->rAngleLeg[2], s->rAngleLeg[3],
                 s->bPartOk ? '+' : '-');
        break;
    default: /* SCR_BADGE : badge UID; press = content (optional) */
        if (s->sRfidUid[0] == '\0')
            snprintf(l2, 17, "no badge read");
        else if (badge_content)
            snprintf(l2, 17, "%-16.16s",
                     s->sRfidData[0] ? s->sRfidData : "(no content)");
        else
            snprintf(l2, 17, "%-16.16s", s->sRfidUid);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* LCD detection on the bus                                             */
/* ------------------------------------------------------------------ */
static bool lcd_detect(void)
{
    static const uint8_t ADRS[] = {0x27, 0x3F}; /* PCF8574 then PCF8574A */
    for (size_t i = 0; i < sizeof(ADRS); i++) {
        Wire.beginTransmission(ADRS[i]);
        if (Wire.endTransmission() != 0)
            continue; /* no answer */
        s_addr = ADRS[i];
        if (lcd_init_display() != ESP_OK)
            return false;
        Serial.printf("[HMI] LCD 1602 detected at 0x%02X.\n", ADRS[i]);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* HMI task                                                            */
/* ------------------------------------------------------------------ */
void local_hmi_task(void *arg)
{
    (void)arg;
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
    Wire.setTimeOut(50);

    screen_t scr = SCR_STATE;
    int edit_idx = -1;        /* -1 = navigation, otherwise edited parameter */
    bool badge_content = false; /* BADGE screen: false = UID, true = content */
    int sw_prev = 1, sw_stable = 0;
    int errors = 0;
    char cache[2][17] = {{0}, {0}};
    int64_t t_retry = 0, t_net = 0;
    int network_phase = 0;

    Serial.printf("[HMI] Local HMI started (core %d).\n", (int)xPortGetCoreID());

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        int64_t now = esp_timer_get_time();

        /* --- LCD absent: detection every 5 s, HMI on standby --- */
        if (!s_lcd_ok) {
            if (now - t_retry > 5000000) {
                t_retry = now;
                s_lcd_ok = lcd_detect();
                if (s_lcd_ok) {
                    encoder_arm();
                    portENTER_CRITICAL(&s_enc_mux);
                    s_enc_delta = 0;
                    portEXIT_CRITICAL(&s_enc_mux);
                    memset(cache, 0, sizeof(cache));
                    errors = 0;
                }
            }
            continue;
        }

        /* --- inputs: encoder rotation + button --- */
        int32_t delta = 0;
        portENTER_CRITICAL(&s_enc_mux);
        delta = s_enc_delta;
        s_enc_delta = 0;
        portEXIT_CRITICAL(&s_enc_mux);
        bool pressed = false;
        int sw = gpio_get_level(PIN_ENC2_SW);
        if (sw == 0 && sw_prev == 0)
            sw_stable++; /* 2 samples at 0 = confirmed press (40 ms) */
        else
            sw_stable = 0;
        if (sw_stable == 1)
            pressed = true;
        sw_prev = sw;

        snapshot_t s;
        take_snapshot(&s);

        if (edit_idx < 0) { /* navigation between screens */
            if (delta) {
                int n = ((int)scr + delta) % SCR_NB;
                if (n < 0)
                    n += SCR_NB;
                scr = (screen_t)n;
            }
            if (pressed && scr == SCR_SETTINGS)
                edit_idx = 0;
            else if (pressed && scr == SCR_BADGE)
                badge_content = !badge_content; /* toggle UID <-> content */
        } else { /* editing a setting */
            if (delta) {
                double v = param_read(&s, edit_idx) + delta * PARAMS[edit_idx].step;
                param_write(edit_idx, v);
                take_snapshot(&s); /* re-read clamped value */
            }
            if (pressed && ++edit_idx >= NPARAMS)
                edit_idx = -1; /* last parameter: back to navigation */
        }

        /* --- NETWORK screen info alternation (2 s) --- */
        if (now - t_net > 2000000) {
            t_net = now;
            network_phase = (network_phase + 1) % 3;
        }

        /* --- refresh: only lines that changed --- */
        char l1[17], l2[17];
        format_l1(l1, &s, scr);
        format_l2(l2, &s, scr, edit_idx, network_phase, badge_content);
        esp_err_t e = ESP_OK;
        if (strcmp(l1, cache[0]) != 0 && (e = lcd_line(0, l1)) == ESP_OK)
            strlcpy(cache[0], l1, 17);
        if (e == ESP_OK && strcmp(l2, cache[1]) != 0 &&
            (e = lcd_line(1, l2)) == ESP_OK)
            strlcpy(cache[1], l2, 17);

        if (e != ESP_OK) { /* display unplugged? return to detection */
            if (++errors >= 3) {
                Serial.println("[HMI] LCD unreachable, returning to detection.");
                s_lcd_ok = false;
            }
        } else {
            errors = 0;
        }
    }
}
