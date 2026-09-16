/**
 * @file config.cpp
 * @brief Definitions of the global configuration constants.
 */

#include "config.h"

const char* WIFI_SSID = "HUB-WIFI-MPM";
const char* WIFI_PASSWORD = "12345678";

const size_t OPCUA_SEND_BUFFER = 8192;
const size_t OPCUA_RECV_BUFFER = 8192;
const uint16_t OPCUA_PORT = 4840;

const unsigned long PUBLISH_INTERVAL_MS = 2000;
const unsigned long WIFI_STATUS_INTERVAL_MS = 30000;
const unsigned long WIFI_CONNECTION_TIMEOUT_MS = 20000;
