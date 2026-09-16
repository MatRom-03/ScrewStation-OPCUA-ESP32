# Documentation — ScrewStation OPCUA - ESP-32

[🏠 Accueil du projet](../README.md)

Bienvenue dans la documentation du projet. Chaque page est autonome et
accessible depuis ce sommaire ; une barre de navigation en haut de chaque page
permet de passer de l'une à l'autre.

---

## Sommaire

| # | Document | À quoi ça sert |
|---|---|---|
| 1 | [🏗️ Architecture](architecture.md) | Comment le code est organisé, les tâches FreeRTOS, le flux de données |
| 2 | [🔌 Montage](montage.md) | Câblage complet : correspondance des GPIO, composants, résistances, alimentation |
| 3 | [📶 Wi-Fi](wifi.md) | Comment fonctionne la connexion réseau et où la configurer |
| 4 | [🔗 OPC UA](opcua.md) | Comment fonctionne le serveur OPC UA et comment lire les variables |
| 5 | [🖥️ IHM locale](hmi.md) | Les 6 écrans LCD, la navigation et la modification des réglages |
| 6 | [🚀 Procédure opératoire](procedure.md) | Séquence de mise en route, cycle machine, boutons et badge RFID |
| 7 | [📚 Rapport open62541](rapport-open62541-esp32.md) | Génération de la bibliothèque OPC UA pour l'ESP32 |

---

## Par où commencer ?

- **Je veux brancher la carte** → [Montage](montage.md)
- **Je veux comprendre le programme** → [Architecture](architecture.md)
- **Je veux changer de réseau Wi-Fi** → [Wi-Fi](wifi.md) puis modifier `src/config.cpp`
- **Je veux me connecter depuis un SCADA** → [OPC UA](opcua.md)
- **Je veux utiliser l'écran et modifier les réglages** → [IHM locale](hmi.md)
- **Je veux savoir comment faire tourner la machine** → [Procédure opératoire](procedure.md)

---

## Rappel — configuration

Toute la configuration (Wi-Fi, port et buffers OPC UA, intervalles) se modifie
dans **`src/config.cpp`** (déclarations dans `include/config.h`). Il faut
**recompiler et reflasher** après un changement, car les valeurs sont compilées
dans le firmware.

---

[🏠 Accueil du projet](../README.md) · [🏗️ Architecture](architecture.md) · [🔌 Montage](montage.md) · [📶 Wi-Fi](wifi.md) · [🔗 OPC UA](opcua.md) · [🖥️ IHM locale](hmi.md) · [🚀 Procédure opératoire](procedure.md)
