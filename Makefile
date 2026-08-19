CC      := gcc
CFLAGS  := -std=c23 -Wall -Wextra -O2 -g
PKGS    := gtk4 gtksourceview-5 libadwaita-1 json-glib-1.0
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LIBS    := $(shell pkg-config --libs $(PKGS))

SRC     := src/main.c src/roots.c src/fslist.c src/dirty.c src/diffbar.c src/layout.c
OBJ     := $(SRC:.c=.o)
TARGET  := siebcdashboard

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all run clean