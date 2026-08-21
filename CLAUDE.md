# CodeDashBoard (CDB)

IDE léger sous forme de Dashboard, orienté LLM.

Nomenclature : **SIEB** = entreprise, **CodeDashBoard** = nom long,
**CDB** = sigle (binaire, logs, app ID `org.sieb.cdb`).

## Vision

Un environnement de développement qui se présente comme un tableau de bord :
code, recherche et IA cohabitent dans une seule fenêtre GTK, sans lourdeur.
L'objectif est plus proche de **gnome-text-editor** (léger, GTK4, GtkSourceView)
que de gedit (GTK3, fork libgedit).

## Stack

| Composant | Rôle | État |
|---|---|---|
| **C23** | Langage (`-std=c23`, gcc 15) | ✅ |
| **GTK4** (4.22) | UI | ✅ |
| **GtkSourceView 5** (5.20) | Édition de code : coloration syntaxique, numéros de ligne, indentation auto | ✅ |
| **libadwaita** (1.9) | Thème clair/sombre système : `AdwStyleManager` suit `color-scheme` via le portal (comme `RClotGenerator`) | ✅ |
| **json-glib** (1.10) | Persistance JSON des roots (`~/.config/cdb/roots.json`) | ✅ |
| **NetSurf** | Rendu HTML intégré (le Dashboard affiche des pages web) | ⏳ jalon futur |
| **ffsr** | Recherche de code que NetSurf ne couvre pas (projet `~/dev/ffsr`) | ⏳ jalon futur |
| **alvalllm** | IA en intégration douce — pas d'ACP (projet `~/dev/alvalllm`) | ⏳ jalon futur |

Choix de GtkSourceView 5 (et pas libgedit) : série GTK4 officielle GNOME, toujours
maintenue (5.21+ en 2026), utilisée par gnome-text-editor. Le fork libgedit est
GTK3, incompatible avec notre stack.

## Architecture

```
~/dev/SIEB-CodeDashBoard/
├── CLAUDE.md       # ce fichier
├── Makefile        # make / make run / make clean
└── src/
    ├── main.c      # UI GTK4 : HeaderBar, panneau Dossiers (arbre), GtkSourceView, barre de statut
    ├── roots.c     # modèle des roots : add/remove + persistance JSON (json-glib)
    └── roots.h
```

- `main.c` : une seule fenêtre 1280×800, `GtkHeaderBar` (bouton Ouvrir →
  `GtkFileDialog`), `GtkPaned` horizontal (gauche = panneau Dossiers,
  droite = éditeur), barre de statut (fichier courant + ligne:colonne).
- Panneau **Dossiers** : `GtkListView` + `GtkTreeListModel` (2 niveaux).
  Un **root de structure** (ex: `/home/eric/dev`) contient des **roots de
  projet** (ex: `/home/eric/dev/alvalllm`) ; un projet peut aussi être orphelin
  à la racine. Menu « + » → `GtkFileDialog` dossier ; clic droit → suppression.
  Placement : si une structure est sélectionnée à l'ajout d'un projet, le
  projet devient son enfant.
- `roots.c` : `RootEntry` = GObject (kind : structure/projet, children :
  `GListStore` pour les structures). `roots_load()` au démarrage, `roots_save()`
  après chaque ajout/suppression. JSON : `{"roots":[{path,kind,children?}]}`,
  joli format, échappé proprement par json-glib.
- Détection de langue via `gtk_source_language_manager_guess_language()`.
- Schéma de couleurs Adwaita.

## Conventions

- C23, `snake_case`, callbacks GTK de type `on_<signal>_<widget>`.
- Structure de contexte `App` passée en `user_data` — pas de globales.
- Compilation stricte : `-Wall -Wextra` (pas de `-Wpedantic` : bruit des headers GTK).
- Une fonction = une responsabilité ; le code reste lisible sans sur-architecture.
- json-glib : toujours tester `json_object_has_member()` avant
  `json_object_get_*_member()` (assertion sinon).

## Jalons

- [x] **0 — Premier jet UI** : fenêtre GTK4 + GtkSourceView opérationnelle,
      ouverture de fichiers, statut curseur, thème système (libadwaita).
- [x] **0b — Dossiers** : roots de structure / de projet, ajout/suppression,
      persistance JSON.
- [ ] **1 — NetSurf** : widget d'affichage HTML (prévisualisation, docs, dashboard).
- [ ] **2 — ffsr** : intégration de la recherche pour ce que NetSurf ne permet pas.
- [ ] **3 — alvalllm** : intégration douce de l'IA pour compléter le dashboard.

## Commandes

```sh
make        # compilation (binaire : ./cdb)
make run    # compile puis lance
make clean
```

## Debug

```sh
CDB_DEBUG=1 ./cdb   # dump des allocations + schéma de thème
```