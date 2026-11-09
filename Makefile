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
GUI_SRC := src/gui.c src/db.c src/profiles.c src/macos_menu.m src/splitview.m src/segmented.m \
           src/export.c src/export_db.c src/export_dialog.m src/table_menu.m \
           src/ai.c src/ai_db.c src/json.c src/row_edit.c src/grid_edit.m \
           src/sql_tips.c src/loading.m
GUI_HDR := src/db.h src/profiles.h src/macos_menu.h src/splitview.h src/segmented.h \
           src/export.h src/export_db.h src/export_dialog.h src/table_menu.h \
           src/ai.h src/ai_db.h src/json.h src/row_edit.h src/grid_edit.h \
           src/sql_tips.h src/loading.h
GUI_LIBS := $(LIBUI_LIB) -framework Foundation -framework AppKit -lobjc -lcurl

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

# Unit tests: saved-connections file format, export writers, the AI request /
# response handling, the row-editing SQL and the SQL tips (no database or network needed).
TEST_CFLAGS := -std=c11 -Wall -Wextra -g -O0 -D_DARWIN_C_SOURCE -fsanitize=address,undefined -Isrc

test: tests/profiles_test.c src/profiles.c src/profiles.h tests/export_test.c src/export.c src/export.h \
      tests/ai_test.c src/ai.c src/ai.h src/json.c src/json.h \
      tests/row_edit_test.c src/row_edit.c src/row_edit.h \
      tests/sql_tips_test.c src/sql_tips.c src/sql_tips.h
	@mkdir -p $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $(BUILD)/profiles_test tests/profiles_test.c src/profiles.c
	$(BUILD)/profiles_test $(BUILD)/test-profiles
	$(CC) $(TEST_CFLAGS) -o $(BUILD)/export_test tests/export_test.c src/export.c
	$(BUILD)/export_test
	$(CC) $(TEST_CFLAGS) -o $(BUILD)/ai_test tests/ai_test.c src/ai.c src/json.c -lcurl
	$(BUILD)/ai_test
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -o $(BUILD)/row_edit_test tests/row_edit_test.c src/row_edit.c
	$(BUILD)/row_edit_test
	$(CC) $(TEST_CFLAGS) -o $(BUILD)/sql_tips_test tests/sql_tips_test.c src/sql_tips.c
	$(BUILD)/sql_tips_test

# Export round trip against a real server: SQL re-import, XML and CSV checks.
# Creates and drops the databases test_sqlc_export_src and test_sqlc_export_dst.
export-test: tests/export_db_test.c tests/export_roundtrip.sh src/export.c src/export_db.c src/db.c
	@mkdir -p $(BUILD)
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -o $(BUILD)/export_db_test tests/export_db_test.c \
		src/export.c src/export_db.c src/db.c $(LDLIBS)
	sh tests/export_roundtrip.sh $(BUILD)/export_db_test $(BUILD)/export-test

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

.PHONY: all gui gui-test test export-test clean distclean debug compile_flags.txt
