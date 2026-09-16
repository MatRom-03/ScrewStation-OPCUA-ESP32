# 🚀 Procédure opératoire — cycle machine

[🏠 Accueil du projet](../README.md) · [📖 Documentation](README.md)

Ce document décrit la **séquence de mise en route et de cycle** de la
visseuse A1, avec les actions à faire sur les boutons physiques, le badge
RFID et l'écran LCD. La même logique est accessible via OPC UA (boutons
`Cmd/bStart`, `Cmd/bStop`, `Cmd/bAckFault`).

---

## 1. Mise sous tension

Au boot, l'ESP32 :

1. Initialise l'automate et l'image machine.
2. Se connecte au Wi-Fi configuré.
3. Démarre le serveur OPC UA sur le port `4840`.
4. Détecte l'écran LCD I2C (adresses `0x27` puis `0x3F`).
5. Passe en état `INIT` (LED orange clignote) pendant 500 ms.

> Si aucun lecteur RC522 n'est détecté, la machine passe automatiquement en
> **mode simulation** : les crans d'encodeur et le badge sont simulés.

---

## 2. Séquence normale d'un cycle

### Étape 1 — Arrêt (`STOP`)

- **État LCD** : `STOP`
- **LED** : orange fixe
- **Action** : appuyer sur le bouton **START** (GPIO 27).

→ Transition vers `READY`.

### Étape 2 — Prêt (`READY`)

- **État LCD** : `READY`
- **LED** : verte clignote
- **Action** : appuyer à nouveau sur **START** pour lancer la production.

→ Transition vers `WAITING_COMPONENT`.

### Étape 3 — Attente composant (`WAITING_COMPONENT`)

- **État LCD** : `WAIT`
- **LED** : verte clignote
- **Action** : présenter un **badge RFID** devant le lecteur.
  - **Mode réel** : badgez une carte compatible MIFARE (n'importe laquelle).
  - **Mode simulation** : attendre 2 s sans badge ; un UID simulé est généré
    automatiquement.

→ Transition vers `SCREWING`, pied 1/4.

> Le badge n'est accepté que sur **arrivée** : il faut l'avoir retiré
> auparavant pendant au moins 3 lectures (≈ 300 ms) pour pouvoir relancer un
> cycle. Laisser le badge sur le lecteur ne redémarre pas un cycle.

### Étape 4 — Vissage (`SCREWING`)

La machine exécute la même séquence pour chacun des 4 pieds :

| Sous-étape | Durée max | Description |
|---|---|---|
| **INDEXING** | 8 s | Le moteur pas-à-pas tourne d'un quart de tour pour amener le pied suivant face au poste de serrage. |
| **TIGHTENING** | 2 s | Le moteur est arrêté : le serrage est **symbolique** (le pied peut être emmanché à la main sur le démonstrateur). |
| **CONTROL** | 0,5 s | Mesure de la position (encodeur #1 ou simulation) et verdict OK/NOK. |

- **État LCD** : `SCREW` avec le pied courant, ex. `SCREW 2/4`
- **LED** : verte fixe

Si un pied est **hors tolérance**, la machine passe en `FAULT`.

Si les 4 pieds sont conformes, la machine passe en `END_CYCLE`.

### Étape 5 — Fin de cycle (`END_CYCLE`)

- **État LCD** : `END CYC`
- **LED** : verte fixe
- **Durée** : 1 s
- **Résultat** :
  - Le compteur **OK** est incrémenté.
  - `bPartOk` passe à `true`.
  - L'horodatage et le temps de cycle sont mis à jour.

→ Retour automatique en `WAITING_COMPONENT` pour le cycle suivant (même ordre
de production).

---

## 3. Boutons physiques

| Bouton | GPIO | Effet |
|---|---|---|
| **START** (vert) | 27 | En `STOP` → passe en `READY`  <br>En `READY` → passe en `WAITING_COMPONENT`  <br>En `SUSPENDED` → repasse en `READY` |
| **STOP** (rouge) | 14 | En `READY` ou `WAITING_COMPONENT` → `SUSPENDED`  <br>En `SCREWING` → demande l'arrêt en fin de pied courant  <br>En `END_CYCLE` → `SUSPENDED` |
| **ACK** (jaune) | 13 | En `FAULT` → acquitte le défaut et retourne en `STOP` |

> Les boutons sont actifs bas avec pull-up interne et anti-rebond 30 ms.

---

## 4. Signification des LEDs

| État machine | LED verte | LED orange | LED rouge |
|---|---|---|---|
| `INIT` | éteinte | clignote | éteinte |
| `STOP` | éteinte | fixe | éteinte |
| `READY` / `WAITING_COMPONENT` | clignote | éteinte | éteinte |
| `SCREWING` / `END_CYCLE` | fixe | éteinte | éteinte |
| `SUSPENDED` | éteinte | clignote | éteinte |
| `FAULT` | éteinte | éteinte | fixe |

---

## 5. Gestion des défauts (`FAULT`)

Un défaut se produit quand un pied dévie de plus de la tolérance réglée
(`rTolDeg`) par rapport à la cible (`rTargetDeg`, usine 90°).

- **ALARM_SHORT** (`iAlarmCode = 1`) : angle trop faible (glissement friction).
- **ALARM_LONG** (`iAlarmCode = 2`) : angle trop élevé.

**Action utilisateur** :

1. Lire l'alarme sur l'écran LCD (`STA` → `ALARM SHORT` ou `ALARM LONG`).
2. Corriger ou noter le problème.
3. Appuyer sur **ACK** (GPIO 13).

→ Retour en `STOP`. Il faut alors appuyer sur **START** deux fois pour relancer
un nouveau cycle.

---

## 6. Réglages via l'écran LCD

1. Naviguer jusqu'à l'écran `SET` avec l'encodeur n°2.
2. Appuyer sur le bouton de l'encodeur pour entrer en mode édition.
3. Modifier les paramètres :

| Paramètre | Valeur usine | Rôle |
|---|---|---|
| `Target` | 90° | Angle cible d'indexage par pied |
| `Tol` | 13° | Tolérance acceptée autour de la cible |
| `Slip` | 10 % | Probabilité de glissement friction (mode simulation) |
| `Speed` | 300 pas/s | Vitesse du moteur d'indexage |

4. Appuyer pour passer au paramètre suivant ; après le 4e, retour à la
   navigation.

> Les réglages s'appliquent **immédiatement** et sont aussi accessibles en
> écriture via OPC UA (`ScrewStation/Settings/...`).

---

## 7. Exemple complet d'un cycle réussi

1. **Allumer** l'ESP32.
2. Attendre `STOP` (LED orange fixe).
3. Appuyer sur **START** → `READY` (LED verte clignote).
4. Appuyer sur **START** → `WAIT` (LED verte clignote).
5. Présenter un badge RFID → `SCREW 1/4` (LED verte fixe).
6. La machine indexe, serre symboliquement et contrôle chaque pied.
7. Si les 4 pieds sont OK → `END CYC` pendant 1 s, compteur OK +1.
8. Retour automatique en `WAIT` pour le cycle suivant.

---

## 8. Mode simulation (carte nue)

Si aucun lecteur RC522 n'est détecté au boot :

- `bSimMode = true`
- Le badge est généré automatiquement après 2 s en `WAITING_COMPONENT`.
- Les crans d'encodeur sont simulés (7 ou 8 crans, avec glissement possible).

Cela permet de tester la logique automate et OPC UA sans le matériel complet.

---

## 9. Récapitulatif des états

| Code | État | Signification |
|---|---|---|
| 0 | `INIT` | Initialisation au boot |
| 1 | `STOP` | Arrêt, attente de START |
| 2 | `READY` | Prêt, attente de START pour charger un composant |
| 3 | `WAITING_COMPONENT` | Attente du badge RFID |
| 4 | `SCREWING` | Indexage + serrage + contrôle des 4 pieds |
| 5 | `END_CYCLE` | Cycle terminé avec succès |
| 6 | `SUSPENDED` | Production suspendue par STOP |
| 7 | `FAULT` | Défaut de position, acquittement nécessaire |

---

[🏠 Accueil](../README.md) · [📖 Sommaire docs](README.md) · [🖥️ IHM locale](hmi.md) · [🔗 OPC UA](opcua.md)
