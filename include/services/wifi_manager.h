/**
 * @file wifi_manager.h
 * @brief Wi-Fi connection management for the ESP32.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

/**
 * @brief Initializes and connects the ESP32 to the configured Wi-Fi network.
 *        Restarts the ESP32 if the connection fails.
 */
void wifi_init();

/**
 * @brief Checks if the ESP32 is currently connected to a Wi-Fi network.
 * @return true if connected, false otherwise.
 */
bool wifi_is_connected();

/**
 * @brief Writes the current local IP address into the provided buffer.
 * @param buf  Destination buffer.
 * @param len  Size of the destination buffer.
 */
void wifi_get_ip(char* buf, size_t len);

/**
 * @brief Returns the current Wi-Fi signal strength.
 * @return RSSI value in dBm.
 */
int32_t wifi_get_rssi();

/**
 * @brief Prints the current Wi-Fi status to the serial port.
 */
void wifi_print_status();

/**
 * @brief Checks the Wi-Fi connection and reconnects if necessary.
 *
 * Should be called periodically from the main loop or a task.
 */
void wifi_check();

#endif
