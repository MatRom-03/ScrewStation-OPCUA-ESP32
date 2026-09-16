# 📶 Fonctionnement du Wi-Fi

[🏠 Accueil](../README.md) · [📖 Sommaire](README.md) · [🏗️ Architecture](architecture.md) · [🔌 Montage](montage.md) · [🔗 OPC UA](opcua.md)

L'ESP32 se connecte au réseau en **mode station (STA)** : il rejoint un réseau
Wi-Fi existant (hotspot de TP, box…) et obtient une adresse IP par DHCP. C'est
cette adresse qui sert d'endpoint au serveur OPC UA.

Code concerné : [`src/services/wifi_manager.cpp`](../src/services/wifi_manager.cpp)
et [`include/services/wifi_manager.h`](../include/services/wifi_manager.h).

---

## 1. Où configurer le réseau

Les identifiants sont des **constantes compilées**, définies dans
**`src/config.cpp`** :

```cpp
// src/config.cpp
const char* WIFI_SSID = "HUB-WIFI-MPM";   // nom du réseau (2,4 GHz)
const char* WIFI_PASSWORD = "12345678";   // mot de passe

const unsigned long WIFI_STATUS_INTERVAL_MS = 30000;      // statut toutes les 30 s
const unsigned long WIFI_CONNECTION_TIMEOUT_MS = 20000;   // timeout de connexion
```

> Après modification, **recompilez et reflashez** : les valeurs sont intégrées au
> firmware, pas lues depuis un fichier de configuration externe.

---

## 2. Déroulement de la connexion

La fonction `wifi_init()` (appelée une fois dans `setup()`, avant
`screwstation_init()`) exécute les étapes suivantes :

1. **Réinitialisation** de l'interface Wi-Fi.
2. Passage en **mode station** (`WiFi.mode(WIFI_STA)`).
3. **Désactivation du modem-sleep** (`WiFi.setSleep(false)`).
4. `WiFi.begin(SSID, PASSWORD)` : lancement de l'association.
5. **Attente active** de `WL_CONNECTED`, par pas de 500 ms, jusqu'à
   `WIFI_CONNECTION_TIMEOUT_MS` (20 s par défaut).
6. **En cas d'échec** : message d'erreur, attente de 5 s puis
   **redémarrage de la carte** (`ESP.restart()`).
7. En cas de succès : affichage du SSID, de l'IP et du RSSI.

```text
[WIFI] Connecting to Wi-Fi network...
.....
[WIFI] Connected to Wi-Fi network!
[WIFI] SSID       : HUB-WIFI-MPM
[WIFI] IP address : 192.168.1.42
[WIFI] RSSI       : -58 dBm
```

### Pourquoi désactiver le modem-sleep ?

Par défaut, l'ESP32 endort périodiquement sa radio (modem-sleep) pour économiser
l'énergie. Cela introduit des **pics de latence de plusieurs centaines de
millisecondes** qui font chuter les sessions OPC UA des clients. Comme la carte
est alimentée en permanence, on force la radio **toujours active**
(`setSleep(false)`).

---

## 3. API disponible

Déclarée dans `include/services/wifi_manager.h` :

| Fonction | Rôle |
|---|---|
| `wifi_init()` | Connexion au réseau au démarrage (bloquante, puis `ESP.restart()` si échec) |
| `wifi_is_connected()` | Retourne `true` si `WL_CONNECTED` |
| `wifi_get_ip()` | Adresse IP locale sous forme de `String` |
| `wifi_get_rssi()` | Puissance du signal en dBm |
| `wifi_print_status()` | Affiche SSID / IP / RSSI sur la console |

### Utilisations dans le projet

- **Serveur OPC UA** (`src/services/opcua_server.cpp`) :
  - l'**adresse IP** sert de *hostname* annoncé (`config.customHostname`) pour
    éviter les problèmes de résolution DNS chez les clients ;
  - l'**IP** est affichée dans l'URL de l'endpoint au démarrage ;
  - un **statut Wi-Fi** (connecté / IP / RSSI) est imprimé toutes les
    `WIFI_STATUS_INTERVAL_MS`.
- **IHM locale** (`src/devices/local_hmi.cpp`), écran `NET` : alternance entre
  le SSID, l'adresse IP et le port OPC UA toutes les 2 s.

---

## 4. Limites et dépannage

| Symptôme | Cause / solution |
|---|---|
| `Wi-Fi disconnected, reason: 201` en boucle | Réseau introuvable : mauvais nom, réseau **5 GHz** (l'ESP32 ne fait que du **2,4 GHz**), ou hotspot éteint |
| La carte redémarre en boucle | Mauvais SSID/mot de passe : la connexion échoue, le firmware redémarre après 5 s |
| Aucune IP affichée | Le DHCP n'a pas répondu dans les 20 s (réseau lent) : augmentez `WIFI_CONNECTION_TIMEOUT_MS` |
| Client OPC UA injoignable après une coupure Wi-Fi | La connexion n'est **pas** reconnectée automatiquement après le démarrage : si le lien tombe, le serveur devient injoignable jusqu'au prochain redémarrage |
| L'IP change à chaque boot | Normal en DHCP ; réservez une IP fixe sur le routeur pour un usage SCADA stable |

> L'ouverture du port série (moniteur) **réinitialise la carte** (DTR/RTS) : pour
> observer un démarrage, capturez la console **en direct** plutôt qu'en
> « post-mortem ».

---

[🏠 Accueil](../README.md) · [📖 Sommaire](README.md) · [🏗️ Architecture](architecture.md) · [🔌 Montage](montage.md) · [🔗 OPC UA](opcua.md)
