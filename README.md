# HomeCloud Shell

> Version 1.1.0 : les commandes d’administration (`homecloud server`, `listdisk`, `listuser`, `adduser`, `removeuser`, `permission`, `adddisk`, `removedisk`, `linkdisk`) fonctionnent via l’API `/api/shell/*` de HomeCloud Server.
> Version 1.0.1 : correction du délai d’affichage des caractères saisis dans le terminal distant.

Un « SSH simulé » dans votre espace personnel HomeCloud : on se connecte avec son compte HomeCloud, on obtient un vrai terminal
où l'on exécute de vrais programmes, **isolés dans un bac à sable** qui ne voit que son espace personnel.

- `homecloud-shelld` : le serveur (à installer sur la machine de HomeCloud, **après HomeCloud Web**).
- `homecloud-shell` : le client en ligne de commande, stylisé (à installer sur n'importe quel ordinateur Linux/macOS/Termux).

La connexion passe par **HomeCloud Web** (même port, `GET /shell`) : un seul port à ouvrir. HomeCloud Web ≥ 1.1.0 est requis.

## Installation du serveur

```sh
sudo sh install.sh        # édition complète de HomeCloud
sh install.sh             # Termux / édition lite
```

L'assistant détecte HomeCloud et HomeCloud Web, teste le bac à sable, puis pose deux questions : autoriser le réseau dans
le bac à sable, et démarrer automatiquement avec HomeCloud. Options : `--yes --network|--no-network --autostart|--no-autostart --insecure`.

Commandes : `homecloud-shelld install | start | stop | restart | status | uninstall`.

### Termux (sans root)

Sans root ni `bubblewrap`, l'installeur propose d'installer **proot** (`pkg install proot`) et l'utilise comme bac à sable : le shell ne voit que
le système Termux (`$PREFIX`) et l'espace personnel (`/home/<utilisateur>`), pas `~/.homecloud`. Si le test échoue : `export PROOT_NO_SECCOMP=1`
puis relancer. proot n'isole ni les processus ni le réseau (pas de `--no-network` effectif).
Dernier recours : `homecloud-shelld install --insecure` = **aucune isolation** (le shell lit toutes les données de HomeCloud), pour un compte unique de confiance.

## Utilisation

```sh
homecloud-shell alice@192.168.1.20:8680            # session interactive
homecloud-shell alice@192.168.1.20:8680 -- ls -la  # une commande, code de sortie conservé
```

Mot de passe demandé sans écho (ou variable `HOMECLOUD_PASSWORD` pour les scripts). Entrée puis `~.` ferme la session.
Le dossier courant est `/home/<utilisateur>` = **votre espace personnel HomeCloud** : ce que vous y écrivez apparaît dans le client
HomeCloud, et inversement.

## Ouvrir un port

Lancez un service dans le shell (par exemple `python3 -m http.server 3000`). Il est alors public à l'adresse :

```
http://<ip>:<port HomeCloud Web>/port/<utilisateur>/<port>/<route sur le serveur>
```

Dès qu'un programme du shell se met à écouter sur un port (HTTP ou autre), le client affiche une ligne encadrée avec le lien :

```
┌───────────────────────────────────────────────────────────────┐
│ Port 3000 ouvert : http://192.168.1.78:8680/port/nheard/3000/ │
└───────────────────────────────────────────────────────────────┘
```

Le cadre attend que le terminal soit calme et ne s'affiche jamais par-dessus un programme plein écran (vim, htop...).
Seuls les ports ouverts sur `0.0.0.0` / `127.0.0.1` sont annoncés (c'est là que HomeCloud Web se connecte). Le lien est
relayé en HTTP : pour un service non HTTP, il ne répondra pas dans un navigateur.

Sans connexion nécessaire pour les visiteurs. HomeCloud Web ne relaie un port que s'il est ouvert par un programme de la
session de cet utilisateur : impossible d'atteindre SSH, HomeCloud ou une base de données par ce biais.

## Ce que le shell dit de la machine

Dans le bac à sable, les commandes qui décrivent le système répondent avec l'identité de HomeCloud Shell, jamais celle du serveur :
`uname` (nom du noyau `Linux` et architecture réels conservés pour ne pas casser les compilations ; version du noyau, version
et système remplacés), `hostname`, `hostnamectl`, `whoami`, `logname`, `lsb_release`, ainsi que `/etc/os-release`,
`/usr/lib/os-release`, `/etc/lsb-release`, `/etc/issue`, `/proc/version` et `/proc/cmdline`.

```
$ uname -a
Linux homecloud 6.1.0-homecloud #1 HomeCloud Shell 1.0.0 x86_64 HomeCloud
```

Les textes sont dans `src/identity.c` (une chaîne par commande ou fichier). Les commandes sont de petits scripts `sh` placés en tête
du `PATH` (`/run/hc/bin`) : ce n'est pas une barrière de sécurité, `/usr/bin/uname` appelé directement donne toujours la vraie
valeur. Le matériel (`/proc/cpuinfo`, `/proc/meminfo`), les adresses réseau et l'uptime ne sont pas masqués. Valable pour les
bacs à sable `ns` et `bwrap` (pas `proot` ni `simple`).

## Sécurité

- Authentification par HomeCloud lui-même (le mot de passe n'est pas stocké). 5 échecs depuis une adresse = blocage 60 s.
- Bac à sable (espaces de noms Linux, pivot_root) : système de fichiers racine éphémère, `/usr` et `/bin` en lecture seule,
  `/etc` générée, aucun accès à la configuration ni aux données de HomeCloud ni aux autres utilisateurs, toutes capacités retirées,
  `no_new_privs`, limites mémoire/processus (cgroup v2 quand disponible). Alternative : `bubblewrap` si installé et pas root.
- Les pages servies par `/port/` sont sur la même origine que HomeCloud Web : elles reçoivent une CSP `sandbox` (pas d'accès
  aux cookies ni à l'API) et leurs `Set-Cookie` sont supprimés.
- Pas de HTTPS : réseau de confiance, VPN ou reverse proxy.

## Limites connues

- **Les processus vivent le temps de la session** : à la déconnexion, tout le bac à sable est détruit, y compris un serveur lancé
  en arrière-plan. Gardez une session ouverte tant que le port doit répondre (pas de tmux persistant).
- Pas de WebSocket ni de corps de requête « chunked » sur `/port/`.
- Pas de scp/sftp ni de redirection de ports : l'espace personnel se synchronise via HomeCloud.
- Le bac à sable intégré demande root ; sans root : `bubblewrap` ou `proot` (Termux), sinon l'installation refuse (sauf `--insecure`).
- Édition complète : le volume doit être monté (HomeCloud déverrouillé) pour que l'espace existe.
- Sans réseau dans le bac, aucun port ne peut être ouvert.
- Testés ici : édition lite, backend intégré, sans systemd. **Non testés** : unité systemd, `bubblewrap`, `proot` réel (arguments vérifiés seulement), Termux, édition complète.


## Commandes intégrées au shell

Le shell fournit la commande `homecloud` (elle n'est disponible que dans le shell isolé) :

- `homecloud` ou `homecloud info` : affiche la version, le copyright et le site officiel.
- `homecloud help` : affiche l'aide.
- `homecloud usedisk <diskname>` : demande un compte administrateur, vérifie les droits auprès de HomeCloud Server, monte le disque chiffré si nécessaire et ouvre un shell isolé dans ce disque. `exit` revient au shell précédent.
- Commandes d'administration (chacune exige la catégorie ACL correspondante, accordée par un administrateur dans HomeCloud Web) :

| Commande | Catégorie ACL | Effet |
|---|---|---|
| `homecloud server` | ServerInfo | nom, version, port, nombre d'utilisateurs et de disques, stockage |
| `homecloud listdisk` | DiskList | liste des disques (taille, propriétaire, type) |
| `homecloud listuser` | UserManage | liste des utilisateurs |
| `homecloud adduser <nom> [motdepasse] [quota_go]` | UserManage | crée un compte non administrateur (mot de passe demandé sans écho s'il est omis) |
| `homecloud removeuser <nom>` | UserManage | supprime un compte (jamais un administrateur, ni le compte connecté) |
| `homecloud permission <nom> [categorie [on\|off]]` | PermissionManage | affiche ou modifie les catégories ACL d'un compte |
| `homecloud adddisk <nom> <taille_go>` | DiskManage | crée un disque partagé chiffré |
| `homecloud removedisk <nom>` | DiskManage | supprime un disque (pas un espace personnel) |
| `homecloud linkdisk <disque> <utilisateur> <Read\|Write\|None>` | DiskLink | règle l'accès d'un utilisateur à un disque |

  Le compte agissant est celui de la session Shell ; le démon se présente à HomeCloud Server (boucle locale) avec la clé `/etc/homecloud/shell-bridge.key`, créée par `homecloud-shelld install`, et c'est le serveur qui vérifie l'ACL. Ces commandes sont refusées dans un shell ouvert par `usedisk`.
- La touche `Tab` complète les sous-commandes, les catégories ACL, `on`/`off` et `Read`/`Write`/`None`.

Le processus isolé ne reçoit ni accès au répertoire global des disques ni privilèges administrateur. Les identifiants sont transmis au démon par un marqueur privé retiré du flux terminal, puis vérifiés via l'API locale HomeCloud. Le serveur ne renvoie au démon que l'identifiant opaque du disque ; aucun chemin interne ni aucune clé LUKS ne sont exposés.
