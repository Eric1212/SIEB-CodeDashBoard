# Plan i18n — CodeDashBoard (CDB)

**Statut** : phase 0 et jalons A, B, C, D1, D1.5, D2, D3, D4 livrés — **le
Jalon D est bouclé** ; **prochaine étape : Jalon E** (`llmtile.c`).
**Date** : 2025-08-28 (rév. 6 — Jalon D bouclé : D3 + D4 livrés, msgmerge en
`--no-fuzzy-matching`, catalogue à 125 msgids tous traduits).
**Décideur** : Éric Boucher.

---

## 1. Objectif

Rendre l'interface et les messages de CDB traduisibles via **gettext**,
avec l'**anglais comme langue pivot** (msgid). Le français actuel devient
une traduction (`fr.po`), l'anglais une traduction (`en.po`).

**Périmètre i18n** : tout ce qui est exposé à l'utilisateur — chaînes UI,
messages console/logs (`g_print`, `g_printerr`, etc.), **et prompts système
LLM** (les instructions au modèle doivent suivre la langue UI choisie par
l'utilisateur ; un init-prompt anglais ne doit pas recevoir de réponses en
français).

**Hors périmètre (reste inchangé)** :
- Les clés JSON (contrats techniques avec l'API LLM).
- Le CSS, les noms de thèmes (`Adwaita`, `Adwaita-dark`).
- `CLAUDE.md` et les commentaires du code (documentation interne, en français).

---

## 2. Choix techniques validés

| Décision | Choix | Justification |
|---|---|---|
| **Moteur i18n** | gettext via `glib/gi18n.h` | Standard GNOME, déjà linké via glib, extraction automatique (`xgettext`), gestion native des pluriels (`ngettext`), outillage traducteur mûr. |
| **Langue pivot** | **Anglais** (msgid) | Standard industrie ; facilite les contributions externes ; les outils gettext supposent l'anglais. |
| **Traductions initiales** | `fr` (état actuel) + `en` (pivot, ~identique au msgid) | Couverture immédiate des deux langues de travail. |
| **Sélection langue** | Système par défaut (`setlocale(LC_ALL, "")`) + override `Système / Français / English` en config, **appliqué au redémarrage** | La re-traduction à chaud de GTK est complexe et risquée ; un redémarrage est honnête. |
| **Locale dev** | `bindtextdomain` vers `./po/locale` (relatif au binaire) | `make run` fonctionne sans rien installer sur le système. |

---

## 3. Phase 0 — Cleanup `/CDB::` : ✅ TERMINÉE

L'ancien protocole textuel `/CDB::` (prédécesseur des `tool_calls` JSON) a
été **entièrement retiré** — commit `c00d8c5`. Seuls subsistaient des
résidus ; le parsing texte n'existait déjà plus.

- Suppression des fonctions de migration de personas (code mort, 118 lignes).
- Retrait de `cdb_retries` et `CDB_RETRY_MAX` (jamais lus).
- Nettoyage des prompts (persona + `tools_policy`) : plus aucune mention
  du protocole fantôme.
- Réécriture des 16 commentaires obsolètes ; MAJ `CLAUDE.md`.
- **Conservé** (machinerie `tool_calls` active) : `CdbCmdSpec`, `CdbResult`,
  `CdbDecision`, `CdbPoll`, `cdb_next_step`, `cmd_queue`, `cdb_results`,
  `tools_schema_cdb_*`, `cdb_tool_*`.

Validé : 0 warning, `make test` 17/17.

---

## 4. Jalons i18n (chaque jalon compile, tourne, est validable)

### Jalon A — Infrastructure gettext ✅ (commit `b6d3414`)
- `src/i18n.h` : wrapper définissant `GETTEXT_PACKAGE` (`"cdb"`), `LOCALEDIR`,
  inclusion de `<glib/gi18n.h>`, macros `_()`, `N_()`, `ngettext()`.
- `src/main.c` : `setlocale` + `bindtextdomain` + `textdomain` en tout début
  de `main()`, avant toute init GTK.
- `Makefile` : cibles `pot` (extraction), `po` (mise à jour), `mo`
  (compilation), mise à jour de `clean`.
- `po/` : squelette `POTFILES.in`, `LINGUAS` (`fr en`).
- `.gitignore` : `*.mo`, `po/locale/`.

**Critère de validation** : `make pot && make mo` produit un `.mo` valide
(même vide de traductions).

### Jalon B — Chaîne témoin (end-to-end) ✅
**Chaîne livrée** : `main.c:1822` `"Explorateur"` → msgid `"Explorer"`, `fr.po`
→ `"Explorateur"`, `en.po` msgstr vide (retombe sur le msgid).

**Découverte validée par `strace` sur le binaire** : libintl résout bien la
locale régionale `fr_CA.UTF-8` vers le catalogue `fr` (il tente
`fr_CA.UTF-8`, `fr_CA`, `fr_CA.utf8`, `fr.UTF-8`, `fr.utf8`, puis **`fr`** ✅).
**Pas besoin de catalogue `fr_CA` distinct** — règle à retenir pour toute
future langue régionale (`pt_BR`, `es_MX`…).
- Marquer **une** chaîne de `main.c` avec `_()`.
- Créer `fr.po` et `en.po` minimaux.
- **Critère** : `LANG=en_US.UTF-8 ./cdb` affiche la chaîne en anglais ;
  sans `LANG` (français système), elle reste en français.

### Jalon C — Petits fichiers ✅ (`fc46a16`)
`ibox.c`, `session.c`, `llmlive.c` + les libellés que la tuile pose sur la
boîte (sortis de E pour recoller une famille sémantique coupée). 14 msgids.

### Jalon D — `main.c` — **éclaté en sous-jalons**
Le fichier est trop gros pour un seul diff relisible (~50 chaînes annoncées,
en réalité ~120 littérales dont beaucoup ne sont pas de l'UI).

| Sous-jalon | Contenu | État |
|---|---|---|
| **D1** | Les quatre dialogues de l'explorateur | ✅ `89751b5` (11 msgids) |
| **D1.5** | **Diagnostics** : 26 rapports d'échec marqués, 11 traces de télémétrie détruites, journal `CDB_DEBUG` conservé et marqué | ✅ `b096a08` + `2632ae4` — politique au §5 |
| **D2** | Menu contextuel, validations de nom, alertes, tooltip d'ajout de root + **premier `ngettext`** | ✅ `4485020` (18 msgids) |
| **D3** | Settings / LLM : labels, placeholders, descriptions de catégories, `Vide\n(emplacement réservé)` | ← **PROCHAIN** |
| **D4** | Menus du HeaderBar et des tuiles. Fait : le harnais `CDB_TEST_SETTINGS` ne retrouve plus la fenêtre par son titre mais par un marqueur technique (`g_object_set_data "cdb-settings"`) — le titre devient donc traduisable. Option B appliquée : `Settings` → **« Réglages »** (le plan promettait « Paramètres » ; livré « Réglages », terme GNOME d'un panneau de config, et le msgid est partagé avec le titre de tuile), `Exit` → « Quitter », `About CDB` → « À propos de CDB ». `Settings` = 1 msgid, 2 références (`layout.c:325` + `main.c:4644`) | ✅ `1494fdd` (14 msgids) |

### Jalon E — `llmtile.c` (~44 chaînes restantes)
Marquage, slots inclus. Refactor du hack `"%d tour%s"` en vrai
`ngettext("turn", "turns", n)`.

### Jalon F — `llmcore.c` (UI + prompts système)
Tri entre chaînes UI/prompts (à marquer) et clés JSON (à exclure).
**Simplifié par la phase 0** : plus de tri `/CDB::` à faire.
⚠ ~20 `g_strdup_printf("lecture impossible : %s", …)` y sont des **résultats
d'outils envoyés au modèle ET affichés dans la boîte** : à marquer, en
gardant présent qu'ils partent aussi sur le réseau.

### Jalon G — Sélecteur de langue et docs
- Sélecteur de langue dans l'UI + persistance config.
- Section « Internationalisation » dans `CLAUDE.md` (règles de marquage,
  procédure d'ajout d'une langue).
- ~~`make test` étendu à `msgfmt --check`~~ : **frappé.** Les tests unitaires
  `tests/agent_state` ont été retirés du projet (`19805d8`), sur décision
  d'Éric. Le garde-fou i18n vit désormais dans `make i18n-check` — voir §7.

---

## 5. Politique des diagnostics (décidée en D1.5)

74 appels de sortie dans le projet (`g_print*`, `g_warning`, un `fprintf`),
triés **au cas par cas, en lisant le code** — et non par heuristique : trois
scripts de classification successifs se sont contredits (34, puis 7, puis 67
sous garde), la méthode a donc été abandonnée au profit de la lecture.

| Population | Traitement | Motif |
|---|---|---|
| **Rapports d'échec** (26 sites, 7 fichiers) | **marqués** | incondi­tionnels *par conception* : une écriture de config qui échoue doit rester visible. Non marqués, un utilisateur en `LANG=en` apprenait sa perte de données en français |
| **Passes-simples GLib** (`"CDB: %s"` avec `error->message`) | **non marqués** | le texte vient de GLib, déjà traduit dans ~80 langues ; un msgid serait un doublon |
| **Journal `CDB_DEBUG`** (12 lignes à prose) | **marquées** | `CDB_DEBUG=1` est documenté dans `CLAUDE.md` § Debug : surface publique, donc pas de franglais |
| **Sans mot à traduire** (`load_file path=%s`, `activate pos=%u`, `tile id=%s widget=%p`, `[btn] busy=%d`, fragments `" [%u]=%s"`) | **non marquées** | le `msgstr` serait identique au `msgid` byte à byte : du bruit de catalogue |
| **Télémétrie d'une traque close** (11 sites) | **détruits** | `row=%p`, `mods=0x%x`, « CLAIMÉ », `MUTATION` : ils disaient *comment* le code marche, pas *ce qui se passe*. La traque du multi-select par geste est gagnée |
| **Harnais `CDB_TEST_*`** (`test_settings_step`, `test_modal_idle`, `cdb_test_delay`…) | **gardés, non marqués** | autorisés mais reconnus vestiges : **dette de ménage**. Traduire des chaînes qui sauteront au prochain ménage serait du travail perdu |

**Ce que le journal est devenu** : ouverture → sauvegarde → suppression →
création → sélection → rebuild → thème, avec les noms. Un log lisible **sans
la fenêtre** — terminal, copié‑collé, rapport de bug — ce qui était le critère
décisif : une trace ne se juge pas contre l'écran, mais contre le log seul.

**Le piège à connaître, en D1.5 et de nouveau en D2** : `msgmerge` marie les
msgid qui se ressemblent. Il a proposé `Delete folder` → **« Nouveau dossier »**
(l'inverse exact de l'action) et réduit un pluriel à `"Supprimer"` sans `%u`.
`msgfmt --check` les accepte sans un mot ; seule la détection des entrées
`fuzzy` les fait voir. D'où le durcissement de `make i18n-check`.

---

## 6. Règles de marquage (référence pour les jalons E→F)

1. **Marquer `_()`** : toute chaîne visible par l'utilisateur (labels,
   boutons, tooltips, dialogues, annonces CDB, **prompts système LLM**),
   ainsi que les rapports d'échec et le journal `CDB_DEBUG` — avec la
   nuance décisive du §5.
2. **Ne pas marquer** : clés JSON, noms de thèmes, CSS, **identifiants
   d'action** (`win.new-window`) et d'icônes (`view-refresh-symbolic`),
   **jetons comparés en code** (`"DELETE"` de la confirmation de suppression),
   commandes shell (`"cp -a %s %s"`), **passes-simples GLib** (`"CDB: %s"`
   dont le texte vient d'un `error->message` déjà traduit par GLib), et toute
   **chaîne sans mot à traduire** (`"load_file path=%s"`, `"tile id=%s
   widget=%p"`) — le `msgstr` y serait identique au `msgid` byte à byte.
3. **Pluriels** : toujours `ngettext(singulier, pluriel, n)` — jamais de
   concaténation du type `"%d tour%s"`. Premier cas réel livré en D2.
4. **Formats** : conserver les marqueurs `%s`, `%d` ; si l'ordre peut changer
   selon la langue, utiliser les positionnels `%1$s`, `%2$d`.
   **Une phrase montée en morceaux doit être défaite, pas traduite au
   morceau** : `"Créer %s dans :\n%s"` recevait le fragment français « un
   dossier » / « un fichier » ; D1 l'a remplacé par deux msgids de phrase
   entière, sinon chaque langue hérite de l'ordre de mots du français.
5. **Ponctuation** : l'espace française avant `: ; ! ?` et les chevrons `« »`
   vivent dans `fr.po`, jamais dans le msgid anglais. Les points de suspension
   (`…`) restent dans le msgid : ils signalent une boîte qui s'ouvre.
6. **Commentaires** : restent en français, inchangés.

---

## 7. Commandes et garde-fous

```sh
make pot        # régénère po/cdb.pot depuis les sources marquées
make po         # met à jour fr.po / en.po depuis le .pot (msgmerge)
make mo         # compile po/locale/<lang>/LC_MESSAGES/cdb.mo
make            # construit AUSSI les catalogues : sans ça, un `make clean`
                # laissait l'app sans traduction (piège réel, vu en D1.5)
make i18n-check # msgfmt --check + exige ZÉRO entrée fuzzy
make run        # lance CDB (langue = système)
LANG=en_US.UTF-8 ./cdb   # force l'anglais
```

**Le garde-fou `i18n-check` n'est pas décoratif.** `msgfmt --check` accepte
une entrée `#, fuzzy` sans un mot, alors que `gettext` l'**ignore** à
l'exécution : une traduction fuzzy est une traduction morte. En D2, `msgmerge`
avait marié `Delete folder` à « Nouveau dossier » — l'inverse exact de
l'action — et réduit un pluriel à `"Supprimer"` sans `%u`. Seul le comptage
des fuzzy l'a fait voir. Un contrôle qui ne peut pas échouer ne prouve rien :
le motif a été vérifié sur la forme réelle (`#, fuzzy, c-format` → 1,
`#, c-format` → 0).

**Ce qu'on ne peut PAS vérifier par le CLI** : une clé finissant par `\n` est
injouable en shell, la commande `$(…)` rognant le saut de ligne terminal —
d'où de faux « 0/14 conformes » en D1.5. La preuve passe par `msgunfmt` sur le
`.mo` compilé, ou par `ngettext` pour les pluriels.

---

*Plan établi conjointement avec Claude (2025-08-27). Rév. 5 : Jalon D éclaté
en D1/D1.5/D2/D3/D4 ; politique des diagnostics ajoutée (§5) ; règles de non-
marquage portées de 2 à 5 catégories (§6.2) ; interdiction des phrases montées
en morceaux (§6.4) ; garde-fou fuzzy et limites du contrôle CLI (§7).
Toute modification passe par une révision de ce document.*
