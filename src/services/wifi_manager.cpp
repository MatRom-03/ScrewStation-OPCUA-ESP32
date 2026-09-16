/**
 * @file wifi_manager.cpp
 * @brief Implementation of Wi-Fi connection management.
 */

#include "services/wifi_manager.h"
#include "config.h"
#include <WiFi.h>

void wifi_init()
{
    Serial.println("[WIFI] Connecting to Wi-Fi network...");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECTION_TIMEOUT_MS)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WIFI] Connection failed. Restarting in 5 seconds.");
        delay(5000);
        ESP.restart();
    }

    Serial.println("[WIFI] Connected to Wi-Fi network!");
    wifi_print_status();
}

bool wifi_is_connected()
{
    return WiFi.status() == WL_CONNECTED;
}

void wifi_get_ip(char* buf, size_t len)
{
    if (buf == NULL || len == 0)
    {
        return;
    }
    IPAddress ip = WiFi.localIP();
    snprintf(buf, len, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

int32_t wifi_get_rssi()
{
    return WiFi.RSSI();
}

void wifi_print_status()
{
    Serial.print("[WIFI] SSID       : ");
    Serial.println(WIFI_SSID);
    Serial.print("[WIFI] IP address : ");
    Serial.println(WiFi.localIP());
    Serial.print("[WIFI] RSSI       : ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
}

static bool s_reconnecting = false;
static unsigned long s_reconnect_start = 0;

void wifi_check()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        if (s_reconnecting)
        {
            s_reconnecting = false;
            Serial.println("[WIFI] Reconnected!");
            wifi_print_status();
        }
        return;
    }

    if (!s_reconnecting)
    {
        Serial.println("[WIFI] Connection lost. Reconnecting...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        s_reconnecting = true;
        s_reconnect_start = millis();
        return;
    }

    if (millis() - s_reconnect_start >= WIFI_CONNECTION_TIMEOUT_MS)
    {
        Serial.println("[WIFI] Reconnection attempt timed out, will retry.");
        s_reconnecting = false;
    }
}
