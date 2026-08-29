# CDB translations

CDB is translated with **gettext**. The source language (`msgid`) is
**English**. This file, the template and the `TRANSLATORS:` comments are
written in English on purpose: they are the help shown to whoever translates
into any language. `fr.po` keeps its own header in French, since the help on a
French translation page should be readable in French.

## Files

- `POTFILES.in` — the sources scanned by `xgettext`.
- `LINGUAS` — the languages built and shipped (one per line).
- `cdb.pot` — generated template (never edit by hand).
- `<lang>.po` — translations (one per language, e.g. `fr.po`, `en.po`).
- `locale/` — compiled `.mo` catalogs (generated, ignored by git).

## Commands (from the project root)

```sh
make pot     # regenerate po/cdb.pot from the _()/N_() marked sources
make po      # update the .po files from the .pot (msgmerge; msginit for a
             # language listed in LINGUAS but not yet present)
make mo      # compile po/locale/<lang>/LC_MESSAGES/cdb.mo (msgfmt)
make i18n-check   # valid syntax AND zero fuzzy entries
```

## Adding a language

1. Add its code to `LINGUAS` (e.g. `es`).
2. `make po` — this creates `po/es.po` for you. To do it by hand instead:
   `msginit --locale=es --input=po/cdb.pot --output=po/es.po`
3. Translate `po/es.po` (Poedit, or a text editor).
4. `make mo`, then `LANG=es_ES.UTF-8 ./cdb` to see it. The language picker
   reads the catalogs from disk, so no C code changes.
5. The system locale must also exist (`locale -a | grep es`), otherwise the
   strings are translated but the dates and numbers are not.

## Rules

- Mark human-visible strings with `_()`; plurals with `ngettext()`, never
  `? "s" : ""`.
- Do **not** translate: JSON keys, action names (`win.new-window`), CSS, icon
  names, tokens compared in code, and strings with no word to translate.
- Keep the format specifiers (`%s`, `%d`, `%1$s`…).
- A comment meant for the translator must start with `TRANSLATORS:` — that is
  what makes `xgettext` copy it into the catalogs. Ordinary code comments stay
  in French, like the rest of the sources.
- See `docs/I18N_PLAN.md` §6 for the full marking rules.
