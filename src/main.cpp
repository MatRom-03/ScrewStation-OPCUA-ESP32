/**
 * @file main.cpp
 * @brief Entry point of the ESP32 OPC UA + Wi-Fi project.
 *
 * Initializes the devices, connects to Wi-Fi, then starts the OPC UA server
 * task on core 1 with the array of devices to expose.
 */

#include <Arduino.h>
#include "config.h"
#include "services/wifi_manager.h"
#include "opcua_object.h"
#include "services/opcua_server.h"
#include "devices/screwstation.h"
#include <esp_task_wdt.h>

/**
 * @brief Arduino setup function. Runs once at startup.
 */
void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n==============================");
    Serial.println("  ESP32 OPC UA + Wi-Fi STA");
    Serial.println("  ScrewStation (A1 automaton)");
    Serial.println("==============================");

    wifi_init();
    screwstation_init();

    // Declare here every object to expose over OPC UA.
    OpcUaObject devices[] = {
        screwstation_get_object(),
    };

    Serial.printf("[SYSTEM] Free heap before OPC UA: %d bytes\n", ESP.getFreeHeap());

    opcua_server_start_task(devices, sizeof(devices) / sizeof(devices[0]));
}

/**
 * @brief Arduino loop function. Runs repeatedly on core 1.
 *        The OPC UA server runs in its own task; loop only feeds the watchdog.
 */
void loop()
{
    esp_task_wdt_reset();
    delay(1000);
}
