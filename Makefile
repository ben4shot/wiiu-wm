CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-format-truncation -std=c11 -D_GNU_SOURCE
PKG_CFLAGS := $(shell pkg-config --cflags wayland-client cairo json-c)
PKG_LIBS := $(shell pkg-config --libs wayland-client cairo json-c) -lm -ldl -pthread

PROTO_DIR := protocols
SRC_DIR := src
BIN := wiiu-shell
TEST_BIN := test_shell

PROTOCOLS := xdg-shell wlr-foreign-toplevel-management-unstable-v1
PROTO_HEADERS := $(PROTOCOLS:%=$(PROTO_DIR)/%-client-protocol.h)
PROTO_SOURCES := $(PROTOCOLS:%=$(PROTO_DIR)/%-protocol.c)

SOURCES := $(filter-out $(SRC_DIR)/test_shell.c, $(wildcard $(SRC_DIR)/*.c)) $(PROTO_SOURCES)
OBJECTS := $(SOURCES:.c=.o)

TEST_SOURCES := $(SRC_DIR)/test_shell.c $(SRC_DIR)/config.c $(SRC_DIR)/draw_shapes.c \
                $(SRC_DIR)/touch_gestures.c $(SRC_DIR)/app_manager.c $(SRC_DIR)/grid_view.c \
                $(SRC_DIR)/recents_view.c $(SRC_DIR)/input_gamepad.c \
                $(SRC_DIR)/icons.c $(SRC_DIR)/editor.c $(SRC_DIR)/poly.c \
                $(SRC_DIR)/images.c $(SRC_DIR)/page_cache.c
TEST_OBJECTS := $(TEST_SOURCES:.c=.o)

# ------------------------------------------------------------ wiiu-wm (compositor propio)
WM_BIN := wiiu-wm
WLR_PC := $(shell pkg-config --exists wlroots-0.20 && echo wlroots-0.20)
ifeq ($(WLR_PC),)
# Sin libwlroots-0.20-dev instalado: cabeceras extraídas del .deb en build-deps/ (ver README)
WLR_CFLAGS := -Ibuild-deps/include/wlroots-0.20 -Ibuild-deps/include/libdrm -Ibuild-deps/include
WLR_LIBS := -lwlroots-0.20
else
WLR_CFLAGS := $(shell pkg-config --cflags wlroots-0.20)
WLR_LIBS := $(shell pkg-config --libs wlroots-0.20)
endif
WM_CFLAGS := -DWLR_USE_UNSTABLE $(WLR_CFLAGS) $(shell pkg-config --cflags wayland-server pixman-1 xkbcommon xcb xcb-ewmh xcb-icccm 2>/dev/null)
WM_LIBS := $(WLR_LIBS) $(shell pkg-config --libs wayland-server xkbcommon pixman-1 xcb xcb-ewmh xcb-icccm 2>/dev/null) $(PKG_LIBS) -l:libEGL.so.1 -l:libGLESv2.so.2 -ldl
WM_PROTO := $(PROTO_DIR)/wlr-layer-shell-unstable-v1-protocol.h
WM_SOURCES := $(wildcard $(SRC_DIR)/wm/*.c)
WM_OBJECTS := $(WM_SOURCES:.c=.o)
WM_SHARED := $(SRC_DIR)/config.o $(SRC_DIR)/draw_shapes.o $(SRC_DIR)/grid_view.o $(SRC_DIR)/recents_view.o \
             $(SRC_DIR)/input_gamepad.o $(SRC_DIR)/icons.o $(SRC_DIR)/images.o $(SRC_DIR)/page_cache.o \
             $(SRC_DIR)/poly.o $(SRC_DIR)/app_manager.o

all: $(BIN)

wm: $(WM_BIN)

# Solo compilar (sin enlazar): sirve en un PC sin la biblioteca wlroots 0.20
wm-check: $(WM_OBJECTS)

$(PROTO_DIR)/%-protocol.h: $(PROTO_DIR)/%.xml
	wayland-scanner server-header $< $@

$(WM_OBJECTS): | $(WM_PROTO) $(PROTO_HEADERS)

$(SRC_DIR)/wm/%.o: $(SRC_DIR)/wm/%.c
	$(CC) $(CFLAGS) -MMD -MP $(WM_CFLAGS) $(PKG_CFLAGS) -I$(PROTO_DIR) -I$(SRC_DIR) -c $< -o $@

$(WM_BIN): $(WM_OBJECTS) $(WM_SHARED)
	$(CC) $^ $(WM_LIBS) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_OBJECTS)
	$(CC) $(TEST_OBJECTS) $(PKG_LIBS) -o $@

$(PROTO_DIR)/%-client-protocol.h: $(PROTO_DIR)/%.xml
	wayland-scanner client-header $< $@

$(PROTO_DIR)/%-protocol.c: $(PROTO_DIR)/%.xml
	wayland-scanner private-code $< $@

# Las cabeceras de protocolo deben existir antes de compilar cualquier objeto
$(OBJECTS) $(TEST_OBJECTS): | $(PROTO_HEADERS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP $(PKG_CFLAGS) -I$(PROTO_DIR) -I$(SRC_DIR) -c $< -o $@

$(BIN): $(OBJECTS)
	$(CC) $(OBJECTS) $(PKG_LIBS) -o $@

-include $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(WM_OBJECTS:.o=.d)

clean:
	rm -f $(SRC_DIR)/*.o $(SRC_DIR)/*.d $(SRC_DIR)/wm/*.o $(SRC_DIR)/wm/*.d $(PROTO_DIR)/*.o $(PROTO_DIR)/*.d \
	      $(BIN) $(TEST_BIN) $(WM_BIN) $(WM_PROTO) \
	      $(PROTO_HEADERS) $(PROTO_SOURCES) test_*.png

.PHONY: all clean test wm wm-check
