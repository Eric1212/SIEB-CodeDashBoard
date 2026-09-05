<p align="center">
  <picture>
    <source srcset="resources/svg/banner-light.svg" media="(prefers-color-scheme: dark)">
    <img src="resources/svg/banner-dark.svg" alt="CodeDashBoard (CDB)" width="600">
  </picture>
</p>

<p align="center"><strong>L'IDE léger, orienté IA.</strong></p>

<p align="center">
  <a href="LICENSE"><img alt="Licence" src="https://img.shields.io/badge/license-custom_(SIEB)-2f6f9f?style=flat-square" /></a>
  <img alt="C" src="https://img.shields.io/badge/C-23-0175c2?style=flat-square" />
  <a href="https://gtk.org"><img alt="GTK" src="https://img.shields.io/badge/GTK-4-7053a5?style=flat-square" /></a>
  <img alt="RAM" src="https://img.shields.io/badge/RAM-%E2%89%A4_128_MiO-3aad3a?style=flat-square" />
</p>

<p align="center">
  <a href="README.md">English</a> | Français | <a href="po/README.md">Ajoutez votre langue</a>
</p>

<p align="center">
  <img src="resources/images/cdb_hero_shadow.webp" alt="CodeDashBoard (CDB) — l'IDE léger, orienté IA" width="830">
</p>

> **Pas d'Electron. Pas de bloat.** Un IDE GTK4 en C23 qui vise les 128 Mio
> de RAM — et un fil de conversation qui rend 100 000 lignes en 0,0 ms.
> Mesuré, pas espéré.

---

### Visite guidée

**La boîte interactive, en trois états.** Une demande se plie, la décision
s'affiche toujours en entier — vert ou rouge — et la réponse arrive
complète, blanc sur noir comme un terminal. Le fil est un GtkTextView
paresseux : 100 000 lignes se rendent en 0,0 ms, là où une GtkLabel exige
62 secondes. Rien n'est jamais tronqué.

<p align="center">
  <img src="resources/images/cdb_ibox_close_shadow.webp" alt="ibox, pliée" width="320">
  <img src="resources/images/cdb_ibox_open_shadow.webp" alt="ibox, ouverte" width="320">
  <img src="resources/images/cdb_ibox_mixed_shadow.webp" alt="ibox, mixte" width="320">
</p>

**Votre fournisseur, avec ou sans clé.** OpenRouter, OpenCode Zen,
HyperCharm et KiloGateway sont pré-câblés — en ajouter un tient sur une
ligne. Le formulaire vous accueille dans les deux cas.

<p align="center">
  <img src="resources/images/cdb_llm_provider_key_shadow.webp" alt="fournisseur, avec clé" width="400">
  <img src="resources/images/cdb_llm_provider_keyless_shadow.webp" alt="fournisseur, sans clé" width="400">
</p>

**Deux menus, zéro chromure.** Les préférences d'outils se tiennent à deux
clics du fil — les modes compris, la plomberie exclue.

<p align="center">
  <img src="resources/images/cdb_menu_shadow.webp" alt="le menu" width="420">
  <img src="resources/images/cdb_menu_tools_shadow.webp" alt="le menu outils" width="420">
</p>

### Installation rapide

```sh
curl -fsSL https://raw.githubusercontent.com/Eric1212/SIEB-CodeDashBoard/master/install.sh -o install.sh
bash install.sh
```

> [!WARNING]
> L'avertissement habituel du « pipe vers shell » s'applique : le script se
> lit — **lisez-le avant de l'exécuter**. Il n'utilise jamais `sudo` et
> n'installe jamais rien : s'il manque des dépendances, il affiche la
> commande de votre gestionnaire de paquets et s'arrête. Vous exécutez
> cette commande vous-même, puis relancez le script (ou faites `make`).

### Installation

```sh
git clone https://github.com/Eric1212/SIEB-CodeDashBoard.git
cd SIEB-CodeDashBoard

make run   # construit puis lance — la voie express
# ou en deux temps :
make       # construit ./cdb et les catalogues .mo
./cdb      # lance le binaire depuis l'arbre de build

# ou, installez à l'échelle système — entrée de bureau + icônes (exige rsvg-convert) :
sudo make install     # puis démarrez CDB depuis le menu d'applications ; retrait : sudo make uninstall
```

#### Non-Linux (Windows & macOS)

##### Windows (WSL2)

**WSL2** — CDB est une appli GTK4 ; lancez-la comme n'importe quelle appli
GUI sous WSLg, la couche Wayland/X11 incluse dans WSL2 (in-box sur
Windows 11, ou WSL du Microsoft Store sur Windows 10). Première fois avec
WSL ? Depuis un PowerShell **administrateur** :

```powershell
wsl --install            # WSL2 + Ubuntu (par défaut) ; redémarrez, puis
                         # choisissez un identifiant/mot de passe dans la distro
wsl --install -d Debian  # n'importe quelle distro — wsl --list --online pour voir
```

Debian sur le Store : <https://apps.microsoft.com/detail/9msvkqc78pk6> (éditeur : The Debian Project).

À partir de ce shell Debian, traitez CDB exactement comme sur une
installation Debian ou Ubuntu native — `install.sh` compris,
`sudo make install` aussi (WSLg place même l'entrée de bureau dans le menu
Démarrer de Windows).

##### macOS (Homebrew)

**macOS (Homebrew)** — compilez depuis les sources contre la pile GTK4 de
Homebrew :

```sh
brew install gcc pkgconf gettext make gtk4 gtksourceview5 libadwaita \
  json-glib vte3 libsoup librsvg
make run   # construit puis lance — la voie express
# ou en deux temps :
make       # pkg-config trouve les kegs via PKG_CONFIG_PATH
./cdb      # XQuartz inutile : GTK4 s'affiche nativement sur macOS
```

GTK4 s'affiche nativement sur macOS — XQuartz inutile. Arrêtez-vous là :
`sudo make install` est une affaire XDG/Linux — /usr est protégé par SIP, et
les entrées .desktop n'y signifient rien.

#### Dépendances

Une chaîne C23 (gcc 15 conseillé) et la pile GTK4. Les noms pkg-config
que CDB cherche : `gtk4`, `gtksourceview-5`, `libadwaita-1`,
`json-glib-1.0`, `vte-2.91-gtk4`, `libsoup-3.0`.

```sh
# Debian / Ubuntu
sudo apt install build-essential gcc-15 pkg-config gettext git \
  libgtk-4-dev libgtksourceview-5-dev libadwaita-1-dev \
  libjson-glib-dev libsoup-3.0-dev libvte-2.91-gtk4-dev librsvg2-bin

# Fedora
sudo dnf install gcc make pkgconf gettext git gtk4-devel gtksourceview5-devel \
  libadwaita-devel json-glib-devel vte291-gtk4 libsoup3-devel librsvg2-tools

# Arch
sudo pacman -S --needed base-devel gtk4 gtksourceview5 libadwaita \
  json-glib libsoup3 vte4 librsvg
```

#### Make

| Cible | Rôle |
|---|---|
| `make` | construit `./cdb` et les outils |
| `make run` | construit, puis lance |
| `make install` / `make uninstall` | entrée de bureau + icônes, ou leur retrait |
| `make asan` | build AddressSanitizer + UBSan |
| `make check` | suite de tests |
| `make pot` / `po` / `mo` / `i18n-check` | chaîne de traduction : régénérer, fusionner, compiler, vérifier |
| `make clean` | balaye l'arbre de build |

### Debug

```sh
CDB_DEBUG=1 ./cdb   # dump des allocations + thème au démarrage
```

### Documentation

Les cibles de build vivent dans le [Makefile](Makefile) ; le flux de traduction,
dans [po/README.md](po/README.md). Les sources sous [src/](src) restent la référence finale.

### Contribuer

Un bug à signaler ou une fonctionnalité à réclamer ? [Ouvrez un ticket](https://github.com/Eric1212/SIEB-CodeDashBoard/issues) — les deux sont bienvenus.

La traduction est l'autre porte ouverte : voir [po/README.md](po/README.md) —
ajoutez votre langue à `LINGUAS`, `make po`, traduisez, `make mo` ; le sélecteur
de langue lit le disque, rien à coder. Pour le reste, CDB est sous une licence
SIEB personnalisée : lisez [LICENSE](LICENSE) d'abord — elle pose des termes précis aux œuvres dérivées et à l'usage commercial.

---

**CodeDashBoard (CDB)** par SIEB · <eb@sieb.ca> · [github.com/Eric1212/SIEB-CodeDashBoard](https://github.com/Eric1212/SIEB-CodeDashBoard)
