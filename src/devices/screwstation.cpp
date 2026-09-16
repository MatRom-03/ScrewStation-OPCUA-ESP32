/**
 * @file screwstation.cpp
 * @brief A1 screwing automaton exposed as an OPC UA device.
 *
 * Port of the student kit "Les Meubles du Futur" (MIT) into the project's
 * generic architecture: the whole machine (automaton, GPIO, RFID, HMI) lives
 * here, and the manufacturer-style OPC UA interface is described with the
 * OpcUaObject model. The automaton task (10 ms polling cycle) is independent
 * of the OPC UA server: the machine runs even without a network.
 */

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "config.h"
#include "devices/local_hmi.h"
#include "devices/rc522.h"
#include "devices/screwstation.h"
#include "devices/machine.h"
#include "services/wifi_manager.h"

/* ------------------------------------------------------------------ */
/* machine image: single instance + lock                               */
/* ------------------------------------------------------------------ */
plant_t g_plant;
SemaphoreHandle_t g_plant_mux = NULL;

/* ---- input image (phase 1) ---- */
typedef struct {
    bool btn_start; /* falling-edge pulses, debounced */
    bool btn_stop;
    bool btn_ack;
    bool cmd_start; /* pulses from OPC UA (Cmd/) */
    bool cmd_stop;
    bool cmd_ack;
    bool cmd_reset_cnt;
} inputs_t;

/* ---- output image (phase 4) ---- */
typedef struct {
    bool led_green;
    bool led_orange;
    bool led_red;
    bool stepper_active; /* step sequencer is running */
    int step_rate;       /* half-steps per second */
} outputs_t;

/* ---- internal automaton context (never visible to OPC UA) ---- */
static struct {
    state_t state;
    leg_phase_t leg_phase;   /* current leg sub-sequence */
    int64_t t_entry_ms;      /* time of entry in current state */
    int64_t t_phase_ms;      /* time of entry in current leg phase */
    int64_t t_cycle_ms;      /* cycle start time (badge read) */
    int32_t steps_start;     /* iStepsCmd at entry of current INDEXING */
    bool stop_requested;     /* Stop received during SCREWING: finish current leg */
    int64_t t_second_ms;     /* state time counting (1 s) */
    /* stepper sequencer */
    int seq_idx;
    int64_t step_acc_us;
    int64_t t_prev_us;
    /* RFID reader: polling divider + arrival detection */
    int rfid_div;
    int rfid_absent_count;
    /* time snapshot at the beginning of the polling cycle (optimization) */
    int64_t t_now_ms;
    int64_t t_now_us;
} ctx;

static inputs_t inputs;
static outputs_t outputs;

/* debounce: last stable level + last change time */
typedef struct { int level; int raw; int64_t t_change; } debounce_t;
static debounce_t db_start = {1, 1, 0}, db_stop = {1, 1, 0}, db_ack = {1, 1, 0};

/* 28BYJ-48 half-step sequence on IN1..IN4 */
static const uint8_t SEQ_HALF_STEP[8] = {0b1000, 0b1100, 0b0100, 0b0110,
                                         0b0010, 0b0011, 0b0001, 0b1001};

/* Bit masks of the 4 stepper coil pins in the ESP32 GPIO output registers.
   GPIO 25/26 are in GPIO.out_* ; GPIO 32/33 are in GPIO.out1_*. */
#define STEP_PIN_MASK_LOW  ((1U << PIN_STEP_IN1) | (1U << PIN_STEP_IN2))
#define STEP_PIN_MASK_HIGH ((1U << (PIN_STEP_IN3 - 32)) | (1U << (PIN_STEP_IN4 - 32)))

/* Phase durations. INDEXING ends ON STEPS (a quarter turn = HALF_STEPS_PER_QUARTER),
   the timeout is only a safety; TIGHTENING is the symbolic leg screwing, motor stopped. */
#define DURATION_INIT_MS 500
#define DURATION_INDEXING_MAX_MS 8000
#define DURATION_TIGHTENING_MS 2000
#define DURATION_CONTROL_MS 500
#define DURATION_END_CYCLE_MS 1000
#define DURATION_SIM_BADGE_MS 2000 /* simulation mode: badge "presented" after 2 s */

/* ------------------------------------------------------------------ */
/* utilities                                                           */
/* ------------------------------------------------------------------ */
static bool falling_edge(debounce_t *db, gpio_num_t gpio)
{
    int raw = gpio_get_level(gpio);
    int64_t t = ctx.t_now_ms;
    if (raw != db->raw) {
        db->raw = raw;
        db->t_change = t;
    }
    if (t - db->t_change > 30 && raw != db->level) { /* stable 30 ms */
        db->level = raw;
        if (raw == 0)
            return true; /* pressed (active low, INPUT_PULLUP) */
    }
    return false;
}

static void change_state(state_t new_state)
{
#if DEBUG_AUTOMATON
    Serial.printf("[AUTOMAT] Transition %d -> %d\n", (int)ctx.state, (int)new_state);
#endif
    ctx.state = new_state;
    ctx.t_entry_ms = ctx.t_now_ms;
}

static void timestamp(char *buf, size_t n)
{
    time_t now;
    time(&now);
    if (now < 1000000000) /* 2001-09-09: time not synchronized yet */
    {
        strlcpy(buf, "-", n);
        return;
    }
    struct tm ti;
    localtime_r(&now, &ti);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &ti);
}

/* ------------------------------------------------------------------ */
/* phase 1 — read inputs                                               */
/* ------------------------------------------------------------------ */
static void read_inputs(void)
{
    inputs.btn_start = falling_edge(&db_start, PIN_BTN_START);
    inputs.btn_stop = falling_edge(&db_stop, PIN_BTN_STOP);
    inputs.btn_ack = falling_edge(&db_ack, PIN_BTN_ACK);

    /* OPC UA pulses: consumed here, only once */
    PLANT_LOCK();
    inputs.cmd_start = g_plant.cmdStart;
    inputs.cmd_stop = g_plant.cmdStop;
    inputs.cmd_ack = g_plant.cmdAck;
    inputs.cmd_reset_cnt = g_plant.cmdResetCounters;
    g_plant.cmdStart = g_plant.cmdStop = false;
    g_plant.cmdAck = g_plant.cmdResetCounters = false;
    PLANT_UNLOCK();
}

/* ------------------------------------------------------------------ */
/* phase 2 — evaluate transitions                                      */
/* ------------------------------------------------------------------ */
static void evaluate_transitions(void)
{
    bool start = inputs.btn_start || inputs.cmd_start;
    bool stop = inputs.btn_stop || inputs.cmd_stop;
    bool ack = inputs.btn_ack || inputs.cmd_ack;
    int64_t since = ctx.t_now_ms - ctx.t_entry_ms;

    switch (ctx.state) {
    case STATE_INIT:
        if (since > DURATION_INIT_MS)
            change_state(STATE_STOP);
        break;
    case STATE_STOP:
        if (start)
            change_state(STATE_READY);
        break;
    case STATE_READY:
        if (stop)
            change_state(STATE_SUSPENDED);
        else if (start)
            change_state(STATE_WAITING_COMPONENT);
        break;
    case STATE_WAITING_COMPONENT:
        if (stop) {
            change_state(STATE_SUSPENDED);
        } else {
            bool badge = false;
            PLANT_LOCK();
            bool sim = g_plant.bSimMode;
            PLANT_UNLOCK();
            if (rc522_detected()) {
                /* real reader (ANY card UID identifies the kit). Polling every
                   10 cycles (100 ms); a badge is only accepted on ARRIVAL
                   (3 empty polls first) — leaving it on the reader does not
                   restart a cycle. */
                if (++ctx.rfid_div >= 10) {
                    ctx.rfid_div = 0;
                    char uid[sizeof(g_plant.sRfidUid)];
                    char data[sizeof(g_plant.sRfidData)];
                    if (rc522_read_badge(uid, sizeof(uid), data, sizeof(data))) {
                        if (ctx.rfid_absent_count >= 3) {
                            PLANT_LOCK();
                            strlcpy(g_plant.sRfidUid, uid, sizeof(g_plant.sRfidUid));
                            strlcpy(g_plant.sRfidData, data, sizeof(g_plant.sRfidData));
                            PLANT_UNLOCK();
                            Serial.printf("[AUTOMAT] Badge read: UID %s%s%s\n", uid,
                                          data[0] ? ", content: " : "", data);
                            badge = true;
                        }
                        ctx.rfid_absent_count = 0;
                    } else if (ctx.rfid_absent_count < 1000) {
                        ctx.rfid_absent_count++;
                    }
                }
            } else if (sim && since > DURATION_SIM_BADGE_MS) {
                /* no wired reader: simulated badge — formatted pseudo-UID
                   OUTSIDE critical section (snprintf may lock) */
                char uid[sizeof(g_plant.sRfidUid)];
                snprintf(uid, sizeof(uid), "SIM-%08X", (unsigned)esp_random());
                PLANT_LOCK();
                strlcpy(g_plant.sRfidUid, uid, sizeof(g_plant.sRfidUid));
                strlcpy(g_plant.sRfidData, "(simulation)", sizeof(g_plant.sRfidData));
                PLANT_UNLOCK();
                badge = true;
            }
            if (badge) {
                ctx.leg_phase = LEG_INDEXING;
                ctx.t_phase_ms = ctx.t_now_ms;
                ctx.t_cycle_ms = ctx.t_phase_ms;
                ctx.stop_requested = false;
                PLANT_LOCK();
                g_plant.iCurrentLeg = 1;
                g_plant.iStepsCmd = 0;
                g_plant.iEncoderCnt = 0;
                for (int i = 0; i < 4; i++) {
                    g_plant.iDetentsLeg[i] = 0;
                    g_plant.rAngleLeg[i] = 0.0;
                    g_plant.bLegOk[i] = false;
                }
                g_plant.bPartOk = false;
                ctx.steps_start = g_plant.iStepsCmd;
                PLANT_UNLOCK();
                change_state(STATE_SCREWING);
            }
        }
        break;
    case STATE_SCREWING:
        if (stop)
            ctx.stop_requested = true; /* finish current leg */
        /* exit transitions are decided in execute_state()
           at the end of each leg's CONTROL */
        break;
    case STATE_END_CYCLE:
        if (stop)
            change_state(STATE_SUSPENDED);
        else if (since > DURATION_END_CYCLE_MS)
            change_state(STATE_WAITING_COMPONENT); /* order not closed: continue */
        break;
    case STATE_SUSPENDED:
        if (start)
            change_state(STATE_READY);
        break;
    case STATE_FAULT:
        if (ack) {
            PLANT_LOCK();
            g_plant.bFault = false;
            g_plant.iAlarmCode = ALARM_NONE;
            PLANT_UNLOCK();
            change_state(STATE_STOP);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* phase 3 — current state actions                                     */
/* ------------------------------------------------------------------ */

/* end of leg CONTROL: POSITIONING verdict */
static void control_leg(void)
{
    PLANT_LOCK();
    double target = g_plant.rTargetDeg;
    double tol = g_plant.rTolDeg;
    double slip = g_plant.rSlipPct;
    int leg = g_plant.iCurrentLeg;
    PLANT_UNLOCK();

    /* Detents passed during indexing. Real wiring: count encoder #1 edges
       (GPIO 34/35, interrupts — wiring milestone).
       Simulation: the 90° target is 7.5 detents (30 detents/turn) and NEVER
       lands exactly -> 7 or 8 passed; friction can additionally slip one
       detent (rSlipPct). */
    int detents = (int)(target / DEG_PER_DETENT) + ((esp_random() & 1) ? 1 : 0);
    if (slip > 0.0 &&
        (double)esp_random() / (double)UINT32_MAX * 100.0 < slip)
        detents -= 1; /* friction slipped */
    double angle = detents * DEG_PER_DETENT;
    bool ok = fabs(angle - target) <= tol;

    PLANT_LOCK();
    g_plant.rDeltaDeg = angle - target;
    g_plant.iDetentsLeg[leg - 1] = detents;
    g_plant.rAngleLeg[leg - 1] = angle;
    g_plant.bLegOk[leg - 1] = ok;
    g_plant.iEncoderCnt += detents;
    if (!ok) {
        g_plant.bFault = true;
        g_plant.iAlarmCode = (angle < target) ? ALARM_POSITION_SHORT
                                              : ALARM_POSITION_LONG;
        g_plant.nBad++;
    }
    PLANT_UNLOCK();

#if DEBUG_AUTOMATON
    Serial.printf("[AUTOMAT] Leg %d : %d detents = %.0f deg (target %.0f +/- %.0f) -> %s\n",
                  leg, detents, angle, target, tol, ok ? "OK" : "NOK");
#endif

    if (!ok) {
        change_state(STATE_FAULT);
        return;
    }
    if (ctx.stop_requested) { /* Stop received: pause after finished leg */
        change_state(STATE_SUSPENDED);
        return;
    }
    if (leg >= 4) {
        /* cycle finished: verdict, counters, timestamp.
           timestamp() (localtime_r) takes a newlib mutex: call it OUTSIDE
           the critical section, otherwise abort() (interrupts disabled). */
        char ts[sizeof(g_plant.tsLastCycle)];
        timestamp(ts, sizeof(ts));
        double tcy = (double)(ctx.t_now_ms - ctx.t_cycle_ms) / 1000.0;
        PLANT_LOCK();
        g_plant.bPartOk = true;
        g_plant.nGood++;
        g_plant.iCurrentLeg = 0;
        g_plant.tCycleTime = tcy;
        strlcpy(g_plant.tsLastCycle, ts, sizeof(g_plant.tsLastCycle));
        PLANT_UNLOCK();
        change_state(STATE_END_CYCLE);
    } else {
        PLANT_LOCK();
        g_plant.iCurrentLeg = leg + 1;
        ctx.steps_start = g_plant.iStepsCmd;
        PLANT_UNLOCK();
        ctx.leg_phase = LEG_INDEXING;
        ctx.t_phase_ms = ctx.t_now_ms;
    }
}

static void execute_state(void)
{
    int64_t in_phase = ctx.t_now_ms - ctx.t_phase_ms;

    outputs.stepper_active = false;

    if (ctx.state == STATE_SCREWING) {
        PLANT_LOCK();
        int speed = g_plant.iSpeed;
        PLANT_UNLOCK();
        switch (ctx.leg_phase) {
        case LEG_INDEXING: { /* quarter turn: next leg comes
                                to face the base index */
            outputs.stepper_active = true;
            outputs.step_rate = speed;
            PLANT_LOCK();
            int32_t done = g_plant.iStepsCmd - ctx.steps_start;
            PLANT_UNLOCK();
            if (done >= HALF_STEPS_PER_QUARTER ||
                in_phase > DURATION_INDEXING_MAX_MS) {
                ctx.leg_phase = LEG_TIGHTENING;
                ctx.t_phase_ms = ctx.t_now_ms;
            }
            break;
        }
        case LEG_TIGHTENING: /* leg screwing — SYMBOLIC: the stool
                                stays in position, motor stopped */
            if (in_phase > DURATION_TIGHTENING_MS) {
                ctx.leg_phase = LEG_CONTROL;
                ctx.t_phase_ms = ctx.t_now_ms;
            }
            break;
        case LEG_CONTROL: /* motor stopped, measurement and verdict */
            if (in_phase > DURATION_CONTROL_MS)
                control_leg();
            break;
        }
    }

    /* reset counters on command */
    if (inputs.cmd_reset_cnt) {
        PLANT_LOCK();
        g_plant.nGood = g_plant.nBad = 0;
        g_plant.tRun = g_plant.tIdle = g_plant.tStop = g_plant.tFault = 0;
        PLANT_UNLOCK();
    }

    /* state time counting (1 s) */
    if (ctx.t_now_ms - ctx.t_second_ms >= 1000) {
        ctx.t_second_ms += 1000;
        PLANT_LOCK();
        switch (ctx.state) {
        case STATE_SCREWING:
        case STATE_END_CYCLE:
            g_plant.tRun++;
            break;
        case STATE_READY:
        case STATE_WAITING_COMPONENT:
            g_plant.tIdle++;
            break;
        case STATE_FAULT:
            g_plant.tFault++;
            break;
        default:
            g_plant.tStop++;
            break;
        }
        PLANT_UNLOCK();
    }

    /* publish state to OPC UA interface */
    PLANT_LOCK();
    g_plant.iState = (int32_t)ctx.state;
    PLANT_UNLOCK();
}

/* ------------------------------------------------------------------ */
/* phase 4 — write outputs                                             */
/* ------------------------------------------------------------------ */
static void write_outputs(void)
{
    bool blink = (ctx.t_now_ms / 500) % 2;

    switch (ctx.state) { /* LED table */
    case STATE_INIT:
        outputs.led_green = false; outputs.led_orange = blink; outputs.led_red = false;
        break;
    case STATE_STOP:
        outputs.led_green = false; outputs.led_orange = true; outputs.led_red = false;
        break;
    case STATE_READY:
    case STATE_WAITING_COMPONENT:
        outputs.led_green = blink; outputs.led_orange = false; outputs.led_red = false;
        break;
    case STATE_SCREWING:
    case STATE_END_CYCLE:
        outputs.led_green = true; outputs.led_orange = false; outputs.led_red = false;
        break;
    case STATE_SUSPENDED:
        outputs.led_green = false; outputs.led_orange = blink; outputs.led_red = false;
        break;
    case STATE_FAULT:
        outputs.led_green = false; outputs.led_orange = false; outputs.led_red = true;
        break;
    }
    gpio_set_level(PIN_LED_GREEN, outputs.led_green);
    gpio_set_level(PIN_LED_ORANGE, outputs.led_orange);
    gpio_set_level(PIN_LED_RED, outputs.led_red);

    /* half-step sequencer, clocked by time accumulation. The 4 coil pins are
       updated atomically through the GPIO output register to reduce jitter. */
    int64_t t_us = ctx.t_now_us;
    if (outputs.stepper_active && outputs.step_rate > 0) {
        ctx.step_acc_us += t_us - ctx.t_prev_us;
        int64_t period_us = 1000000 / outputs.step_rate;
        while (ctx.step_acc_us >= period_us) {
            ctx.step_acc_us -= period_us;
            ctx.seq_idx = (ctx.seq_idx + 1) % 8;
            uint8_t s = SEQ_HALF_STEP[ctx.seq_idx];
            uint32_t set_mask_low =
                (((s >> 3) & 1) ? (1U << PIN_STEP_IN1) : 0) |
                (((s >> 2) & 1) ? (1U << PIN_STEP_IN2) : 0);
            uint32_t set_mask_high =
                (((s >> 1) & 1) ? (1U << (PIN_STEP_IN3 - 32)) : 0) |
                ((s & 1)        ? (1U << (PIN_STEP_IN4 - 32)) : 0);
            /* clear coils that should be low, then set coils that should be high */
            GPIO.out_w1tc = STEP_PIN_MASK_LOW & ~set_mask_low;
            GPIO.out_w1ts = set_mask_low;
            GPIO.out1_w1tc.val = STEP_PIN_MASK_HIGH & ~set_mask_high;
            GPIO.out1_w1ts.val = set_mask_high;
            PLANT_LOCK();
            g_plant.iStepsCmd++;
            PLANT_UNLOCK();
        }
    } else {
        ctx.step_acc_us = 0;
        /* coils de-energized when stopped (28BYJ-48 heating) */
        GPIO.out_w1tc = STEP_PIN_MASK_LOW;
        GPIO.out1_w1tc.val = STEP_PIN_MASK_HIGH;
    }
    ctx.t_prev_us = t_us;
}

/* ------------------------------------------------------------------ */
/* hardware init + automaton loop                                      */
/* ------------------------------------------------------------------ */
static void init_gpio(void)
{
    gpio_config_t outputs_cfg = {
        .pin_bit_mask = (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_ORANGE) |
                        (1ULL << PIN_LED_RED) | (1ULL << PIN_STEP_IN1) |
                        (1ULL << PIN_STEP_IN2) | (1ULL << PIN_STEP_IN3) |
                        (1ULL << PIN_STEP_IN4),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&outputs_cfg);
    gpio_config_t inputs_cfg = {
        .pin_bit_mask = (1ULL << PIN_BTN_START) | (1ULL << PIN_BTN_STOP) |
                        (1ULL << PIN_BTN_ACK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&inputs_cfg);
}

void automate_task(void *arg)
{
    (void)arg;
    init_gpio();
    /* RFID reader: automatic detection — absent = simulated badge */
    rc522_init();
    ctx.rfid_absent_count = 1000; /* first badge presented is accepted */
    ctx.state = STATE_INIT;
    ctx.t_now_us = esp_timer_get_time();
    ctx.t_now_ms = ctx.t_now_us / 1000;
    ctx.t_entry_ms = ctx.t_now_ms;
    ctx.t_second_ms = ctx.t_now_ms;
    ctx.t_prev_us = ctx.t_now_us;
    Serial.printf("[AUTOMAT] Started (10 ms polling cycle, core %d)\n",
                  (int)xPortGetCoreID());

    TickType_t xLastWakeTime = xTaskGetTickCount();
    for (;;) {
        /* Single time snapshot for the whole cycle: consistency and fewer
           calls to esp_timer_get_time(). */
        ctx.t_now_us = esp_timer_get_time();
        ctx.t_now_ms = ctx.t_now_us / 1000;

        read_inputs();          /* 1. input image       */
        evaluate_transitions(); /* 2. state machine     */
        execute_state();        /* 3. state actions     */
        write_outputs();        /* 4. output image      */

        /* Wait until next cycle: precise 10 ms period. */
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(10));
    }
}

/* ------------------------------------------------------------------ */
/* manufacturer OPC UA interface (ScrewStation/...)                    */
/* ------------------------------------------------------------------ */

/* ---- Ident/ : constants ---- */
static char identDevice[] = "ScrewStation SW-4L";
static char identSerial[] = "SW4L-2026-0001";
static char identFw[] = "0.2.0-idx";

/* ---- Diag/ : computed values, refreshed by the server ---- */
static uint32_t diagUptime = 0;
static int32_t diagFreeHeap = 0;
static int32_t diagRssi = 0;
static char diagIp[20] = "0.0.0.0";

/** @brief Registry of exposed variables (one entry per variable). */
static OpcUaProperty properties[] = {
    /* Ident/ */
    { "sDeviceName", 0, OPCUA_TYPE_STRING, OPCUA_ACCESS_READ, identDevice, "Ident", sizeof(identDevice) },
    { "sSerialNo",   0, OPCUA_TYPE_STRING, OPCUA_ACCESS_READ, identSerial, "Ident", sizeof(identSerial) },
    { "sFwVersion",  0, OPCUA_TYPE_STRING, OPCUA_ACCESS_READ, identFw,     "Ident", sizeof(identFw) },
    /* State/ */
    { "iState",     0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READ, &g_plant.iState,     "State", 0 },
    { "bAuto",      0, OPCUA_TYPE_BOOL,   OPCUA_ACCESS_READ, &g_plant.bAuto,      "State", 0 },
    { "bFault",     0, OPCUA_TYPE_BOOL,   OPCUA_ACCESS_READ, &g_plant.bFault,     "State", 0 },
    { "iAlarmCode", 0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READ, &g_plant.iAlarmCode, "State", 0 },
    /* Cycle/ */
    { "sOrderId",    0, OPCUA_TYPE_STRING, OPCUA_ACCESS_READWRITE, g_plant.sOrderId,    "Cycle", sizeof(g_plant.sOrderId) },
    { "iCurrentLeg", 0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READ,      &g_plant.iCurrentLeg, "Cycle", 0 },
    { "sRfidUid",    0, OPCUA_TYPE_STRING, OPCUA_ACCESS_READ,      g_plant.sRfidUid,     "Cycle", sizeof(g_plant.sRfidUid) },
    { "sRfidData",   0, OPCUA_TYPE_STRING, OPCUA_ACCESS_READ,      g_plant.sRfidData,    "Cycle", sizeof(g_plant.sRfidData) },
    { "tCycleTime",  0, OPCUA_TYPE_DOUBLE, OPCUA_ACCESS_READ,      &g_plant.tCycleTime,  "Cycle", 0 },
    { "tsLastCycle", 0, OPCUA_TYPE_STRING, OPCUA_ACCESS_READ,      g_plant.tsLastCycle,  "Cycle", sizeof(g_plant.tsLastCycle) },
    /* Position/ */
    { "iDetentsLeg1", 0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READ, &g_plant.iDetentsLeg[0], "Position", 0 },
    { "iDetentsLeg2", 0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READ, &g_plant.iDetentsLeg[1], "Position", 0 },
    { "iDetentsLeg3", 0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READ, &g_plant.iDetentsLeg[2], "Position", 0 },
    { "iDetentsLeg4", 0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READ, &g_plant.iDetentsLeg[3], "Position", 0 },
    { "rAngleLeg1",   0, OPCUA_TYPE_DOUBLE, OPCUA_ACCESS_READ, &g_plant.rAngleLeg[0],   "Position", 0 },
    { "rAngleLeg2",   0, OPCUA_TYPE_DOUBLE, OPCUA_ACCESS_READ, &g_plant.rAngleLeg[1],   "Position", 0 },
    { "rAngleLeg3",   0, OPCUA_TYPE_DOUBLE, OPCUA_ACCESS_READ, &g_plant.rAngleLeg[2],   "Position", 0 },
    { "rAngleLeg4",   0, OPCUA_TYPE_DOUBLE, OPCUA_ACCESS_READ, &g_plant.rAngleLeg[3],   "Position", 0 },
    { "bLegOk1",      0, OPCUA_TYPE_BOOL,   OPCUA_ACCESS_READ, &g_plant.bLegOk[0],      "Position", 0 },
    { "bLegOk2",      0, OPCUA_TYPE_BOOL,   OPCUA_ACCESS_READ, &g_plant.bLegOk[1],      "Position", 0 },
    { "bLegOk3",      0, OPCUA_TYPE_BOOL,   OPCUA_ACCESS_READ, &g_plant.bLegOk[2],      "Position", 0 },
    { "bLegOk4",      0, OPCUA_TYPE_BOOL,   OPCUA_ACCESS_READ, &g_plant.bLegOk[3],      "Position", 0 },
    { "bPartOk",      0, OPCUA_TYPE_BOOL,   OPCUA_ACCESS_READ, &g_plant.bPartOk,        "Position", 0 },
    { "iStepsCmd",    0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READ, &g_plant.iStepsCmd,      "Position", 0 },
    { "iEncoderCnt",  0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READ, &g_plant.iEncoderCnt,    "Position", 0 },
    { "rDeltaDeg",    0, OPCUA_TYPE_DOUBLE, OPCUA_ACCESS_READ, &g_plant.rDeltaDeg,      "Position", 0 },
    /* Counters/ */
    { "nGood", 0, OPCUA_TYPE_UINT32, OPCUA_ACCESS_READ, &g_plant.nGood, "Counters", 0 },
    { "nBad",  0, OPCUA_TYPE_UINT32, OPCUA_ACCESS_READ, &g_plant.nBad,  "Counters", 0 },
    { "tRun",  0, OPCUA_TYPE_UINT32, OPCUA_ACCESS_READ, &g_plant.tRun,  "Counters", 0 },
    { "tIdle", 0, OPCUA_TYPE_UINT32, OPCUA_ACCESS_READ, &g_plant.tIdle, "Counters", 0 },
    { "tStop", 0, OPCUA_TYPE_UINT32, OPCUA_ACCESS_READ, &g_plant.tStop, "Counters", 0 },
    { "tFault",0, OPCUA_TYPE_UINT32, OPCUA_ACCESS_READ, &g_plant.tFault,"Counters", 0 },
    /* Settings/ (write allowed) */
    { "rTargetDeg", 0, OPCUA_TYPE_DOUBLE, OPCUA_ACCESS_READWRITE, &g_plant.rTargetDeg, "Settings", 0 },
    { "rTolDeg",    0, OPCUA_TYPE_DOUBLE, OPCUA_ACCESS_READWRITE, &g_plant.rTolDeg,    "Settings", 0 },
    { "rSlipPct",   0, OPCUA_TYPE_DOUBLE, OPCUA_ACCESS_READWRITE, &g_plant.rSlipPct,   "Settings", 0 },
    { "iSpeed",     0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READWRITE, &g_plant.iSpeed,     "Settings", 0 },
    { "bSimMode",   0, OPCUA_TYPE_BOOL,   OPCUA_ACCESS_READWRITE, &g_plant.bSimMode,   "Settings", 0 },
    /* Cmd/ (pulses) */
    { "bStart",         0, OPCUA_TYPE_BOOL, OPCUA_ACCESS_READWRITE, &g_plant.cmdStart,        "Cmd", 0 },
    { "bStop",          0, OPCUA_TYPE_BOOL, OPCUA_ACCESS_READWRITE, &g_plant.cmdStop,         "Cmd", 0 },
    { "bAckFault",      0, OPCUA_TYPE_BOOL, OPCUA_ACCESS_READWRITE, &g_plant.cmdAck,          "Cmd", 0 },
    { "bResetCounters", 0, OPCUA_TYPE_BOOL, OPCUA_ACCESS_READWRITE, &g_plant.cmdResetCounters,"Cmd", 0 },
    /* Diag/ */
    { "tUptime",   0, OPCUA_TYPE_UINT32, OPCUA_ACCESS_READ, &diagUptime,   "Diag", 0 },
    { "iFreeHeap", 0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READ, &diagFreeHeap, "Diag", 0 },
    { "iRssi",     0, OPCUA_TYPE_INT32,  OPCUA_ACCESS_READ, &diagRssi,     "Diag", 0 },
    { "sIpAddr",   0, OPCUA_TYPE_STRING, OPCUA_ACCESS_READ, diagIp,        "Diag", sizeof(diagIp) },
};

/** @brief Refreshes computed values (Diag/) before reading. */
static void screwstationUpdate(void)
{
    diagUptime = (uint32_t)(esp_timer_get_time() / 1000000);
    diagFreeHeap = (int32_t)ESP.getFreeHeap();
    diagRssi = wifi_get_rssi();
    wifi_get_ip(diagIp, sizeof(diagIp));
}

/* locks passed to the OPC UA server to protect accesses to g_plant */
static void screwstation_lock(void* ctx)
{
    xSemaphoreTake(*(SemaphoreHandle_t*)ctx, portMAX_DELAY);
}

static void screwstation_unlock(void* ctx)
{
    xSemaphoreGive(*(SemaphoreHandle_t*)ctx);
}

void screwstation_init()
{
    /* create the mutex protecting the machine image */
    g_plant_mux = xSemaphoreCreateMutex();
    if (g_plant_mux == NULL)
    {
        Serial.println("[SCREWSTATION] Failed to create plant mutex!");
        return;
    }

    /* default settings (scenario v0.4: quarter-turn indexing) */
    g_plant.iState = STATE_INIT;
    g_plant.bAuto = true;
    g_plant.bFault = false;
    g_plant.iAlarmCode = ALARM_NONE;
    strlcpy(g_plant.sOrderId, "OF-2026-0001", sizeof(g_plant.sOrderId));
    strlcpy(g_plant.tsLastCycle, "-", sizeof(g_plant.tsLastCycle));
    g_plant.rTargetDeg = 90.0; /* one leg every quarter turn */
    g_plant.rTolDeg = 13.0;    /* 7 detents (84°) and 8 detents (96°) pass */
    g_plant.rSlipPct = 10.0;   /* 10 % simulated slip -> FAULT */
    g_plant.iSpeed = 300;
    g_plant.bSimMode = true; /* bare board: detents and badge simulated */

    /* real timestamp for Cycle/tsLastCycle (Wi-Fi already connected) */
    configTime(0, 0, "pool.ntp.org");

    /* automaton starts right away: the machine runs even without a network */
    xTaskCreatePinnedToCore(automate_task, "automate", 8192, NULL, 12, NULL, 1);

    /* local HMI (LCD + encoder #2): independent operator panel, low priority
       — the firmware works identically without a display */
    xTaskCreatePinnedToCore(local_hmi_task, "local_hmi", 4096, NULL, 5, NULL, 0);
}

OpcUaObject screwstation_get_object()
{
    OpcUaObject object;
    object.name = "ScrewStation";
    object.nodeId = 0;
    object.properties = properties;
    object.propertyCount = sizeof(properties) / sizeof(properties[0]);
    object.onUpdate = screwstationUpdate;
    object.onLock = screwstation_lock;
    object.onUnlock = screwstation_unlock;
    object.lockContext = &g_plant_mux;
    return object;
}
