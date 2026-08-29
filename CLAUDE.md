# CodeDashBoard (CDB)

IDE léger sous forme de Dashboard, orienté LLM.

Nomenclature : **SIEB** = entreprise, **CodeDashBoard** = nom long,
**CDB** = sigle (binaire, logs, app ID `org.sieb.cdb`).

## Vision

Un environnement de développement qui se présente comme un tableau de bord :
code, recherche et IA cohabitent dans une seule fenêtre GTK, sans lourdeur.
Objectif proche de **gnome-text-editor** (léger, GTK4, GtkSourceView),
pas de gedit (GTK3, fork libgedit).

## Stack

| Composant | Rôle | État |
|---|---|---|
| **C23** | Langage (`-std=c23`, gcc 15) | ✅ |
| **GTK4** (4.22) | UI | ✅ |
| **GtkSourceView 5** | Édition de code | ✅ |
| **libadwaita** | Thème clair/sombre système | ✅ |
| **json-glib** | Persistance (roots, llm.json, llm_live.json, slots) | ✅ |
| **libsoup-3** | flux SSE du chat LLM | ✅ |
| **VTE** | terminaux bash + boucle agentique (tool_calls) | ✅ |
| **NetSurf** | rendu HTML intégré | ⏳ jalon futur |
| **ffsr** | recherche | ⏳ jalon futur |
| **alvalllm** | intégration douce de l'IA | ⏳ jalon futur |

## Sessions

Chaque processus CDB = une session 000-999 (`CDB_SESSION`, ou dialogue
si une autre instance tourne). Config dans `~/.config/cdb/<NNN>/` :
`layout.json` (arbre des tuiles, langue, état de la fenêtre), `dirty.json`,
`llm.json` (fournisseurs et clés API, actif, harness, outils, **et** l'état de
l'explorateur : `roots`, `last_file`), `llm_live.json`, `llm_slots/`,
`prompts/`. Il n'y a plus de `window.json` ni de `roots.json` : les deux ont
été absorbés au Jalon G, chacun sous un membre de son fichier d'accueil.

## Architecture LLM

- `src/llmcore.c` : **LlmCore** — état conversationnel + réseau +
  agentique, vit sans vue. `core_history_push`, stream SSE, décision
  d'outil (tool_calls) au core, retries 429/5xx, `core_cdb_announce`.
- **Bouton média = état de la BOUCLE agentique, pas d'une requête.** Un
  seul prédicat, `core_agent_loop_alive()` : vivant = icône **pause** (un
  clic annule tout par `llm_cancel_current`, décision ASK en attente
  comprise) **et** chrono du tour qui tourne ; mort = **play** + horloge
  arrêtée. Loi d'Éric : le compteur couvre l'échange entier, attente de
  décision comprise, et non la seule requête réseau. Deux points de
  peinture, un par événement — la mort de la dernière requête
  (`llm_request_free`) et l'avancée de la file (`llm_cdb_next` →
  `core_sync_buttons`) — plus jamais une branche qui « oublie » de le
  dire.
- `src/llmtile.c` : **LlmTile** — la vue. Buffers GtkTextBuffer par
  vue, replay de l'historique à l'attach (miroir), sélecteur de modèle,
  slots, barre d'approbation. Aucune propriété conversationnelle.
- `src/llmlive.c` : persistance **dirty** `llm_live.json` (historique
  complet, survie crash/redémarrage). **Loi : live = dirty ; les slots
  sont la seule sauvegarde réelle.**
- Miroir : plusieurs tuiles sur un même core ; toute opération de rendu
  boucle sur `core->views` ; plus aucune vue primaire privilégiée.
- Load du live APRÈS `session_init()` (le numéro de session fixe le
  dossier) — appelé dans `on_activate`, pas dans `llm_core_new`.

## Conventions

- C23, `snake_case`, callbacks GTK `on_<signal>_<widget>`.
- Contexte `App` en `user_data` — pas de globales (sauf `cdb_session`,
  initialisé une fois avant tout usage).
- `-Wall -Wextra`.
- Éditions par numéros de ligne (get #→#, push #→#), pas de replace
  global par regex ; chk de garde avant écriture.
- json-glib : `json_object_has_member()` avant tout get.

## Internationalisation

- **Pivot anglais** : le `msgid` est en anglais, `po/fr.po` restitue le
  français parlé. Langue source déclarée dans `po/README.md`.
- Marquer `_()` tout ce qui est **vu par un humain** : labels, dialogues,
  annonces CDB, journaux `CDB_DEBUG`, et les prompts / schémas d'outils
  envoyés au modèle — décision assumée : la langue de l'agent suit celle de
  l'écran.
- **Ne pas marquer** : clés JSON, identifiants d'action (`win.new-window`),
  CSS, noms d'icônes, jetons comparés en code, commandes shell, et toute
  chaîne sans mot à traduire. Attention aux faux amis : `MINIMAL`/`DEFAULT`/
  `YOLO` s'affichent mais sont aussi des **clés** de `llm.json` — les traduire
  changerait la valeur relue au démarrage.
- **Un script ne décide pas** ce qui se traduit. La qualification se fait à la
  lecture ; un scan peut situer (numéro de ligne, compte de msgids) et rien de
  plus. Trois classificateurs successifs se sont contredits (34, 7, 67) : la
  méthode a été abandonnée au profit de la lecture (§5 du plan).
- Pluriels : `ngettext(singulier, pluriel, n)`, jamais de `? "s" : ""`.
- Tables statiques : `N_()` à la définition, `_()` à l'usage. Une
  initialisation `static const` refuse un appel de fonction, et `xgettext` ne
  prétraite pas : sans `N_()`, la chaîne n'entre pas au catalogue **sans la
  moindre erreur**.
- Unités de mesure : **hors gettext**. Un symbole est une norme, pas une
  phrase ; `1024` ⇒ multiple binaire ⇒ `Kio`/`Mio` (jamais `Ko`/`KB`, qui
  valent 1000). Le séparateur décimal vient de `LC_NUMERIC` (`setlocale` dans
  `i18n_init`), pas d'une chaîne à traduire.
- Noms de langues du sélecteur : **endonymes** (« English », « Français »),
  volontairement non traduits — un utilisateur qui ne lit pas la langue
  courante de l'interface doit retrouver le mot qu'il connaît.
- La langue se choisit dans **Réglages → Général** et vit dans le membre
  `"language"` de `layout.json`. Elle prend effet aussitôt pour tout ce qui est
  créé après (nouvelles tuiles, dialogues, et le prompt reconstruit à chaque
  requête) ; les écrans déjà montés gardent leurs étiquettes.

### Ajouter une langue

1. Ajouter le code à `po/LINGUAS`, puis `make po` (initialise le `.po`).
2. Traduire `po/<lang>.po` — Poedit ou éditeur.
3. `make mo`. Le sélecteur propose toute langue dont le catalogue existe à
   côté du binaire : **rien à coder**, la liste est lue sur disque.
4. `make i18n-check` refuse la syntaxe invalide **et** les entrées `fuzzy` :
   `msgmerge` marie des msgid qui se ressemblent et a déjà produit des
   traductions inversées, que `msgfmt --check` accepte sans un mot.
5. Il faut aussi que la **locale système** existe (`locale -a | grep <code>`) :
   sinon CDB le dit à l'écran et garde la langue précédente, au lieu de faire
   semblant de traduire.

## Jalons

- [x] **0/0b** — UI GTK4 + panneaux dossiers.
- [x] **C0→C6** — refactor LLM : split llmcore/llmtile, LlmCore
  singleton, agentique au core, miroir multi-vues, replay à l'attach,
  persistance live, modèle partagé, nettoyage + doc.
- [ ] **1** — NetSurf ; **2** — ffsr ; **3** — alvalllm.

## Commandes

```sh
make        # compilation (binaire : ./cdb)
make run    # compile puis lance
make asan   # build AddressSanitizer + UBSan
make clean

make pot          # régénère po/cdb.pot depuis les sources marquées
make po           # msgmerge des .po (--no-fuzzy-matching, voir plus bas)
make mo           # compile po/locale/<lang>/LC_MESSAGES/cdb.mo
make i18n-check   # syntaxe valide ET zéro entrée fuzzy
```

## Debug

```sh
CDB_DEBUG=1 ./cdb   # dump des allocations + schéma de thème
```
