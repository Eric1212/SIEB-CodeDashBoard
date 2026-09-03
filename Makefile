CC      := gcc
CFLAGS  := -std=c23 -Wall -Wextra -O2 -g
# Suivi des dépendances de headers : chaque .o génère son .d, ré-inclus
# plus bas — modifier un .h recompile les .o dépendants (fini les stale
# objects silencieux après une édition d'en-tête).
CFLAGS  += -MMD -MP
PKGS    := gtk4 gtksourceview-5 libadwaita-1 json-glib-1.0 vte-2.91-gtk4 libsoup-3.0
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LIBS    := $(shell pkg-config --libs $(PKGS))

SRC     := src/main.c src/css.c src/roots.c src/fslist.c src/dirty.c src/diffbar.c src/bashpanel.c src/modal.c src/session.c src/llmcore.c src/llmtile.c src/mdview.c src/layout.c src/llmslots.c src/llmlive.c src/llmtoolpref.c src/llmeffort.c src/llmtrim.c src/ibox.c src/i18n.c src/mem.c src/sfx.c src/textops.c
OBJ     := $(SRC:.c=.o)
DEP     := $(OBJ:.o=.d)
TARGET  := cdb

# Les catalogues .mo font partie du build normal : sans eux CDB tombe
# silencieusement sur les msgid (anglais pivot), et un utilisateur francophone
# verrait son interface passer en anglais après un `make clean`. `mo` est
# phony mais dépend des fichiers .mo : msgfmt ne relance qu'au besoin.
all: $(TARGET) mo tools

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET) mo
	./$(TARGET)

# --- Deps vendorées : checker + updater -------------------------------
# refresh_third_party lit third_party/manifest.toml et compare chaque pin au
# HEAD amont. Il se moque de GTK et de libsoup : flags dédiés, sinon -MMD lui
# accoucherait d'un .d orphelin et --cflags gtk irait chercher des headers
# qu'il ne regarde pas.
TOOLS     := tools/refresh_third_party
TOOLFLAGS := -std=c23 -O1 -Wall -Wextra
TOOLDEPS  := tools/refresh_third_party.c third_party/tomlc17/tomlc17.c \
             third_party/tomlc17/tomlc17.h

tools: $(TOOLS)

$(TOOLS): $(TOOLDEPS)
	$(CC) $(TOOLFLAGS) -I third_party/tomlc17 -o $@ tools/refresh_third_party.c third_party/tomlc17/tomlc17.c

# --- Couche pure : test hors application ------------------------------
# textops.c ne depend que de glib : ni GTK, ni vue, ni disque. Meme lecon
# que pour refresh_third_party : flags dedies sans -MMD, sinon la regle
# generale accoucherait d'un .d orphelin et irait reclamer des headers
# qu'elle ne regarde pas. Le binaire vit dans tools/ et n'entre PAS dans
# `all` : `make` compile, `make check` prouve.
TESTBIN     := tools/test_textops
TESTFLAGS   := -std=c23 -O1 -g -Wall -Wextra
TESTDEPS    := src/textops.c src/textops.h tools/test_textops.c
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
GLIB_LIBS   := $(shell pkg-config --libs glib-2.0)

check: $(TESTBIN)
	./$(TESTBIN)

$(TESTBIN): $(TESTDEPS)
	$(CC) $(TESTFLAGS) $(GLIB_CFLAGS) -o $@ src/textops.c tools/test_textops.c $(GLIB_LIBS)

# Le meme test sous ASan/UBSan : la couche pure manipule des offsets,
# c'est exactement la ou une erreur de bornes se cache sans se voir.
check-asan:
	$(CC) $(TESTFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $(GLIB_CFLAGS) -o $(TESTBIN) src/textops.c tools/test_textops.c \
	    $(GLIB_LIBS) -fsanitize=address,undefined
	./$(TESTBIN)

# .git/hooks/ n'est pas versionné : le hook vit dans tools/git-hooks/ et ce
# script le copie en le rendant exécutable. À lancer après un fresh clone.
install-hooks:
	./tools/install_git_hooks.sh

# Build avec AddressSanitizer + UBSan (debug de corruption mémoire).
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address,undefined -O0 -fno-omit-frame-pointer" \
	            LDFLAGS="-fsanitize=address,undefined" \
	            LIBS="$(LIBS) -fsanitize=address,undefined"


# --- Internationalisation (gettext) -------------------------------------
# Langue source (msgid) = anglais. Catalogues compilés dans po/locale/,
# chargés au runtime relativement au binaire (voir src/i18n.c) : `make run`
# fonctionne sans rien installer. Détail : docs/I18N_PLAN.md.
XGETTEXT := xgettext
MSGFMT   := msgfmt
MSGMERGE := msgmerge
MSGINIT  := msginit
PODIR    := po
LOCALEDIR:= $(PODIR)/locale
POT      := $(PODIR)/cdb.pot
# Année courante AU MOMENT DU BUILD, jamais gravée en dur : les en-têtes
# de copyright (POT de repli, en-têtes des .po) suivent le calendrier.
# Le POT de xgettext, lui, garde « YEAR » littéral — convention GNU : le
# gabarit est un template, msginit remplit l'année à l'initialisation.
YEAR     := $(shell date +%Y)
LANGS    := $(shell grep -v '^\#' $(PODIR)/LINGUAS 2>/dev/null)
POS      := $(addprefix $(PODIR)/,$(addsuffix .po,$(LANGS)))
MOS      := $(foreach l,$(LANGS),$(LOCALEDIR)/$(l)/LC_MESSAGES/cdb.mo)

# Extraction : régénère le gabarit cdb.pot depuis les sources de POTFILES.in.
# xgettext tourne depuis la RACINE : on convertit les chemins « ../src/x »
# de POTFILES.in en « src/x » (sed), ce qui évite toute ambiguïté de -D.
# GNU gettext >= 0.21 ne pose AUCUN fichier quand aucune chaîne n'est
# marquée : on écrit alors un POT d'en-tête valide, pour que `make po` et
# `make mo` restent démontrables avant le premier marquage.
pot:
	sed 's|^\.\./||' $(PODIR)/POTFILES.in | grep -v '^\#' | grep . > $(PODIR)/.potfiles
	rm -f $(POT)
	$(XGETTEXT) --from-code=UTF-8 --keyword=_ --keyword=N_:1 \
	    --keyword=ngettext:1,2 --add-comments=TRANSLATORS \
	    --package-name=cdb --package-version=0.1 \
	    --copyright-holder="Éric Boucher (SIEB)" \
	    --files-from=$(PODIR)/.potfiles --output=$(POT)
	rm -f $(PODIR)/.potfiles
	@if [ ! -f $(POT) ]; then \
	    echo "  XGETTEXT  aucune chaîne marquée : POT d'en-tête seul"; \
	    printf '%s\n' \
	        '# CodeDashBoard translation template.' \
	        '# Copyright (C) $(YEAR) Éric Boucher (SIEB).' \
	        '#' \
	        'msgid ""' \
	        'msgstr ""' \
	        '"Project-Id-Version: cdb 0.1\n"' \
	        '"Report-Msgid-Bugs-To: \n"' \
	        '"POT-Creation-Date: $(shell date -u '+%Y-%m-%d %H:%M+0000')\n"' \
	        '"PO-Revision-Date: YEAR-MO-DA HO:MI+ZONE\n"' \
	        '"Last-Translator: \n"' \
	        '"Language-Team: \n"' \
	        '"Language: \n"' \
	        '"MIME-Version: 1.0\n"' \
	        '"Content-Type: text/plain; charset=UTF-8\n"' \
	        '"Content-Transfer-Encoding: 8bit\n"' > $(POT); \
	fi


# Initialisation d'un .po manquant, puis mise à jour depuis le .pot.
#
# --no-fuzzy-matching est un choix, pas un réglage de confort : msgmerge
# marie les msgid par similarité lexicale et a produit, sur deux jalons
# consécutifs, des traductions FAUSSES — "Cannot delete." -> « Impossible
# de créer le fichier. », "Delete folder" -> « Nouveau dossier », "Reload
# the projects tree" -> « Supprimer le projet ». Le drapeau fuzzy rend la
# faute inoffensive à l'exécution (gettext ignore ces entrées) mais la
# laisse pré-remplie à qui ouvre le .po dans Poedit : un clic et elle est
# validée. Sans fuzzy, la chaîne reste vide et le msgid anglais s'affiche
# — un trou visible, jamais un contresens invisible.
po: pot
	@for l in $(LANGS); do \
	    if [ ! -f $(PODIR)/$$l.po ]; then \
	        $(MSGINIT) --no-translator --locale=$$l \
	            --input=$(POT) --output=$(PODIR)/$$l.po; \
	    fi; \
	    $(MSGMERGE) --update --no-fuzzy-matching --backup=none \
	        $(PODIR)/$$l.po $(POT); \
	    sed -i 's/^# Copyright (C) [0-9-]* \(Éric Boucher (SIEB)\)\.$$/# Copyright (C) $(YEAR) \1./' \
	        $(PODIR)/$$l.po; \
	done

# Compilation des catalogues binaires (.mo) dans po/locale/<lang>/LC_MESSAGES/.
mo: $(MOS)
$(LOCALEDIR)/%/LC_MESSAGES/cdb.mo: $(PODIR)/%.po
	@mkdir -p $(dir $@)
	$(MSGFMT) --check --statistics --output=$@ $<

# Garde-fou : chaque .po doit etre syntaxiquement valide ET sans entree
# « fuzzy ». msgfmt --check ne signale pas le fuzzy — un catalogue fuzzy
# compile tres bien — mais gettext IGNORE ces entrees a l'execution : une
# traduction fuzzy est une traduction morte, donc une regression muette.
# Cas reel attrape par msgmerge : "Rename:\n%s" marie a "Rename" (fuzzy),
# ce qui aurait rendu le corps du dialogue Renommage en anglais.
i18n-check:
	@fail=0; for f in $(POS); do \
	    [ -f "$$f" ] || continue; \
	    if $(MSGFMT) --check --output=/dev/null $$f; then \
	        n=$$(grep -c '^#,.*fuzzy' $$f); \
	        if [ $$n -ne 0 ]; then \
	            echo "ECHEC $$f : $$n entree(s) fuzzy, ignoree(s) a l'execution :"; \
	            grep -n '^#,.*fuzzy' $$f; \
	            fail=1; \
	        else \
	            echo "ok  $$f"; \
	        fi; \
	    else \
	        echo "ECHEC $$f (msgfmt --check)"; fail=1; \
	    fi; \
	done; exit $$fail

clean:
	rm -f $(OBJ) $(DEP) $(TARGET) $(TOOLS) $(TESTBIN)
	rm -rf $(LOCALEDIR)

# Dépendances de headers générées par -MMD (ignorées si absentes).
-include $(DEP)
.PHONY: all run asan clean pot po mo i18n-check tools install-hooks check check-asan
