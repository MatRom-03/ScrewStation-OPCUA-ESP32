# 🔗 Fonctionnement OPC UA

[🏠 Accueil](../README.md) · [📖 Sommaire](README.md) · [🏗️ Architecture](architecture.md) · [🔌 Montage](montage.md) · [📶 Wi-Fi](wifi.md)

Le projet embarque un **serveur OPC UA** qui expose l'état complet de la machine
et permet de la piloter. Il est construit sur la bibliothèque **open62541
v1.3.9** (format *amalgamation*, architecture `freertosLWIP`), située dans
`lib/open62541lib/`.

Code concerné :
- [`src/services/opcua_server.cpp`](../src/services/opcua_server.cpp) — la tâche serveur générique
- [`src/devices/screwstation.cpp`](../src/devices/screwstation.cpp) — la description des variables de la machine
- [`include/opcua_object.h`](../include/opcua_object.h) — le modèle générique d'objet

> Complément utile : [`rapport-open62541-esp32.md`](rapport-open62541-esp32.md),
> qui détaille la génération de la bibliothèque.

---

## 1. Configuration

Définie dans **`src/config.cpp`** :

```cpp
const size_t OPCUA_SEND_BUFFER = 8192;   // buffer réseau d'émission
const size_t OPCUA_RECV_BUFFER = 8192;   // buffer réseau de réception
const uint16_t OPCUA_PORT = 4840;        // port d'écoute TCP
const unsigned long PUBLISH_INTERVAL_MS = 2000; // rafraîchissement des objets
```

Caractéristiques du serveur :

| Élément | Valeur |
|---|---|
| Endpoint | `opc.tcp://<ip-de-la-carte>:4840/` |
| Security Policy | `None` |
| Mode | `None` |
| Authentification | anonyme |
| Nom d'hôte annoncé | l'adresse IP (évite les problèmes de DNS) |
| Tâche | cœur **1**, priorité 1, pile 12288 mots |
| Surveillance | watchdog (task WDT) relancé à chaque itération |

---

## 2. Fonctionnement interne

1. `opcua_server_start_task()` reçoit le **tableau des objets** à exposer et en
   fait une copie (les descripteurs doivent vivre tant que le serveur tourne).
2. Une tâche FreeRTOS dédiée est créée, épinglée au cœur 1.
3. Le serveur est configuré en buffers personnalisés (économie de RAM), avec un
   logger limité aux avertissements.
4. Les **nœuds** sont créés pour chaque objet et chaque propriété (voir §4).
5. La boucle serveur exécute `UA_Server_run_iterate()`, et toutes les
   `PUBLISH_INTERVAL_MS` :
   - appelle le **callback de rafraîchissement** de chaque objet
     (`onUpdate`), qui met à jour les valeurs calculées ;
   - imprime les valeurs sur la console ;
   - vérifie périodiquement l'état Wi-Fi.

---

## 3. Le modèle générique (`OpcUaObject`)

Un objet OPC UA est décrit par une structure simple (`include/opcua_object.h`) :

```cpp
typedef struct {
    const char*    name;       // nom de l'objet (nœud racine)
    uint32_t       nodeId;     // 0 = automatique (namespace 1)
    OpcUaProperty* properties; // tableau de variables
    size_t         propertyCount;
    OpcUaUpdateCallback onUpdate; // rafraîchissement périodique (peut être NULL)
} OpcUaObject;
```

Chaque propriété (`OpcUaProperty`) décrit une variable :

```cpp
typedef struct {
    const char*    name;   // nom de navigation / affichage
    uint32_t       nodeId; // 0 = automatique
    OpcUaValueType type;   // type de donnée
    OpcUaAccess    access; // lecture seule ou lecture/écriture
    void*          value;  // pointeur vers la variable réelle
    const char*    folder; // dossier parent optionnel (NULL = à la racine)
    size_t         size;   // taille du buffer, pour les chaînes
} OpcUaProperty;
```

### Types de données supportés

| Type `OpcUaValueType` | Type C attendu | Type OPC UA |
|---|---|---|
| `OPCUA_TYPE_BOOL` | `bool` | Boolean |
| `OPCUA_TYPE_INT32` | `int32_t` | Int32 |
| `OPCUA_TYPE_UINT32` | `uint32_t` | UInt32 |
| `OPCUA_TYPE_FLOAT` | `float` | Float |
| `OPCUA_TYPE_DOUBLE` | `double` | Double |
| `OPCUA_TYPE_STRING` | `char[]` terminé par `\0` | String |

### DataSource : lecture et écriture en direct

Les variables ne stockent **pas** de copie : elles pointent directement sur les
champs de l'image machine `g_plant`. Le serveur utilise un **DataSource** :

- **lecture** : la valeur est relue à chaque requête du client, sans copie
  intermédiaire périmée ;
- **écriture** : si la propriété est en `OPCUA_ACCESS_READWRITE`, la valeur
  envoyée par le client est recopiée dans la variable locale (les chaînes sont
  tronquées à `size - 1`).

### Dossiers

Le champ `folder` crée automatiquement un **nœud dossier** (`FolderType`) sous
l'objet, et y range la variable. Cela permet de retrouver l'arborescence
« constructeur » `ScrewStation/State/iState`, etc.

---

## 4. Arbre d'adressage `ScrewStation`

L'objet racine s'appelle **`ScrewStation`** (`Objects/ScrewStation`). Toutes les
variables sont dans `ns=1` (NodeId numérique automatique). Parcourez-les de
préférence **par leur nom** (*browse path*), pas par un NodeId figé.

| Dossier | Variable | Type | Accès |
|---|---|---|---|
| **Ident** | `sDeviceName` | String | R |
| | `sSerialNo` | String | R |
| | `sFwVersion` | String | R |
| **State** | `iState` | Int32 | R |
| | `bAuto` | Bool | R |
| | `bFault` | Bool | R |
| | `iAlarmCode` | Int32 | R |
| **Cycle** | `sOrderId` | String | **R/W** |
| | `iCurrentLeg` | Int32 | R |
| | `sRfidUid` | String | R |
| | `sRfidData` | String | R |
| | `tCycleTime` | Double | R |
| | `tsLastCycle` | String | R |
| **Position** | `iDetentsLeg1` … `iDetentsLeg4` | Int32 | R |
| | `rAngleLeg1` … `rAngleLeg4` | Double | R |
| | `bLegOk1` … `bLegOk4` | Bool | R |
| | `bPartOk` | Bool | R |
| | `iStepsCmd` | Int32 | R |
| | `iEncoderCnt` | Int32 | R |
| | `rDeltaDeg` | Double | R |
| **Counters** | `nGood` | UInt32 | R |
| | `nBad` | UInt32 | R |
| | `tRun` | UInt32 | R |
| | `tIdle` | UInt32 | R |
| | `tStop` | UInt32 | R |
| | `tFault` | UInt32 | R |
| **Settings** | `rTargetDeg` | Double | **R/W** |
| | `rTolDeg` | Double | **R/W** |
| | `rSlipPct` | Double | **R/W** |
| | `iSpeed` | Int32 | **R/W** |
| | `bSimMode` | Bool | **R/W** |
| **Cmd** | `bStart` | Bool | **R/W** |
| | `bStop` | Bool | **R/W** |
| | `bAckFault` | Bool | **R/W** |
| | `bResetCounters` | Bool | **R/W** |
| **Diag** | `tUptime` | UInt32 | R |
| | `iFreeHeap` | Int32 | R |
| | `iRssi` | Int32 | R |
| | `sIpAddr` | String | R |

Soit **48 variables** réparties en 8 dossiers.

### Sémantique des variables clés

- **`State/iState`** : code de l'état machine (0 = INIT, 1 = ARRÊT, 2 = PRÊT,
  3 = ATTENTE_COMPOSANT, 4 = VISSAGE, 5 = FIN_CYCLE, 6 = SUSPENDU,
  7 = DÉFAUT).
- **`State/iAlarmCode`** : 0 = aucune, 1 = `POSITION_COURTE`, 2 = `POSITION_LONGUE`.
- **`Position/*`** : crans et angles mesurés pour chacun des 4 pieds (l'angle vaut
  `crans × 12°`).
- **`Settings/*`** : réglages modifiables par le client OPC UA **ou** par l'IHM
  locale (cible, tolérance, glissement, vitesse, simulation).
- **`Cmd/*`** : **impulsions**. Le client écrit `true` ; l'automate les consomme
  à la scrutation suivante puis les remet à `false`. C'est le seul point de
  commande OPC UA de la machine.
- **`Counters/*`** : compteurs bruts (pièces bonnes / mauvaises, temps par
  famille d'états). Aucun calcul de TRS n'est fait dans l'ESP.

---

## 5. Se connecter depuis un client

Avec **UaExpert** ou **Ignition** :

1. Créer une connexion vers `opc.tcp://<ip-de-la-carte>:4840/`
   (récupérer l'IP sur la console série ou l'écran `NET` de l'IHM).
2. **Security Policy = None**, **Message Security Mode = None**.
3. **Authentification anonyme**.
4. Si le nom d'hôte ne se résout pas, utiliser directement **l'adresse IP**
   (*Host Override* dans Ignition).
5. Ne **pas** ajouter `/discovery` à l'URL : ce chemin concerne un serveur de
   découverte (LDS) qui n'est pas utilisé ici.

---

## 6. Limites et précautions

- **Aucun chiffrement / aucune authentification** : choix pédagogique. **Ne
  jamais** exposer ce serveur sur un réseau d'entreprise ou sur Internet ;
  réservez-le à un réseau de TP isolé.
- Le **Namespace 0 est minimal** (économie de RAM) : les **événements OPC UA**
  sont désactivés. Les abonnements aux changements de valeur restent, eux,
  disponibles.
- Comptez environ **32 Ko de tas libre par session** OPC UA ; le Wi-Fi est en
  `WIFI_PS_NONE` pour éviter les coupures de session.
- Les NodeId sont attribués automatiquement : un client doit **parcourir
  l'arbre par nom** plutôt que coder des NodeId en dur.

---

[🏠 Accueil](../README.md) · [📖 Sommaire](README.md) · [🏗️ Architecture](architecture.md) · [🔌 Montage](montage.md) · [📶 Wi-Fi](wifi.md)
