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
roots.json, layout.json, window.json, dirty.json, llm.json,
llm_live.json, llm_slots/.

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
```

## Debug

```sh
CDB_DEBUG=1 ./cdb   # dump des allocations + schéma de thème
```
