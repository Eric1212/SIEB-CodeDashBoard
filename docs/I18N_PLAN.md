# Plan i18n — CodeDashBoard (CDB)

**Statut** : phase 0 et jalons A, B, C, D1, D1.5, D2, D3, D4, E livrés — **le
Jalon E est bouclé** ; **prochaine étape : Jalon F** (`llmcore.c`).
**Date** : 2025-08-28 (rév. 7 — Jalon E livré : 71 msgids de `llmtile.c`,
premier pluriel de la tuile via `ngettext`, patron `N_()` pour les tables
statiques (§6.7) ; catalogue à 196 msgids, `fr.po` 196/196 traduits,
`make i18n-check` vert sur les deux langues, zéro entrée fuzzy).
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

### Jalon E — `llmtile.c` — ✅ `0c1a766` (71 msgids ; +10 titres de sections)
Marquage, slots inclus. **Le refactor imposé est fait** : le hack
`"%d tour%s"` avec son `> 1 ? "s" : ""` est détruit aux deux sites (titre du
popover et tooltip), remplacés par `ngettext("… %d turn", "… %d turns", n)` —
2 msgids à 2 formes, premier pluriel de la tuile et 2e du projet (après D2).
Autres points de E :
- **Trois chaînes « affichées et servantes de clé » vérifiées** (le motif qui
  a mordu en D3.5 et D4) : `[image %u]` n'est jamais re-parsé, `[CDB] ` est
  bien une paire pose/test, `"[CDB] "`/`voice-*`/`slot-action` restent des
  marqueurs techniques. Aucune n'a été marquée.
- **`N_()` découvert en cours de route**, par comptage de msgids et non par
  lecture du `.po` : les six libellés du menu des slots, stockés dans une
  table `static const`, n'entraient pas au catalogue. Voir §6.7.
- **Restés volontairement non marqués** (conformité §6.2 et §5) : étiquettes
  d'acteur `— Éric —` / `— Claude —` / `— CDB · local —` (noms propres),
  `CDB_TEST_PROJET` + `[PROJET]` + `[CHEMIN]` (vestiges de harnais), glyphes
  du spinner, `"%s : %g %s"` / `"%.2f USD"` / `"%dh %02dm %02ds"` (lectures
  de calcul et d'unités, sans mot).
- **Tranché par Éric (rév. 7)** : (1) la question du deux-points est **nulle** —
  une espace avant `:` dans `"%s : %g %s × %g = %.2f USD"` n'est pas un fait de
  langue : ces lectures restent non marquées (§6.2). **Mais** la formulation
  d'origine (« on n'y revient pas en F ») était fausse et Éric l'a relevée :
  l'exemption porte sur *l'absence de mot*, jamais sur *l'absence d'accent*.
  `et`, `ou`, `dans`, `requis`, `vide`, `fichier`, `manquant` ne s'accentuent
  pas. Un scan à mots (non à accents) sur `llmcore.c` signale **198 lignes à
  lire** — dont `"text manquant ou vide."` (2084), `"fichier absent : utilise
  cdb_create."` (2102), `"before_hash obsolete ou absent…"` (2163). Contrôle
  fait sur `llmtile.c` avec ce même filtre : **0**, E n'est pas atteint.
  ⚠ **Recadrage d'Éric (rév. 7)** : ma formule « liste à lire » était encore
  trop généreuse. Un script ne qualifie pas, il situe ; la qualification se
  fait à la lecture. Règle posée en §6.8, **F se lira en entier**. Le même
  scan a d'ailleurs annoncé « vide » un `msgstr ""` suivi de ses
  continuations : deux ratés, une seule cause.
- §6.3 a une **récidive en F** : `llmcore.c:2920` (`nlines > 1 ? "s" : ""`) et
  la famille `%u ligne%s` (2841, 2856, 2878, 2919). À traiter comme en E.
- **Terminologie actée par Éric (rév. 7)** : `Settings` → « Réglages » (sa
  préférence va à « Paramètres » — un seul `msgstr`, 2 références, à tout
  moment réversible) ; `LLM` → **« Modèles »**, le sigle étant inconnu de
  ceux qui lisent l'écran ; `Tools` → « Outils » ; `Providers` → **« Fournis-
  seurs »** (« Inférenceurs » écarté). Conséquence en chaîne, voulue : le nom
  commun devient « fournisseur » dans les phrases — sinon le panneau dirait
  Fournisseurs et le message « ce provider ». `fr.po` retouché en 5 sites
  (155, 159, 163, 178, 445) ; les msgids anglais gardent `provider`. Le chemin
  français se lit **« Réglages → Modèles → Fournisseurs / Outils »**.
- **Fait hors E, mais nécessaire à cette décision** : les titres de sections
  du panneau étaient nus (`main.c:2835`, `2845`, `2887`). D3.5 avait libéré le
  titre — plus aucune clé n'en dépend, vérifié — mais personne n'y avait posé
  le `_()`, si bien que « Outils » et « Fournisseurs » n'existaient nulle part
  à l'écran. Marqués en `N_()` : 7 titres. **Restés nus volontairement** :
  `HyperCharm`, `OpenCode`, `OpenRouter`, `OpenAi-Compatible`, `GitHub/Git`
  (noms, §6.2). Les trois `placeholder` étaient **en français dans le code** :
  ils ont d'abord été écrits en anglais, sinon le msgid fût entré français au
  catalogue. Les `id` ne sont pas marqués — `g_strcmp0(sec->id, "Tools")`
  dispatche dessus, et traduire la clé choisirait un autre formulaire.
  `_(NULL)` : toléré sur cette machine (sondé, catalogue chargé et absent),
  mais non documenté par `dgettext` → garde explicite à `2887`. Bilan :
  +10 msgids, catalogue à 206, 0 trou, 0 fuzzy.
### Jalon F — `llmcore.c` (UI + prompts système)
Tri entre chaînes UI/prompts (à marquer) et clés JSON (à exclure).
**Simplifié par la phase 0** : plus de tri `/CDB::` à faire.
⚠ ~20 `g_strdup_printf("lecture impossible : %s", …)` y sont des **résultats
d'outils envoyés au modèle ET affichés dans la boîte** : à marquer, en
gardant présent qu'ils partent aussi sur le réseau.
**Acquis de E à appliquer en F** : les tables de libellés (dont
`LLM_PROFILE_NAMES`, comparée en `llmtile.c:1920`) prennent `N_()` à la
définition et `_()` à l'usage — règle §6.7. La paire `"[CDB] "` (posée à
`llmcore.c:4761`, testée par `g_str_has_prefix` à `llmtile.c:1539`) est une
convention de transport : **ne pas la marquer**, elle casse le rendu de la
voix CDB si elle devient traduite.
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
   ⚠ **Le test est « y a-t-il un mot », jamais « y a-t-il un accent ».**
   `et`, `ou`, `dans`, `vide`, `absent`, `requis`, `manquant`, `fichier` sont
   français et ne s'accentuent pas : `"text manquant ou vide."` est une phrase
   à marquer, `"%s : %g %s × %g = %.2f USD"` n'en est pas une. Un repérage
   fondé sur les diacritiques laisse passer la première classe entière.
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
7. **Tableaux statiques** : une table `static const` ne peut pas porter `_()`
   — `gettext()` n'est pas une constante C, l'initialisation statique la
   refuse. Marquer `N_()` à la définition (extraction par `xgettext`, no-op
   au runtime) et résoudre `_()` au point d'usage. Faute de `N_()`, la chaîne
   n'entre pas au catalogue **sans le moindre avertissement** : le défaut est
   invisible à la compilation, à `msgfmt --check` et à `i18n-check`. Seul le
   comptage de msgids (`make pot` : 190 → 196) l'a révélé en E.
8. **Méthode de repérage — règle d'Éric (rév. 7)** : aucun script, aucun grep
   ne décide ce qui doit être traduit. La décision se prend en **lisant**,
   ligne à ligne — §5 l'avait déjà tranché pour les diagnostics (trois
   classificateurs, trois verdicts contradictoires : 34, 7, 67). Un scan peut
   tout au plus *situer* : retrouver un numéro de ligne, compter des msgids
   pour constater le silence d'`xgettext`. Dès qu'on lui demande de
   **qualifier**, il rate, et E l'a montré deux fois : il a laissé passer
   `"text manquant ou vide."` (aucun accent) et annoncé « vide » un
   `msgstr ""` suivi de ses lignes de continuation. **Le Jalon F se lira en
   entier**, `llmcore.c` compris, sans arbitre automatique.

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
