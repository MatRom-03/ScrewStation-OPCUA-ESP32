/**
 * @file screwstation.h
 * @brief A1 screwing automaton exposed as an OPC UA device.
 *
 * The device encapsulates the full automaton (state machine, GPIO, RFID, HMI)
 * and exposes the manufacturer-style OPC UA interface.
 */

#ifndef SCREWSTATION_H
#define SCREWSTATION_H

#include "opcua_object.h"

/**
 * @brief Initializes the machine image, the clock (SNTP) and starts the
 *        automaton (10 ms polling cycle) and local HMI tasks.
 */
void screwstation_init();

/**
 * @brief Returns the OPC UA object describing the machine and its properties.
 *
 * Properties (by folder):
 *   - Ident     : sDeviceName, sSerialNo, sFwVersion
 *   - State     : iState, bAuto, bFault, iAlarmCode
 *   - Cycle     : sOrderId(w), iCurrentLeg, sRfidUid, sRfidData,
 *                 tCycleTime, tsLastCycle
 *   - Position  : iDetentsLeg1..4, rAngleLeg1..4, bLegOk1..4, bPartOk,
 *                 iStepsCmd, iEncoderCnt, rDeltaDeg
 *   - Counters  : nGood, nBad, tRun, tIdle, tStop, tFault
 *   - Settings  : rTargetDeg(w), rTolDeg(w), rSlipPct(w), iSpeed(w),
 *                 bSimMode(w)
 *   - Cmd       : bStart(w), bStop(w), bAckFault(w), bResetCounters(w)
 *   - Diag      : tUptime, iFreeHeap, iRssi, sIpAddr
 */
OpcUaObject screwstation_get_object();

#endif
