/**
 * @file config.h
 * @brief Global configuration constants for the ESP32 OPC UA project.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==================== WIFI CONFIGURATION ====================
extern const char* WIFI_SSID;       /**< Wi-Fi network SSID. */
extern const char* WIFI_PASSWORD;   /**< Wi-Fi network stepsword. */

// ==================== OPC UA CONFIGURATION ====================
extern const size_t OPCUA_SEND_BUFFER;   /**< OPC UA send buffer size in bytes. */
extern const size_t OPCUA_RECV_BUFFER;   /**< OPC UA receive buffer size in bytes. */
extern const uint16_t OPCUA_PORT;        /**< OPC UA server TCP port. */

// ==================== TIMING CONFIGURATION ====================
extern const unsigned long PUBLISH_INTERVAL_MS;         /**< Interval between sensor reads/publishes. */
extern const unsigned long WIFI_STATUS_INTERVAL_MS;     /**< Interval between Wi-Fi status prints. */
extern const unsigned long WIFI_CONNECTION_TIMEOUT_MS;  /**< Wi-Fi connection timeout in milliseconds. */

// ==================== DEBUG CONFIGURATION ====================
#define DEBUG_AUTOMATON 1 /**< Set to 0 to disable automaton state/leg logs on Serial. */

#endif
