# Rapport open62541 pour ESP32

## Objectif

Adapter open62541 pour faire fonctionner un serveur OPC UA minimal sur un
ESP32 utilisant le framework Arduino et la pile reseau FreeRTOS/LWIP.

Le serveur expose actuellement deux variables simulees :

- Temperature : `ns=1;i=1001`
- Humidite : `ns=1;i=1002`

## Version utilisee

La bibliotheque a ete generee depuis open62541 :

- Version : `v1.3.9`
- Commit : `70ff3501ddecd7e7594ebc63e2365994d59e010d`
- Architecture : `freertosLWIP`
- Format : amalgamation, un fichier C et un fichier H

Les fichiers integres dans le projet sont :

```text
lib/open62541lib/open62541.c
lib/open62541lib/open62541.h
```

## Pourquoi une generation personnalisee

La configuration standard d'open62541 contient un modele OPC UA Namespace 0
important et plusieurs fonctions inutiles pour notre application. Sur un ESP32,
cela provoquait l'erreur suivante au demarrage :

```text
Initialization of Namespace 0 failed with BadOutOfMemory
```

L'echec venait de l'initialisation dumodele OPC UA en memoire.

## Options de generation finales

La bibliotheque finale a ete generee avec les options suivantes :

```bash
cmake \
  -DUA_ARCHITECTURE=freertosLWIP \
  -DUA_ENABLE_AMALGAMATION=ON \
  -DUA_ENABLE_DISCOVERY=OFF \
  -DUA_ENABLE_DISCOVERY_MULTICAST=OFF \
  -DUA_ENABLE_SUBSCRIPTIONS=ON \
  -DUA_ENABLE_SUBSCRIPTIONS_EVENTS=OFF \
  -DUA_ENABLE_METHODCALLS=OFF \
  -DUA_ENABLE_HISTORIZING=OFF \
  -DUA_ENABLE_DIAGNOSTICS=OFF \
  -DUA_ENABLE_JSON_ENCODING=OFF \
  -DUA_ENABLE_PARSING=OFF \
  -DUA_ENABLE_NODEMANAGEMENT=ON \
  -DUA_NAMESPACE_ZERO=MINIMAL \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  /chemin/vers/open62541
```

Puis l'amalgamation a ete generee avec :

```bash
make open62541-amalgamation-header open62541-amalgamation-source -j4
```

## Signification des options importantes

### `UA_ARCHITECTURE=freertosLWIP`

Selectionne l'architecture reseau compatible avec FreeRTOS et LWIP, utilisee
par l'ESP32.

### `UA_NAMESPACE_ZERO=MINIMAL`

Utilise une version minimale du modele d'informations OPC UA standard. Cela
reduit fortement la consommation RAM et ROM.

Cette option convient a notre cas simple, mais peut etre insuffisante pour un
projet qui utilise des modeles OPC UA complexes ou des noeuds standard avances.

### `UA_ENABLE_SUBSCRIPTIONS=ON`

Conserve les subscriptions OPC UA. C'est important pour Ignition, qui peut
recevoir les changements de valeur sans effectuer uniquement du polling.

### `UA_ENABLE_SUBSCRIPTIONS_EVENTS=OFF`

Desactive les evenements OPC UA. Les evenements ne sont pas necessaires pour
une temperature et une humidite.

Cette desactivation est necessaire pour utiliser le Namespace 0 minimal.

### `UA_ENABLE_NODEMANAGEMENT=ON`

Conserve la possibilite d'ajouter dynamiquement les noeuds `Temperature` et
`Humidite` dans le programme ESP32.

### Fonctions desactivees

Les fonctions suivantes ont ete desactivees car elles ne sont pas utilisees :

- Discovery LDS
- Discovery multicast
- Appels de methodes
- Historisation
- Diagnostics serveur
- Encodage JSON
- Fonctions de parsing avancees

## Adaptation ESP32 manuelle

L'architecture FreeRTOS d'open62541 inclut normalement :

```c
#include <task.h>
```

Avec Arduino-ESP32, le header est expose dans le namespace de framework
`freertos`. Le fichier genere a donc ete corrige manuellement :

```c
#include <freertos/task.h>
```

Cette modification se trouve dans :

```text
lib/open62541lib/open62541.c
```

Elle est necessaire pour eviter l'erreur de compilation :

```text
fatal error: task.h: No such file or directory
```

## Configuration PlatformIO

Le fichier `platformio.ini` contient les definitions necessaires :

```ini
build_flags =
    -D UA_ARCHITECTURE_FREERTOSLWIP
    -D UA_ENABLE_AMALGAMATION
    -Wno-write-strings
    -Wno-discarded-qualifiers
```

Le flag `UA_IPV6=0` a ete retire. L'amalgamation determine deja la valeur
appropriee depuis la configuration LWIP, et sa presence provoquait un warning
de redefinition.

## Configuration du serveur

Le programme utilise :

- Wi-Fi en mode station, avec connexion a un reseau existant
- Port OPC UA : `4840`
- SecurityPolicy : `None`
- Connexion anonyme activee
- Adresse endpoint basee sur l'IP DHCP de l'ESP32
- Serveur execute sur le core 1
- Pile de la tache OPC UA : `12288` mots
- Buffers reseau open62541 : `8192` octets en emission et reception

L'URL est affichee dans le moniteur serie, par exemple :

```text
opc.tcp://192.168.4.100:4840/
```

Il ne faut pas ajouter `/discovery` a cette URL. Ce chemin concerne un serveur
de decouverte LDS, qui n'est pas utilise ici.

## Securite

La configuration actuelle est volontairement minimale et utilise :

- Pas de chiffrement
- SecurityPolicy `None`
- Authentification anonyme
- Verification de certificat permissive

Cette configuration est adaptee a un reseau de travaux pratiques isole. Elle
ne doit pas etre utilisee telle quelle sur un reseau de production ou expose
sur Internet.

## Reproduction du fork

Pour recreer la bibliotheque :

1. Recuperer open62541 `v1.3.9`.
2. Appliquer les options CMake indiquees dans ce rapport.
3. Generer `open62541.c` et `open62541.h`.
4. Remplacer `#include <task.h>` par `#include <freertos/task.h>` dans le C genere.
5. Copier les deux fichiers dans `lib/open62541lib/`.
6. Executer un clean PlatformIO avant compilation.

```bash
pio run -t clean
pio run
```

## Limites connues

- La version finale doit etre validee sur la carte ESP32 cible apres compilation.
- Le Namespace 0 minimal peut ne pas contenir tous les noeuds standard attendus
  par un client OPC UA complexe.
- Les evenements OPC UA ne sont pas disponibles.
- Les communications ne sont pas chiffrees.
- Les valeurs sont actuellement simulees ; le remplacement par le DHT-22 devra
  etre fait dans `genererValeursSimulees()`.

## Licence

open62541 est distribue sous licence MPL-2.0. Le fork du groupe doit conserver
la licence et les mentions legales du projet open62541. Le code d'integration
ESP32 peut etre documente et distribue separement selon la licence choisie par
le groupe.
