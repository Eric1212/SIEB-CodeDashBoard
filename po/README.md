# Traductions de CDB

CDB est traduit avec **gettext**. La langue source (msgid) est l'**anglais**.

## Fichiers

- `POTFILES.in` — liste des sources scannées par `xgettext`.
- `LINGUAS` — langues prises en charge (une par ligne).
- `cdb.pot` — gabarit généré (ne pas éditer à la main).
- `<lang>.po` — traductions (une par langue, ex. `fr.po`, `en.po`).
- `locale/` — catalogues compilés `.mo` (générés, ignorés par git).

## Commandes (depuis la racine du projet)

```sh
make pot     # régénère po/cdb.pot depuis les sources marquées _()/N_()
make po      # met à jour les .po existants depuis le .pot (msgmerge)
make mo      # compile po/locale/<lang>/LC_MESSAGES/cdb.mo (msgfmt)
```

## Ajouter une langue

1. Ajouter son code à `LINGUAS` (ex. `es`).
2. `msginit --locale=es --input=po/cdb.pot --output=po/es.po`
3. Traduire `po/es.po` (Poedit, ou un éditeur texte).
4. `make mo` puis `LANG=es_ES.UTF-8 ./cdb` pour tester.

## Règles

- Marquer les chaînes visibles avec `_()` ; pluriels avec `ngettext()`.
- Ne **pas** traduire : clés JSON, noms de thèmes, CSS, chaînes techniques.
- Conserver les marqueurs de format (`%s`, `%d`, `%1$s`…).
- Voir `docs/I18N_PLAN.md` §5 pour le détail.
