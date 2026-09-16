# 🖥️ IHM locale — écrans LCD

[🏠 Accueil du projet](../README.md) · [📖 Documentation](README.md)

L'ESP32 pilote un écran **LCD 1602 I2C** (2 lignes de 16 caractères) connecté à
l'encodeur n°2. L'IHM est une simple **vue/éditeur** de l'image machine
(`g_plant`) : elle affiche l'état, les compteurs, le réseau, les réglages et le
dernier cycle, et permet de modifier les réglages machine.

---

## Navigation générale

| Action | Effet |
|---|---|
| **Tourner l'encodeur n°2** | Passe à l'écran suivant / précédent (boucle sur 6 écrans) |
| **Appuyer sur le bouton de l'encodeur** | *Écran SET* : entre dans le mode réglages  <br>*Écran BAD* : bascule UID ↔ contenu du badge |

> Si l'écran n'est pas branché au boot, l'ESP32 retente la détection toutes les
> 5 s. L'automate et le serveur OPC UA continuent de fonctionner sans écran.

---

## Les 6 écrans

Chaque écran partage la **ligne du haut** :

```text
READY         2/4 SET
```

- À gauche : l'état machine (`INIT`, `STOP`, `READY`, `WAIT`, `SCREW`,
  `END CYC`, `SUSPND`, `FAULT`).
- Au centre : le pied courant `1/4` … `4/4` (uniquement en mode `SCREW`).
- À droite : le tag de l'écran (`STA`, `PRO`, `NET`, `SET`, `CYC`, `BAD`).

La **ligne du bas** dépend de l'écran actif.

---

### 1. `STA` — État machine

Ligne du bas :

- En **FAULT** : `ALARM LONG` ou `ALARM SHORT`
- Sinon : numéro d'ordre en cours (`sOrderId`), ex. `OF-2026-0001`

| Pour y aller | Depuis n'importe quel écran, tourner l'encodeur jusqu'à `STA` |

---

### 2. `PRO` — Production (compteurs bruts)

Ligne du bas :

```text
OK   123 NOK  45
```

Affiche les compteurs **OK** et **NOK** bruts. Conformément à la doctrine du
kit, **aucun TRS/OEE** n'est calculé dans l'ESP32 : c'est le rôle du SCADA.

| Pour y aller | Tourner l'encodeur jusqu'à `PRO` |

---

### 3. `NET` — Réseau

Ligne du bas alterne toutes les 2 secondes entre :

1. **SSID** du point d'accès (`HUB-WIFI-MPM` ou `?` si déconnecté)
2. **Adresse IP** de l'ESP32 (`192.168.4.100`)
3. **Port OPC UA** (`OPC UA p4840`)

| Pour y aller | Tourner l'encodeur jusqu'à `NET` |

---

### 4. `SET` — Réglages machine

Ligne du bas au repos :

```text
Press = settings
```

Appuyer sur le bouton pour entrer dans le **mode édition**. Les 4 paramètres
sont alors parcourus un par un :

| # | Paramètre | Signification | Pas | Min | Max | Unité |
|---|---|---|---|---|---|---|
| 1 | `Target` | Angle cible d'indexage | 1,0° | 45° | 180° | ° |
| 2 | `Tol` | Tolérance autour de la cible | 0,5° | 1° | 45° | ° |
| 3 | `Slip` | Probabilité de glissement friction | 1 % | 0 % | 100 % | % |
| 4 | `Speed` | Vitesse moteur | 25 pas/s | 100 | 900 | pas/s |

En mode édition :

| Action | Effet |
|---|---|
| Tourner l'encodeur | Augmente / diminue le paramètre actif (avec limites) |
| Appuyer sur le bouton | Passe au paramètre suivant |
| Après le 4e paramètre | Retour à l'écran `SET` en mode navigation |

La valeur est appliquée **en direct** dans `g_plant` et est donc visible
immédiatement par l'automate et OPC UA.

| Pour y aller | Tourner jusqu'à `SET`, puis appuyer pour éditer |

---

### 5. `CYC` — Dernier cycle

Ligne du bas :

```text
 84  96  92  88+
```

Affiche les **angles d'indexage des 4 pieds** en degrés, suivis d'un indicateur
:

- `+` : pièce conforme (`bPartOk = true`)
- `-` : pièce non conforme

| Pour y aller | Tourner l'encodeur jusqu'à `CYC` |

---

### 6. `BAD` — Badge RFID

Ligne du bas :

- Aucun badge lu : `no badge read`
- Par défaut : **UID** du badge (`SIM-...` en mode simulation)
- Après appui sur le bouton : **contenu** du badge (`sRfidData`)

| Pour y aller | Tourner jusqu'à `BAD` |
| Bascule UID ↔ contenu | Appuyer sur le bouton |

---

## Cheminements typiques

### Vérifier l'IP et le port OPC UA

1. Tourner l'encodeur jusqu'à l'écran `NET`.
2. Attendre 2 s : l'IP apparaît.
3. Attendre encore 2 s : le port OPC UA apparaît.

### Modifier la tolérance

1. Tourner jusqu'à `SET`.
2. Appuyer : l'écran affiche `Target ... *`.
3. Appuyer à nouveau : passe à `Tol   ... *`.
4. Tourner pour ajuster la tolérance.
5. Appuyer pour valider et passer au paramètre suivant (ou revenir à `SET`).

### Consulter le dernier cycle

1. Tourner jusqu'à `CYC`.
2. Lire les 4 angles et le verdict `+`/`-`.

### Voir le contenu d'un badge

1. Tourner jusqu'à `BAD`.
2. L'UID s'affiche.
3. Appuyer pour voir le contenu éventuel du badge.
4. Réappuyer pour revenir à l'UID.

---

## Comportements particuliers

- L'écran reste **fonctionnel sans badge RFID** : un badge simulé est généré
  automatiquement (`SIM-...`) quand aucun lecteur RC522 n'est détecté.
- Les valeurs affichées sont des **copies** de l'image machine lues sous
  verrou : l'automate n'est jamais bloqué par l'IHM.
- En cas de débranchement de l'écran, la tâche HMI repasse en détection toutes
  les 5 s.

---

[🏠 Accueil](../README.md) · [📖 Sommaire docs](README.md) · [🔌 Montage](montage.md) · [🔗 OPC UA](opcua.md)
