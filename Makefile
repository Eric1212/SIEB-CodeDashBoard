CC      := gcc
CFLAGS  := -std=c23 -Wall -Wextra -O2 -g
# Suivi des dépendances de headers : chaque .o génère son .d, ré-inclus
# plus bas — modifier un .h recompile les .o dépendants (fini les stale
# objects silencieux après une édition d'en-tête).
CFLAGS  += -MMD -MP
PKGS    := gtk4 gtksourceview-5 libadwaita-1 json-glib-1.0 vte-2.91-gtk4 libsoup-3.0
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LIBS    := $(shell pkg-config --libs $(PKGS))

SRC     := src/main.c src/css.c src/roots.c src/fslist.c src/dirty.c src/diffbar.c src/bashpanel.c src/modal.c src/session.c src/llmcore.c src/llmtile.c src/mdview.c src/layout.c src/llmslots.c src/llmlive.c src/llmtoolpref.c src/ibox.c
OBJ     := $(SRC:.c=.o)
DEP     := $(OBJ:.o=.d)
TARGET  := cdb

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

# Test unitaire de la boucle agentique : les predicats du bouton (alive /
# rolling), sans GTK, sans reseau et sans clic. Le test fournit son propre
# main(), donc on lie tous les objets SAUF main.o.
TEST_SRC  := tests/agent_state.c
TEST_OBJ  := $(TEST_SRC:.c=.o)
APP_OBJ   := $(filter-out src/main.o,$(OBJ))

test: $(TEST_OBJ) $(APP_OBJ)
	$(CC) $(CFLAGS) -o tests/agent_state $(APP_OBJ) $(TEST_OBJ) $(LIBS) $(LDFLAGS)
	./tests/agent_state

# Build avec AddressSanitizer + UBSan (debug de corruption mémoire).
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address,undefined -O0 -fno-omit-frame-pointer" \
	            LDFLAGS="-fsanitize=address,undefined" \
	            LIBS="$(LIBS) -fsanitize=address,undefined"

clean:
	rm -f $(OBJ) $(DEP) $(TARGET) $(TEST_OBJ) $(TEST_OBJ:.o=.d) tests/agent_state

# Dépendances de headers générées par -MMD (ignorées si absentes).
-include $(DEP)

.PHONY: all run test asan clean
