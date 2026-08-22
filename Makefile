CC      := gcc
CFLAGS  := -std=c23 -Wall -Wextra -O2 -g
PKGS    := gtk4 gtksourceview-5 libadwaita-1 json-glib-1.0 vte-2.91-gtk4 libsoup-3.0
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LIBS    := $(shell pkg-config --libs $(PKGS))

SRC     := src/main.c src/roots.c src/fslist.c src/dirty.c src/diffbar.c src/bashpanel.c src/modal.c src/session.c src/llm.c src/mdview.c src/layout.c
OBJ     := $(SRC:.c=.o)
TARGET  := cdb

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

# Build avec AddressSanitizer + UBSan (debug de corruption mémoire).
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address,undefined -O0 -fno-omit-frame-pointer" \
	            LDFLAGS="-fsanitize=address,undefined" \
	            LIBS="$(LIBS) -fsanitize=address,undefined"

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all run asan clean