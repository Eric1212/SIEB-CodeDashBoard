# Plan i18n — CodeDashBoard (CDB)

**Statut** : phase 0 et jalons A, B, C, D1, D1.5, D2, D3, D4, E, F, **G** livrés.
Le plan est clos en tant que tel : tout ce qui était traduisable l'est, la
langue se choisit dans l'interface et se relit au démarrage. Les deux ménages de
config demandés par Éric sont faits (`window.json` → `layout.json`,
`roots.json` → `llm.json`). Reste hors code la validation d'écran par Éric.
**Date** : 2026-08-29 (rév. 9 — Jalon G livré : sélecteur de langue dans
Réglages → Général, endonymes hors gettext, liste lue sur disque, `i18n_apply()`
à escalier de candidats qui garde le territoire (§6.11), `layout.json` et
`llm.json` régis par la même règle — une écriture ne détruit pas ce
qu'elle ne connaît pas (§6.12) — avec rapatriement de `window.json` et des
racines ; au passage, un widget jamais créé corrigé (§4 G.2), un écrasement de
`last_file` corrigé avec le déplacement, et les formats réels de `session.h`,
`llm.h` et `roots.h` rétablis. Catalogue à **303 msgids**, 300 traduits, 3 non
traduits **vouloirs** (`MINIMAL`/`DEFAULT`/`YOLO`), `make i18n-check` vert sur
les deux langues, zéro fuzzy, build à 0 warning. Le « 2025-08-28 » de la
révision 8 était une année erronée : la machine et l'historique portent 2026).
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
### Jalon F — `llmcore.c` — ✅ `7be9bdb` (90 msgids ; prompt en msgid anglais)
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

**Journal de la passe shell de F** — `llmcore.c` est lu en entier (4 985
lignes) et marqué ; `fr.po` a été rempli à la main, sans `msgmerge`.

1. **Une ligne non marquée, bloquée par le garde-fou de CDB lui-même** :
   `llmcore.c:2789`, le message de refus de la séquence d'échappement NUL
   (un antislash suivi des cinq caractères `u0000`). Le détecteur travaille
   sur le *texte* de l'argument — c'est son rôle, `json-glib` ne donnant pas
   les longueurs — donc il ne distingue pas un vrai NUL d'un document qui
   nomme ce NUL. Claude ne peut pas réécrire cette ligne, ni même la citer
   en documentation. À marquer au `sed` en passe shell, ou à assumer non
   marquée (§6.2 y suffirait : le msgid ne porte qu'un jeton).
2. **Le contrôle décisif est le COMPTE, pas la syntaxe.** `fr.po` porte
   ~85 msgids recopiés à la main : une virgule d'écart entre ma copie et le
   code ne provoque aucune erreur, elle crée une entrée muette. Donc
   `make pot && make po` doit rendre **zéro trou** (`msgattrib
   --untranslated`) et le compte : attendu 286, **mesuré 297** (§ F). Tout
   msgid que je n'aurais pas su recopier apparaîtra là, vide, et se corrigera
   en une passe.
3. **`msgfmt --check` : fait, 0 erreur.** Build propre (0 warning) sur les
   ~90 écritures de F posées sans compilation — le risque était pris, il est
   payé : rien n'était cassé.
4. **Contrôle de fidélité des deux blocs de prompt (mesuré, et il a mordu)** :
   le `msgstr` français de la persona et de la policy a été comparé au texte **du
   commit précédent** (`git show HEAD:src/llmcore.c`), en reconstituant les
   littérales C, en les dé‑échappant, et en relisant la traduction **dans le `.mo`
   compilé** par `gettext` lui‑même — pas en parsant le `.po` à la main.

   | bloc | longueur | écarts | nature |
   |---|---|---|---|
   | persona (`LLM_INITPROMPT_DEFAULT`) | 1 162 = 1 162 | **0** | identique atome pour atome |
   | policy (`tools_policy`) | 2 231 = 2 231 | **20** | tous des accents (`e`→`è`, `a`→`à`, `E`→`É`, `e`→`ê`) |

   La décision d'Éric du point 1 (« obsolete » → « obsolète ») valait pour les
   messages d'erreur ; elle est **étendue ici** aux accents de la policy, et le
   contrôle dit qu'elle est la seule chose qui ait bougé : ta session francophone
   lit le prompt d'hier, accents en place.

   ⚠ **Ce que le contrôle a attrapé, et qui serait passé inaperçu** : une première
   version du test s'arrêtait aux quatre premières différences. La vingtième était
   un `t` devenu `s` — j'avais changé « entre ta découverte et **ta** destruction »
   en « … et **sa** destruction » dans le prompt que le modèle reçoit. Corrigé
   (« sa » est probablement un meilleur français, et c'est ce que dit le msgid
   anglais *its destruction*, mais ce n'est pas ma ligne éditoriale : pour le
   remettre, un seul mot sur la ligne 803 de `fr.po`). **Un contrôle qui
   échantillonne ne prouve rien** : la comparaison doit être intégrale, caractère
   par caractère.
   Deux bogues de mon propre contrôleur, consignés pour la même raison :
   `enumerate(L, a)` renumérote toute la liste depuis `a` (mes plages ressortaient
   gonflées de `a-1`, et je comparais 4 967 caractères à 5 283 sans le voir), et
   mes marqueurs de fin de plage étaient français dans un fichier devenu anglais
   (`StopIteration`). Les deux venaient de l'outil, pas du texte — le moment
   d'accuser son mètre avant d'innocenter sa mesure.
5. **RÉSOLU — et j'avais écrit l'inverse.** `LLM_PROFILE_NAMES` n'est pas à
   marquer telle quelle : défini `llmtoolpref.c:14` (`MINIMAL`, `DEFAULT`,
   `YOLO`), il sert **de clé persistée** — `llmtoolpref.c:334` compare la
   valeur relue de `active.profile` dans `llm.json` avec `g_strcmp0` — **et**
   de libellé affiché (`main.c:3384`, `llmtile.c:2402`, `2412`). C'est le cas
   D3.5/D4 en pleine face : traduire sans séparer casse la relecture de la
   config au démarrage. Les trois noms restent donc nus. **Si on veut les
   traduire, c'est le patron D3.5** : une clé technique d'un côté, un libellé
   `N_()` de l'autre — un refactor de `llmtoolpref.c` et 3 sites, pas une
   ligne de `.po`. Décision Éric.
6. **RÉSOLU — l'hypothèse tenait** : `xgettext` extrait bien les littérales
   du corps d'un `#define`. La persona et la policy sont au catalogue,
   confirmé par le compte (287 msgids alors) et non par une lecture du `.pot`. Le
   repli (sortir la persona de sa macro) n'a pas servi. `N_(String)` est bien
   `(String)` (`gi18n.h:30`), sans cast : l'initialiseur de tableau compile.
7. `en.po` ne demande rien : msgids déjà anglais, `msgstr` vide = identité.
8. **Décision d'Éric requise** sur les résumés de la barre d'approbation
   (`llmcore.c` : replace, create, delete, insert) : ils portent le plural
   hack avec DEUX pluriels dans la même phrase (« -3 lignes / +5 lignes »),
   qu'un seul `ngettext` ne peut pas rendre. Laissés non marqués. Trois
   issues : deux msgids (`removed:` / `added:`), un format sans mot
   (« -3 / +5 »), ou phrase unique par opération.
9. **Un mot choisi par Claude, à valider** : l'étiquette du bloc de
   raisonnement, `mdview.c:29` — `Thinking` → « Raisonnement ».

### Deux chaînes trouvées HORS `llmcore.c`, après F

En balayant pour « couvrir tout ce qui manque », deux littérales françaises
nues ont été trouvées **dans des fichiers déjà jalonnés** :

- `ibox.c:677` — `n == 1 ? "ligne" : "lignes"`, le §6.3 en pleine face, dans
  un « petit fichier » supposé traité au **Jalon C**. Corrigé en
  `ngettext("%lu line", "%lu lines", n)` : 3ᵉ pluriel réel du projet.
- `mdview.c:29` — `#define THINK_LABEL "Thinking"`, étiquette posée à
  `mdview.c:363`. Marque en `N_()`/`_()`. Et le fichier **n'incluait pas
  `i18n.h`** : la marque y aurait été une erreur de compilation, détectée en
  lisant les `#include`, pas en écrivant.

Leçon de méthode, qui rejoint §6.8 par l'autre bout : mes deux localisateurs
ont raté **chacun une chose différente**. Celui à dictionnaire de mots a trouvé
`ibox.c` mais pas `session.c` ; celui « toute littérale avec un espace » a
trouvé `session.c` mais **a laissé passer `ligne`/`lignes`** — ces mots n'ont
pas d'espace. Aucun des deux n'aurait dû trancher, et aucun n'a tranché : ils
ont situés, la décision est venue de la lecture des sites.

### État de F (mesuré, pas déclaré)

**297 msgids** au catalogue, build à 0 warning, `msgmerge` sans erreur,
`i18n-check` vert sur fr et en, **0 fuzzy**. Les trous sont au nombre de trois
et sont **vouloirs** : `MINIMAL`, `DEFAULT`, `YOLO`, mots communs aux deux
langues, malgré tout extraits sur la décision d'Éric (« au cas où », parce que
toutes les langues ne sont pas aussi proches que fr et en). Leur `msgstr` reste
vide exprès — `gettext` retombe sur le msgid, l'écran français ne change pas —
et un commentaire `TRANSLATORS` le dit dans le `.po` (vérifié présent).

Preuve que le tri à la main a fonctionné, en références lues dans le `.po` :
`invalid JSON arguments for %s.` **7 sites**, `missing path.` **6**,
`absolute path required.` **6**, `cannot read: %s` **4**, `Cancelled by the
user.` **3**. Vingt-six phrases physiques sont devenues quatre entrées.

### Fermé depuis la première version de ce journal

- **Unités de taille : la clé a été supprimée, pas traduite** (décision
  d'Éric). `llm_slots_size_str` rend `%zu o`, `%.1f Kio`, `%.1f Mio` en dur, et
  les trois msgids sont sortis du `.pot` et du `fr.po` (300 → 297). Trois
  faits, tous vérifiés : le diviseur est 1024, donc le multiple est **binaire**
  — `Ko`/`Mo` et `KB`/`MB` valent 1000, et cette imprécision était dans le code
  **avant** moi, en français comme en anglais ; le séparateur décimal vient de
  `LC_NUMERIC` (`i18n.c:44`, sonde compilée : `[1.5]` en `C`, `[1,5]` en fr) ;
  et ce qui restait à « traduire » — `B` contre `o` — n'était pas une
  divergence de langue mais de racine de mot. **Un symbole d'unité n'est pas de
  la prose** ; le consigner ici pour qu'aucun jalon futur ne remette une unité
  derrière gettext. Au passage, `LANG=fr_CA.UTF-8` est la locale réelle du
  poste et `fr_CA.utf8` est générée : la virgule fonctionne, et le Jalon B
  (« pas besoin de catalogue `fr_CA` distinct ») s'en trouve confirmé.
- **`llmcore.c:2789` marqué**, via shell, en construisant les antislashs avec
  `printf` pour que la séquence n'apparaisse jamais dans le texte de ma commande
  — le garde-fou de CDB la refuserait, et il a raison. **Piège consigné parce
  qu'il mord** : en remplacement `sed`, chaque antislash du fichier coûte deux
  antislashs dans la commande. Ma première tentative n'en a rendu qu'un, donc un
  antislash unique devant `u0000` dans une littérale C — ce que C lit comme un
  nom de caractère universel désignant le NUL lui-même, c'est-à-dire
  précisément le défaut que ce code existe pour refuser, avec troncature à
  `strlen`. Détecté à `cat -A`, corrigé, revérifié aux octets (`od -c`) et
  contrôlé dans le `.mo` : **zéro octet NUL** dans le msgstr compilé.
- **Point 5 (clés de profils) : fait, et j'avais écrit l'inverse.**
  `LLM_PROFILE_NAMES` est bien une clé persistée (`llmtoolpref.c:348` compare la
  valeur relue de `llm.json`, `:59` l'écrit, `llmtile.c:2414` la promeut dans le
  `g_object_set_data` comparé en `:1922`). Patron D3.5 appliqué : table de clés
  nue et inchangée, plus `LLM_PROFILE_LABELS` en `N_()` et `llm_profile_label()`
  pour les cinq sites d'affichage (`llmtile.c:1897`, `1899`, `1905`, `2402`,
  `main.c:3384`). Vérifié par l'usage : `llm_profile_name()` n'a plus qu'un
  appelant, celui qui écrit la clé.
- **Point 8 (double pluriel de la barre d'approbation) : résolu sans arbitre**,
  la contrainte étant technique et non éditoriale. Un seul des quatre sites avait
  deux compteurs ; les trois autres portent la **ligne entière** dans un
  `ngettext`, donc l'ordre des mots reste libre par langue. Pour `replace`, deux
  `ngettext` composés de part et d'autre du `/` (un symbole, pas de la prose),
  temporaires libérés sur le chemin unique. Rendu contrôlé au `.mo`.

### Ce qui reste ouvert

- **Un mot à valider** : `Thinking` → « Raisonnement » (choisi par moi).
- **Dernier mot « byte » du projet** : l'étiquette `bytes:` du résultat de
  `cdb_delete`. C'est un label du canal machine (règle 9), pas de la prose, donc
  laissé tel quel — mais si la cohérence avec « octet » doit aller jusque-là,
  c'est une ligne, et elle est à toi.
- **Dette assumée** : `main.c:5028` (`CDB: réouverture settings`), dernière
  littérale française nue du projet, appartient au harnais `CDB_TEST_*` — le §5
  la classe « gardée, non marquée ».
- **La leçon de contrôle, à ne pas perdre** : l'invariant « 0 trou » ne prouve
  **que** mes recopies retombent sur le code. Une chaîne que je n'ai pas marquée
  n'entre pas dans le `.pot`, donc rien ne manque jamais. Trois plages m'ont
  échappé ainsi (`llmcore.c:1937-1959`, `2791-2830`, et `ibox.c` que le Jalon C
  croyait traité) et seule la relecture les a trouvées. **Un compte n'est pas un
  inventaire** — cf. §6.8.
### Jalon G — Sélecteur de langue et docs — ✅ livré

**Réponses préalables d'Éric** (elles ont fixé le design, pas moi) : 1) le
sélecteur va dans **Réglages → Général**, et l'état de `window.json` rentre
aussi dans `layout.json` — « window.json s'est créé sans mon autorisation,
c'est aussi dans layout.json » ; 2) les noms de langues **dans leur propre
langue uniquement** ; 3) application live souhaitée, « si trop complexe, au
prochaine démarrage » ; un fil qui change de langue en cours de route regarde
l'utilisateur le gérer — aucun mécanisme de renouvellement n'a été construit.

**Mesuré**, après `make clean` et `po/locale/` supprimé (tout rebâti) :

| | |
|---|---|
| build | 0 erreur, 0 warning |
| catalogue | **303 msgids** (297 à la fin de F, +6 dans G, −1 mort en route : la plainte d'écriture de `window.json` n'est plus prononcée par personne), 300 traduits, 3 voulu, 0 fuzzy |
| `i18n-check` | ok fr, ok en |
| cohabitation dans `layout.json` | 6 écritures croisées : l'arbre ne perd plus la langue, la langue ne perd plus l'arbre (sonde hors UI, `session_config_path` fourni localement — le `~/.config/cdb` réel ne sert pas de banc d'essai) |
| cohabitation dans `llm.json` | les **sept** écrivains réels appelés un par un (`save_provider`, `set_allowed_models`, `save_retry429`, `save_retry5xx`, `switch_active`, `set_active_profile`, `save_tool_mode`) puis `roots_save` et `roots_write_last_file` : après chacun, `roots`, `last_file` **et la clé API** se relisent intacts |
| migrations | `window.json` → membre `"window"` de `layout.json` ; `roots.json` → membres `"roots"` + `"last_file"` de `llm.json`. Chacune : écriture, **relecture de la copie**, puis suppression de l'original. Deuxième lancement silencieux (idempotence vérifiée), et l'avis est sorti **en français** — un seul témoin qui prouve aussi que la langue du fichier est appliquée avant la première chaîne |
| changement de langue | même processus : `en` → `en_CA.UTF-8`, `fr` → `fr_CA.UTF-8`, catalogues ET formats différents |
| Réglages sous Xvfb (`CDB_TEST_SETTINGS`) | 0 `Gtk-CRITICAL` |

**Trois choses trouvées en route**, aucune de celles que je cherchais :

1. **`setlocale` ne connaît ni « en » ni « fr »** sur ce poste : les deux
   renvoient `ECHEC`, comme `fr.UTF-8`. Seules les formes complètes passent.
   Sans l'escalier de candidats (§6.11 c), le bouton « English » aurait été
   parfaitement décoratif — et « Français » n'aurait marché que par accident,
   parce que `LANG` est déjà `fr_CA.UTF-8`.
2. **Un widget déclaré et jamais créé** : `InitPromptCtx.status` (`main.c`) est
   lu à quatre endroits — ajouté au pied de l'éditeur, aligné, étiré, et servi
   par le retour « Saved ✓ » — sans qu'aucune ligne ne l'assigne ; `g_new0` le
   laissait à `NULL`. Trois `Gtk-CRITICAL` **à chaque ouverture de Réglages**,
   depuis `8f57e230` (2026-08-23), et la confirmation d'enregistrement jamais
   visible. Corrigé, vérifié au même harnais, **commit séparé** : il est
   antérieur à G et doit pouvoir être annulé indépendamment.
3. **`session.h` documentait un format faux** : il promettait un `session.json`
   qui n'a jamais existé et ignorait `llm.json`, `llm_live.json`, `llm_slots/`,
   `prompts/`. Un commentaire d'en-tête sur le format des données se lit comme
   une spécification — corrigé contre le répertoire réellement listé.

**Dettes nommées** : trois blocs de lecture‑modification‑écriture se
ressemblent dans `layout.c`. Je les ai laissés plutôt que de refactorer du code
écrit, compilé et sondé dans le même mouvement ; le jour où un quatrième
membre arrive, c'est `file_update(mutate, u)` qu'il faudra écrire.

**Exécuté au second avis — mon premier argument était le mauvais.** J'avais
objecté les `api_key` : `roots.json` porte l'état de l'explorateur, `llm.json`
porte des secrets, et les rejoindre voulait dire qu'un changement de projet
toucherait au fichier des clés. Éric a répondu « *tu sous-estimes llm* », et il
visait juste : la confidentialité ne bouge pas d'un octet — même fichier, même
dossier de session, mêmes permissions. Ce que je protégeais était une
**frontière de code**, pas une donnée. Et cette frontière était déjà franchie :
`llm.json` portait `providers`, `active`, `harness`, `tools` — quatre
préoccupations, pas une — avec une règle de préservation **écrite noir sur
blanc** dans `llm.h` depuis avant ce plan. Les racines n'ont fait qu'y entrer.

Fait donc : `roots` et `last_file` sont des membres de `llm.json`, `roots.json`
est migré une fois, relu, puis supprimé, avec la même discipline que
`window.json`. Le déplacement a trouvé un défaut en chemin (§6.12 : `roots_save`
écrasait `last_file`).

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

9. **Étiquette de protocole dans une phrase (décision d'Éric, F)** : quand une
   chaîne contient un **champ de protocole que le modèle doit réécrire atome
   pour atome** (`path:`, `line_count:`, `authored_range:`, `hash_block:`,
   `file_hash:`, `from_line`, `block_hash`…), l'étiquette garde sa forme
   technique — la traduire casserait l'appel — mais **la phrase qui la porte
   se met à la langue du client**. Deux cas réels du fichier :
   `"\n[CDB] HTTP %u — nouvelles tentatives en cours…\n"` (3711) ne bouge ni
   `[CDB] ` ni `HTTP`, et sa prose se traduit ; `"file_hash obsolete : le
   fichier n'est plus celui qui a ete confirme…"` (2722) garde `file_hash`
   dans le msgid anglais et rend « hash de fichier obsolète » en français.
   **Limite assumée du « tout se traduit » de F** : le *nom* d'un champ se
   francise dans la phrase (`path manquant.` → « chemin manquant. »,
   `block_hash requis` → « hash de bloc requis »), le **protocole** (libellés
   `clé: valeur`, noms passés en argument, `binary: yes`) reste en anglais.
   Les quatre hash se distinguent à l'écrit — « hash de bloc », « d'avant »,
   « d'après », « de fichier » — sinon quatre erreurs différentes portent le
   même mot. Cette frontière ne se décide pas au motif mais à la lecture :
   il faut savoir si la chaîne est **écrite** par le modèle ou seulement
   **lue** par lui.
10. **Les unités de mesure ne sont pas de la prose — pas de clé de traduction.**
    Décision d'Éric, Jalon F, à propos de `llm_slots_size_str` (`o`, `Kio`,
    `Mio`). Le réflexe à interdire est celui qui m'a fait commettre la faute :
    « la chaîne est affichée, donc `_()` ». Un symbole d'unité est une norme, pas
    une phrase. Trois contrôles, dans cet ordre, avant qu'une taille soit
    traduisible :

    1. **Le diviseur dit le préfixe.** `1024` ⇒ multiple **binaire** : `Ki`/`Mi`
       (kibi, mébi). `1000` ⇒ `k`/`M`. En français l'unité racine est l'**octet**,
       donc les symboles sont `o`, `Kio`, `Mio`, `Gio` — **jamais** `Ko`/`Mo` ni
       `KB`/`MB`, qui valent 1000. Confondre les deux n'est pas une faute de
       langue mais une **erreur d'information d'un facteur 1,024** ; le code la
       portait avant ce jalon, dans les deux langues.
    2. **Le séparateur décimal n'est pas une chaîne.** Il vient de `LC_NUMERIC`,
       posé par `setlocale(LC_ALL, "")` dans `i18n_init` (`i18n.c:44`). Sonde
       compilée sur ce poste : `[1.5]` en `LANG=C`, `[1,5]` en `fr_CA.UTF-8`.
       Une clé de traduction ne peut rien pour lui, et n'a rien à y faire.
    3. **Ce qui reste après 1 et 2 n'est pas une langue.** La seule divergence
       entre `%zu B` et `%zu o` était la racine du mot — *byte* contre *octet* —
       donc un choix de métrologie, tranché par Éric : l'octet. Une clé dont le
       seul travail est d'écrire « byte » là où l'on écrit « octet » ne traduit
       rien : elle inscrit une préférence dans le catalogue.

    Application : les msgids `%zu B`, `%.1f KB`, `%.1f MB` ont été **retirés** du
    `.pot` et du `fr.po` (300 → 297 msgids) et le code rend les symboles en dur.
    Limite de la règle, pour qu'elle ne devienne pas un interdit bête : une unité
    qui changerait de **nom** et non de symbole selon la langue — « octet » écrit
    en toutes lettres face à « byte » — reste de la prose et se marque. C'est le
    **symbole** qui est hors périmètre, pas le mot quand il est du texte.

    **Prolongement décidé par Éric (Jalon F)** : le mot retenu est **octet**, y
    compris dans les msgids **anglais** — « byte » est écarté partout. Trois sites
    touchés : `llmcore.c:4240` et `:4589` (les descriptions de `cdb_read` et de la
    policy disent « exact octets of the range ») et l'étiquette de protocole
    `bytes:` du résultat de `cdb_delete`, devenue `octets:`. Ce dernier changement
    est permis parce que la règle 9 distingue les deux sortes d'étiquettes :
    `bytes:` est seulement **lue** par le modèle, alors que `file_hash:` est
    **rejouée** atome pour atome dans les appels — les secondes restent en anglais.
    Aucun des msgids concernés ne porte de mot à traduire en plus : le fait que
    « octet » s'écrive identiquement en français et en anglais rend la clé
    inutile, ce qui rejoint le point 3 ci-dessus.

11. **Le sélecteur de langue — quatre règles qui ne sont pas du goût mais de
    l'honnêteté** (Jalon G).

    a) **Les noms de langues ne se traduisent pas.** « English », « Français »,
       « Español » s'affichent dans leur propre langue (*endonymes*), hors
       gettext. Décision d'Éric : « Je vois "English" dans mon interface même si
       je suis français. C'est un principe de secours pour l'utilisateur final
       qui cherche "English" ou le mot naturel dans sa langue. » Traduire ces
       noms les rend inutiles précisément dans le cas où ils servent. Un code
       hors table affiche son code ISO — jamais une devinette.

    b) **La liste vient du disque, pas du code.** Le sélecteur énumère
       `<dir du binaire>/po/locale/<code>/LC_MESSAGES/cdb.mo` : ajouter une
       langue = `make po && make mo`, aucun `if` à écrire.

    c) **Une langue sans locale installable ne se propose pas au silence.**
       `setlocale` refuse « en » et « fr » sur ce poste (vérifié : `ECHEC` ;
       seules les formes complètes `en_US.UTF-8`, `fr_CA.UTF-8` passent, et
       `fr.UTF-8` échoue aussi). D'où l'escalier de candidats de `i18n_apply()`
       et, si aucun ne passe : un message à l'écran **au lieu d'un
       enregistrement** — on n'écrit pas une préférence qui ne marchera pas.

    d) **Le territoire survit au changement de langue.** `fr_CA.UTF-8` + choix
       « English » ⇒ `en_CA.UTF-8` **avant** `en_US.UTF-8`. On change de langue,
       pas de pays : dates, heures et devises restent celles du poste. Mesuré à
       la sonde : `locale=en_CA.UTF-8`, date `Wed 31 Dec 1969 07:00:00 PM`.

    **Étendue de l'application live** : le changement prend effet aussitôt sur
    tout ce qui se construit après — tuile nouvelle, dialogue, Réglages
    rouverte, et surtout le prompt et les schémas d'outils reconstitués **à
    chaque requête**, donc la langue que reçoit le modèle bascule
    immédiatement. Les widgets déjà montés gardent leurs étiquettes et le texte
    d'aide le dit : un écran à moitié retraduit ment plus qu'un redémarrage. Ce
    n'est pas une paresse de catalogue, c'est la nature d'un label GTK, créé
    une fois.

    **Persistance** : membre `"language"` de `layout.json`, et **pas un fichier
    de plus** (décision d'Éric, §6.12). L'absence de clé vaut « suivre
    l'environnement » — ce qui est différent d'une valeur vide, et c'est pour
    cela que la ligne « Système » écrit une clé absente et non `"language": ""`.

12. **Le propriétaire d'un fichier préserve ce qu'il ne comprend pas.**
    `layout.json` est réécrit en entier par `layout_save()` depuis l'arbre des
    tuiles : y déposer une langue sans corriger l'écriture, c'était la voir
    effacée à la première division de tuile, sans un message. Les écritures sont
    donc passées en lecture-modification-écriture, `layout_merge_members()` a
    été ajouté pour qu'un seul module sache écrire ce fichier — et la règle est
    vérifiée dans les deux sens par une sonde (l'arbre ne perd plus la langue,
    la langue ne perd plus l'arbre).

    Consignée au passage, la source du litige : `window.json` est né du commit
    `11feed6` **sans marque de décision**, dans un dépôt où les choix validés
    sont écrits. Son état (taille, maximisé, fullscreen) est de l'affichage, il
    vit maintenant sous le membre `"window"` de `layout.json`, migré une fois
    puis l'ancien fichier supprimé — supprimé seulement après relecture de la
    copie.

    Le second logement, `roots.json` dans `llm.json`, a réussi pour une raison
    différente — et c'est la mesure qui l'a dite, pas la lecture : `llm.json`
    est écrit par **sept** points (cinq dans `llmcore.c`, deux dans
    `llmtoolpref.c`), et les sept mutent déjà la COPIE de l'objet relu. La règle
    y était donc respectée avant que je n'y pose quoi que ce soit. Mes deux
    premières regex m'avaient fait conclure l'inverse : elles comptaient « objet
    reconstruit » à la simple présence d'un `json_object_new()`, qui se
    trouvait être la branche de repli quand le fichier ne se parse pas — et ma
    seconde version soumettait au dictionnaire les lignes de code plutôt que les
    seuls commentaires, ce qui noyait 12 fautes réelles sous 184 mots légitimes.
    Vérifié à l'exécution ensuite, par une sonde qui APPELLE les sept écrivains
    un par un : après chacun, `roots`, `last_file` et la clé API se relisent
    intacts.

    Le déplacement a corrigé un défaut au passage : `roots_save()` écrivait son
    fichier EN ENTIER avec la seule clé `roots`, ce qui effaçait `last_file` à
    chaque ajout ou retrait de dossier — alors que `roots_write_last_file()`,
    sur le MÊME fichier, relisait avant d'écrire. Deux règles, un fichier, et
    une perte silencieuse. La fusion ne laisse plus ce choix.

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
