CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CPPFLAGS += -D_DARWIN_C_SOURCE $(shell mariadb_config --cflags)
LDLIBS  += $(shell mariadb_config --libs)

BUILD := build

# ---- CLI: sqlc ----
BIN := sqlc
SRC := src/main.c src/complete.c src/db.c
HDR := src/complete.h src/db.h

all: $(BIN) gui

$(BIN): $(SRC) $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS) -ledit

# ---- libui-ng (vendored submodule), built as a static library ----
# Compiled directly with clang, so meson/ninja aren't needed.
LIBUI_DIR  := third_party/libui-ng
LIBUI_SRC  := $(filter-out %/OLD_table.c,$(wildcard $(LIBUI_DIR)/common/*.c)) \
              $(filter-out %/OLD_table.m,$(wildcard $(LIBUI_DIR)/darwin/*.m))
LIBUI_OBJ  := $(patsubst $(LIBUI_DIR)/%,$(BUILD)/libui/%.o,$(LIBUI_SRC))
LIBUI_LIB  := $(BUILD)/libui.a
LIBUI_FLAGS := -O2 -w -I$(LIBUI_DIR) -mmacosx-version-min=10.13

$(LIBUI_DIR)/ui.h:
	git submodule update --init $(LIBUI_DIR)

$(BUILD)/libui/%.o: $(LIBUI_DIR)/% | $(LIBUI_DIR)/ui.h
	@mkdir -p $(dir $@)
	$(CC) $(LIBUI_FLAGS) -c $< -o $@

$(LIBUI_LIB): $(LIBUI_OBJ)
	rm -f $@
	ar rcs $@ $^

# ---- GUI: sqlc-gui ----
GUI_BIN := sqlc-gui
GUI_SRC := src/gui.c src/db.c src/profiles.c src/keychain.c src/macos_menu.m
GUI_HDR := src/db.h src/profiles.h src/keychain.h src/macos_menu.h
GUI_LIBS := $(LIBUI_LIB) -framework Foundation -framework AppKit -framework Security -lobjc

gui: $(GUI_BIN)

$(GUI_BIN): $(GUI_SRC) $(GUI_HDR) $(LIBUI_LIB)
	$(CC) $(CPPFLAGS) -I$(LIBUI_DIR) $(CFLAGS) -o $@ $(GUI_SRC) $(LDFLAGS) $(GUI_LIBS) $(LDLIBS)

# ---- dev helpers ----
# Scripted GUI smoke test with sanitizers; needs a database with tables
# customers, orders and a view (see README).
gui-test: $(GUI_SRC) $(GUI_HDR) $(LIBUI_LIB)
	$(CC) $(CPPFLAGS) -I$(LIBUI_DIR) -std=c11 -Wall -Wextra -g -O0 -fsanitize=address,undefined \
		-DSQLC_AUTOTEST -o $(BUILD)/sqlc-gui-test $(GUI_SRC) $(GUI_LIBS) $(LDLIBS)
	SQLC_CONFIG_DIR=$(BUILD)/test-config SQLC_TEST_DB=$${SQLC_TEST_DB:-test_sqlc_demo} \
		$(BUILD)/sqlc-gui-test

# Unit test for the saved-connections file format (no database needed).
test: tests/profiles_test.c src/profiles.c src/profiles.h
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -g -O0 -D_DARWIN_C_SOURCE -fsanitize=address,undefined -Isrc \
		-o $(BUILD)/profiles_test tests/profiles_test.c src/profiles.c
	$(BUILD)/profiles_test $(BUILD)/test-profiles

debug: CFLAGS = -std=c11 -Wall -Wextra -g -O0 -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean $(BIN) $(GUI_BIN)

# Editor support (clangd): writes the include paths for this machine.
compile_flags.txt:
	printf "%s\n" -std=c11 -D_DARWIN_C_SOURCE -I$(LIBUI_DIR) $(shell mariadb_config --cflags) | tr " " "\n" > $@

clean:
	rm -f $(BIN) $(GUI_BIN)

distclean: clean
	rm -rf $(BUILD)

.PHONY: all gui gui-test test clean distclean debug compile_flags.txt
