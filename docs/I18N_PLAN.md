# Plan i18n — CodeDashBoard (CDB)

**Statut** : phase 0 et jalons A/B livrés ; **prochaine étape : Jalon C**.
**Date** : 2025-08-27 (rév. 4 — A et B validés de bout en bout).
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

### Jalon C — Petits fichiers ← PROCHAIN
`ibox.c` (~4 chaînes), `session.c` (~3), `llmlive.c` (~1) : marquage,
extraction, traduction. Valide la recette sur des cas réels mais petits.

### Jalon D — `main.c` (~50 chaînes)
Marquage + passage à l'anglais pivot. Attention aux `g_strdup_printf` avec
formats, et au pluriel `"Supprimer %u éléments"` →
`ngettext("Delete %u item", "Delete %u items", n)`.

### Jalon E — `llmtile.c` (~46 chaînes)
Marquage, slots inclus. Refactor du hack `"%d tour%s"` en vrai
`ngettext("turn", "turns", n)`.

### Jalon F — `llmcore.c` (~36 chaînes UI + prompts système)
Tri entre chaînes UI/prompts (à marquer) et clés JSON (à exclure).
**Simplifié par la phase 0** : plus de tri `/CDB::` à faire.

### Jalon G — Garde-fous et sélecteur
- `make test` étendu : `msgfmt --check` sur tous les `.po` (échec = test rouge).
- Sélecteur de langue dans l'UI + persistance config.
- Section « Internationalisation » dans `CLAUDE.md` (règles de marquage,
  procédure d'ajout d'une langue).

---

## 5. Règles de marquage (référence pour les jalons C→F)

1. **Marquer `_()`** : toute chaîne visible par l'utilisateur (labels,
   boutons, tooltips, dialogues, annonces CDB, messages console, **prompts
   système LLM**).
2. **Ne pas marquer** : clés JSON, noms de thèmes, CSS, formats purs
   (`"%s"`), chaînes techniques sans texte.
3. **Pluriels** : toujours `ngettext(singulier, pluriel, n)` — jamais de
   concaténation du type `"%d tour%s"`.
4. **Formats** : conserver les marqueurs `%s`, `%d` ; si l'ordre peut changer
   selon la langue, utiliser les marqueurs positionnels `%1$s`, `%2$d`.
5. **Commentaires** : restent en français, inchangés.

---

## 6. Commandes (après Jalon A)

```sh
make pot        # régénère po/cdb.pot depuis les sources marquées
make po         # met à jour fr.po / en.po depuis le .pot (msgmerge)
make mo         # compile po/locale/<lang>/LC_MESSAGES/cdb.mo
make run        # lance CDB (langue = système)
LANG=en_US.UTF-8 ./cdb   # force l'anglais
```

---

*Plan établi conjointement avec Claude (2025-08-27). Rév. 3 : phase 0
(cleanup `/CDB::`) livrée. Toute modification passe par une révision de ce
document.*
