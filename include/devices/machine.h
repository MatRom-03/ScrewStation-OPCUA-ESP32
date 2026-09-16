/*
 * machine.h — shared definitions for the A1 screwing automaton
 * ==========================================================
 * Four-leg stool screwing machine.
 * The complete machine state lives in a single structure (plant_t),
 * shared between the automaton task, the local HMI task (LCD + encoder)
 * and the OPC UA server, protected by a spinlock (very short sections).
 *
 * Ported from the student kit "Les Meubles du Futur" (MIT) to fit the
 * generic `src/devices/` architecture of the ScrewStation OPCUA - ESP-32 project.
 */
#ifndef MACHINE_H
#define MACHINE_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ---- Pinout (same for bare board / wired board) ---- */
#define PIN_STEP_IN1 GPIO_NUM_26
#define PIN_STEP_IN2 GPIO_NUM_25
#define PIN_STEP_IN3 GPIO_NUM_33
#define PIN_STEP_IN4 GPIO_NUM_32
#define PIN_LED_GREEN GPIO_NUM_16
#define PIN_LED_ORANGE GPIO_NUM_2 /* also onboard blue LED: state visible on bare board */
#define PIN_LED_RED GPIO_NUM_15
#define PIN_BTN_START GPIO_NUM_27
#define PIN_BTN_STOP GPIO_NUM_14
#define PIN_BTN_ACK GPIO_NUM_13
/* Local HMI: LCD 1602 I2C + encoder #2 for navigation.
   GPIO 36/39: input-only, NO internal pull-up — the KY-040 module has
   on-board resistors; a bare encoder requires external 10 kΩ resistors. */
#define PIN_I2C_SDA GPIO_NUM_21
#define PIN_I2C_SCL GPIO_NUM_22
#define PIN_ENC2_CLK GPIO_NUM_36
#define PIN_ENC2_DT GPIO_NUM_39
#define PIN_ENC2_SW GPIO_NUM_4
/* RFID RC522 reader on VSPI — 3.3 V mandatory */
#define PIN_RFID_SCK GPIO_NUM_18
#define PIN_RFID_MISO GPIO_NUM_19
#define PIN_RFID_MOSI GPIO_NUM_23
#define PIN_RFID_SS GPIO_NUM_5
#define PIN_RFID_RST GPIO_NUM_17

/* ---- Drive states (codes = OPC UA State/iState value) ---- */
typedef enum {
    STATE_INIT = 0,
    STATE_STOP = 1,
    STATE_READY = 2,
    STATE_WAITING_COMPONENT = 3,
    STATE_SCREWING = 4,
    STATE_END_CYCLE = 5,
    STATE_SUSPENDED = 6,
    STATE_FAULT = 7,
} state_t;

/* Leg sub-sequence during SCREWING (the machine is an INDEXING TABLE — the
   screwing itself is symbolic):
   INDEXING    = quarter-turn rotation (present the leg facing the index)
   TIGHTENING  = leg screwing, motor STOPPED (by hand / timed)
   CONTROL     = POSITIONING verdict (encoder detents counted) */
typedef enum {
    LEG_INDEXING = 0,
    LEG_TIGHTENING = 1,
    LEG_CONTROL = 2,
} leg_phase_t;

/* Alarm codes (State/iAlarmCode) — positioning fault */
#define ALARM_NONE 0
#define ALARM_POSITION_SHORT 1 /* insufficient rotation (slippage…) */
#define ALARM_POSITION_LONG 2  /* excessive rotation */

/* ---- Indexing: encoder #1, pressed against the stool through the cup,
   measures the REAL rotation. 30 detents/turn -> a quarter turn is 7.5
   detents: it NEVER lands exactly (7 or 8 detents stepsed) — this is the
   core of the exercise, students compensate on the SCADA side. ---- */
#define DETENTS_PER_TURN 30
#define DEG_PER_DETENT (360.0 / DETENTS_PER_TURN) /* 12° */
#define HALF_STEPS_PER_TURN 4096 /* 28BYJ-48 in half-step mode (gear ratio 64:1) */
#define HALF_STEPS_PER_QUARTER (HALF_STEPS_PER_TURN / 4)

/* ---- Complete machine image (mirror of the ScrewStation interface) ---- */
typedef struct {
    /* State/ */
    int32_t iState;
    bool bAuto;
    bool bFault;
    int32_t iAlarmCode;
    /* Cycle/ */
    char sOrderId[24];
    int32_t iCurrentLeg;   /* 0 = none, 1..4 during SCREWING */
    char sRfidUid[24];     /* badge hex UID = identifier */
    char sRfidData[20];    /* optional content (16 ASCII chars, "" otherwise) */
    double tCycleTime;     /* last cycle duration [s] */
    char tsLastCycle[24];  /* timestamp of last cycle end */
    /* Position/ — stool indexing */
    int32_t iDetentsLeg[4]; /* counted encoder detents, legs 1..4 */
    double rAngleLeg[4];    /* corresponding angle [°] (detents × 12) */
    bool bLegOk[4];
    bool bPartOk;
    int32_t iStepsCmd;      /* commanded half-steps (current cycle) */
    int32_t iEncoderCnt;    /* accumulated cycle detents (simulated if bSimMode) */
    double rDeltaDeg;       /* last leg deviation from target [°] */
    /* Counters/ */
    uint32_t nGood;
    uint32_t nBad;
    uint32_t tRun;   /* accumulated times by state family [s] */
    uint32_t tIdle;
    uint32_t tStop;
    uint32_t tFault;
    /* Settings/ (OPC UA write allowed) */
    double rTargetDeg;   /* target rotation per leg [°] (90) */
    double rTolDeg;      /* positioning tolerance [± °] */
    double rSlipPct;     /* friction slip probability [%] */
    int32_t iSpeed;      /* indexing speed [half-steps/s] */
    bool bSimMode;       /* true = bare board: detents and badge simulated */
    /* Cmd/ (impulses set by OPC UA, consumed by the automaton) */
    bool cmdStart;
    bool cmdStop;
    bool cmdAck;
    bool cmdResetCounters;
} plant_t;

extern plant_t g_plant;
extern SemaphoreHandle_t g_plant_mux;

/* RULE: between PLANT_LOCK and PLANT_UNLOCK, only memory reads/copies.
 * Never call anything that may block or take another lock — printf/snprintf,
 * time/localtime/strftime, malloc, ESP_LOG… — : keep the critical section
 * as short as possible. Format BEFORE, copy UNDER the lock. */
#define PLANT_LOCK() xSemaphoreTake(g_plant_mux, portMAX_DELAY)
#define PLANT_UNLOCK() xSemaphoreGive(g_plant_mux)

/* Automaton task: 10 ms polling cycle */
void automate_task(void *arg);

/* Local HMI task (LCD + encoder #2): independent operator panel */
void local_hmi_task(void *arg);

#endif /* MACHINE_H */
