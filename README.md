# ScrewStation OPCUA - ESP-32 — Automate « visseuse » sur ESP32 avec serveur OPC UA

> Projet pédagogique : un **ESP32** programmé comme un **automate industriel**
> (cycle de scrutation, machine à états, image d'entrées/sorties) qui expose une
> **interface OPC UA de style « constructeur »**, le tout en Wi-Fi.

---

## 1. Ce que cherche à faire le projet

Le projet reproduit, sur une carte **ESP32 DevKit V1 (30 broches)**, le
comportement de la machine **A1 « visseuse »** du démonstrateur *Les Meubles du
Futur* : une **table d'indexage** qui présente les **4 pieds d'un tabouret** face
au poste de vissage.

Chaque pied subit trois phases : **INDEXAGE** (rotation d'un quart de tour par le
moteur pas-à-pas), **SERRAGE** symbolique (moteur arrêté), puis **CONTRÔLE** de
position. La rotation réelle est mesurée par un encodeur à **30 crans/tour** :
un quart de tour vaut 7,5 crans et ne tombe jamais juste (7 ou 8 crans). Hors
tolérance, la machine passe en **DÉFAUT** avec une alarme de position. Le
tabouret est identifié par un **badge RFID** (UID d'une carte ISO 14443-A).

Le programme vise trois objectifs :

1. **Faire vivre la machine** même sans réseau : l'automate tourne en boucle de
   scrutation à **10 ms**, indépendamment du Wi-Fi et d'OPC UA.
2. **Exposer l'état complet** de la machine sur un **serveur OPC UA** (port
   4840), afin d'être piloté et supervisé par un client SCADA (UaExpert,
   Ignition…). L'interface est volontairement hétérogène, « AS-IS », comme une
   machine du commerce.
3. **Fournir une IHM locale** (afficheur LCD 1602 + encodeur) : un pupitre
   autonome relié à la même image machine.

> Ce dépôt est l'adaptation, dans l'architecture générique `src/devices/`, du kit
> étudiants **« Les Meubles du Futur »** (auteur : Eric Truffet, code MIT).
> **Modifications et adaptations du code : Mattéo MOISANT.**

---

## 2. Fonctionnalités

| Fonction | Détail |
|---|---|
| Automate | Machine à états à 8 états, cycle de scrutation 10 ms (cœur 1) |
| Moteur | 28BYJ-48 + ULN2003, indexage d'un quart de tour (demi-pas) |
| Position | Mesure simulée 7/8 crans avec glissement, tolérance configurable |
| RFID | Lecteur RC522 (SPI), UID comme identifiant ; badge simulé si absent |
| IHM locale | LCD 1602 I2C + encodeur rotatif, 6 écrans, édition des réglages (cœur 0) |
| Voyants | 3 LED (verte / orange / rouge) pilotées par l'état machine |
| Commandes | 3 boutons (Start / Stop / Acquit) + commandes OPC UA |
| Réseau | Wi-Fi station |
| OPC UA | Serveur open62541, 48 variables réparties en 8 dossiers |

---

## 3. Configuration — à modifier dans `src/config.cpp`

⚠️ **Toute la configuration applicative se trouve dans `src/config.cpp`**
(les déclarations sont dans `include/config.h`). Avant la première compilation,
pensez notamment à renseigner le réseau Wi-Fi :

```cpp
// src/config.cpp
const char* WIFI_SSID = "HUB-WIFI-MPM";   // <-- à remplacer
const char* WIFI_PASSWORD = "12345678";   // <-- à remplacer

const size_t OPCUA_SEND_BUFFER = 8192;    // buffers réseau OPC UA (min. 8192)
const size_t OPCUA_RECV_BUFFER = 8192;
const uint16_t OPCUA_PORT = 4840;         // port du serveur OPC UA

const unsigned long PUBLISH_INTERVAL_MS = 2000;        // rafraîchissement OPC UA
const unsigned long WIFI_STATUS_INTERVAL_MS = 30000;   // impression statut Wi-Fi
const unsigned long WIFI_CONNECTION_TIMEOUT_MS = 20000; // timeout de connexion
```

> Les identifiants Wi-Fi sont **compilés dans le firmware** : après modification,
> il faut recompiler et reflasher la carte.

---

## 4. Démarrage rapide

Prérequis : **VS Code + extension PlatformIO** (ou PlatformIO Core en ligne de
commande).

```bash
pio run                  # compile
pio run -t upload        # flashe la carte (sinon --upload-port COM4)
pio device monitor       # console série (115200 bauds)
```

Au démarrage, la console affiche l'URL du serveur, par exemple :

```text
[OPC UA] Endpoint URL: opc.tcp://192.168.1.42:4840/
```

Avec un client OPC UA (UaExpert, Ignition) : **Security Policy = None**, **Mode
= None**, connexion **anonyme**, en utilisant de préférence **l'adresse IP**. Ne
pas ajouter `/discovery` à l'URL.

---

## 5. Matériel

- 1× ESP32 DevKit V1 (30 broches, module ESP32-WROOM-32, 4 Mo de flash)
- 1× ULN2003 + moteur pas-à-pas 28BYJ-48
- 1× afficheur LCD 1602 avec dos I2C (PCF8574, adresses 0x27 ou 0x3F)
- 1× encodeur rotatif KY-040 (+ 1 encodeur « n°1 » prévu sur l'axe, non codé)
- 1× lecteur RFID RC522 (3,3 V **uniquement**)
- 3× LED (vertes / orange / rouge) + résistances
- 3× boutons poussoirs
- 1× alimentation 5 V / 3 A (rail moteur) + condensateur de découplage
- Fils, plaques de prototypage, câble micro-USB de données

👉 Détail complet du câblage et des résistances : **[`docs/montage.md`](docs/montage.md)**

---

## 6. Documentation

| Document | Contenu |
|---|---|
| [`docs/README.md`](docs/README.md) | Sommaire et navigation de la documentation |
| [`docs/montage.md`](docs/montage.md) | Câblage, correspondance des GPIO, résistances, alimentation |
| [`docs/wifi.md`](docs/wifi.md) | Fonctionnement de la couche Wi-Fi |
| [`docs/opcua.md`](docs/opcua.md) | Fonctionnement du serveur OPC UA et arbre d'adressage |
| [`docs/architecture.md`](docs/architecture.md) | Organisation du code, tâches FreeRTOS, flux de données |
| [`docs/rapport-open62541-esp32.md`](docs/rapport-open62541-esp32.md) | Rapport technique de génération de la bibliothèque open62541 |

---

## 7. Structure du projet

```text
ScrewStation-OPCUA-ESP32/
├── README.md                     ← ce fichier
├── platformio.ini                ← environnement PlatformIO (esp32dev, Arduino)
├── docs/                         ← documentation (montage, wifi, opcua, archi, rapport open62541)
├── include/
│   ├── config.h                  ← déclarations de configuration
│   ├── opcua_object.h            ← modèle générique d'objet OPC UA
│   ├── devices/
│   │   ├── screwstation.h        ← API du device « visseuse »
│   │   ├── machine.h             ← image machine (g_plant), brochage, états
│   │   ├── rc522.h               ← API du lecteur RFID
│   │   └── local_hmi.h           ← API de l'IHM locale
│   └── services/
│       ├── opcua_server.h        ← serveur OPC UA générique
│       └── wifi_manager.h        ← gestion Wi-Fi
├── src/
│   ├── main.cpp                  ← point d'entrée
│   ├── config.cpp                ← valeurs de configuration  ★ à modifier
│   ├── devices/
│   │   ├── screwstation.cpp      ← automate + interface OPC UA
│   │   ├── rc522.cpp             ← pilote MFRC522
│   │   └── local_hmi.cpp         ← LCD + encodeur (Wire)
│   └── services/
│       ├── opcua_server.cpp      ← tâche serveur OPC UA
│       └── wifi_manager.cpp      ← connexion Wi-Fi
└── lib/open62541lib/             ← open62541 (amalgamation freertosLWIP)
```

Organisation détaillée : **[`docs/architecture.md`](docs/architecture.md)**

---

## 8. Crédits et licence

- Adaptation, modifications et intégration : **Mattéo MOISANT** — projet **ScrewStation OPCUA - ESP-32**.
- Machine, scénario et code d'origine : kit **« Les Meubles du Futur » — Automate
  A1 visseuse »**, © Eric Truffet, distribué sous licence **MIT**.
- Bibliothèque OPC UA : **open62541** (licence MPL-2.0).
