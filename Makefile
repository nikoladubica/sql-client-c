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
# Portable C shared by the macOS and Windows GUIs.
GUI_C_SRC := src/gui.c src/db.c src/profiles.c src/export.c src/export_db.c \
           src/ai.c src/ai_db.c src/json.c src/row_edit.c src/sql_tips.c src/tabs.c \
           src/browse.c src/sidebar.c src/platform.c
GUI_C_HDR := src/db.h src/profiles.h src/export.h src/export_db.h src/ai.h src/ai_db.h \
           src/json.h src/row_edit.h src/sql_tips.h src/tabs.h src/browse.h src/sidebar.h \
           src/platform.h
# The AppKit shims (macOS); src/win/ has the Windows versions of the same headers.
SHIM_HDR := src/macos_menu.h src/splitview.h src/segmented.h src/export_dialog.h \
           src/table_menu.h src/grid_edit.h src/loading.h src/tabbar.h
GUI_SRC := $(GUI_C_SRC) src/macos_menu.m src/splitview.m src/segmented.m src/export_dialog.m \
           src/table_menu.m src/grid_edit.m src/loading.m src/tabbar.m
GUI_HDR := $(GUI_C_HDR) $(SHIM_HDR)
GUI_LIBS := $(LIBUI_LIB) -framework Foundation -framework AppKit -lobjc -lcurl

gui: $(GUI_BIN)

$(GUI_BIN): $(GUI_SRC) $(GUI_HDR) $(LIBUI_LIB)
	$(CC) $(CPPFLAGS) -I$(LIBUI_DIR) $(CFLAGS) -o $@ $(GUI_SRC) $(LDFLAGS) $(GUI_LIBS) $(LDLIBS)

# ---- Windows GUI: sqlc-gui.exe (MinGW-w64) ----
# `make windows` in an MSYS2 MINGW64 / UCRT64 shell (needs the mariadb-connector-c
# and curl packages, see README), or cross-compiled with x86_64-w64-mingw32-*
# given Windows builds of both. `make win-check` compiles everything for
# Windows without linking (no Windows libraries needed; uses this machine's
# MariaDB / curl headers when cross-compiling).
WIN_NATIVE := $(filter MINGW% MSYS%,$(shell uname -s))
WIN_PREFIX ?= $(if $(WIN_NATIVE),,x86_64-w64-mingw32-)
WIN_CC     := $(WIN_PREFIX)gcc
WIN_CXX    := $(WIN_PREFIX)g++
WIN_RC     := $(WIN_PREFIX)windres
WIN_BUILD  := $(BUILD)/win
WIN_BIN    := sqlc-gui.exe
WIN_SRC    := $(GUI_C_SRC) $(wildcard src/win/*.c)
WIN_HDR    := $(GUI_C_HDR) $(SHIM_HDR) $(wildcard src/win/*.h)
WIN_CURL_INC    ?= $(firstword $(wildcard $(shell brew --prefix 2>/dev/null)/opt/curl/include))
# MariaDB Connector/C: pkg-config's libmariadb.pc where the package has one, else mariadb_config.
WIN_DEPS_CFLAGS ?= $(if $(WIN_NATIVE),$(shell pkg-config --cflags libmariadb 2>/dev/null || mariadb_config --include) \
                   $(shell pkg-config --cflags libcurl),\
                   $(shell mariadb_config --include) $(if $(WIN_CURL_INC),-I$(WIN_CURL_INC)))
WIN_DEPS_LIBS   ?= $(shell pkg-config --libs libmariadb 2>/dev/null || mariadb_config --libs) \
                   $(shell pkg-config --libs libcurl)
WIN_CFLAGS := -std=c11 -Wall -Wextra -O2 -D_UI_STATIC -D__USE_MINGW_ANSI_STDIO=1 -D_GNU_SOURCE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
              -DUNICODE -D_UNICODE -Isrc -Isrc/win -I$(LIBUI_DIR)
WIN_SYSLIBS := -luser32 -lkernel32 -lgdi32 -lcomctl32 -luxtheme -lmsimg32 -lcomdlg32 -ld2d1 \
               -ldwrite -lole32 -loleaut32 -loleacc -luuid -lwindowscodecs -ladvapi32 \
               -lshell32

WIN_LIBUI_SRC := $(filter-out %/OLD_table.c,$(wildcard $(LIBUI_DIR)/common/*.c)) \
                 $(wildcard $(LIBUI_DIR)/windows/*.cpp)
WIN_LIBUI_OBJ := $(patsubst $(LIBUI_DIR)/%,$(WIN_BUILD)/libui/%.o,$(WIN_LIBUI_SRC))
WIN_LIBUI_LIB := $(WIN_BUILD)/libui.a
WIN_LIBUI_FLAGS := -O2 -w -D_UI_STATIC -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -I$(LIBUI_DIR)

$(WIN_BUILD)/libui/%.c.o: $(LIBUI_DIR)/%.c | $(LIBUI_DIR)/ui.h
	@mkdir -p $(dir $@)
	$(WIN_CC) $(WIN_LIBUI_FLAGS) -std=c11 -c $< -o $@

$(WIN_BUILD)/libui/%.cpp.o: $(LIBUI_DIR)/%.cpp | $(LIBUI_DIR)/ui.h
	@mkdir -p $(dir $@)
	$(WIN_CXX) $(WIN_LIBUI_FLAGS) -std=c++17 -c $< -o $@

$(WIN_LIBUI_LIB): $(WIN_LIBUI_OBJ)
	rm -f $@
	$(WIN_PREFIX)ar rcs $@ $^

$(WIN_BUILD)/sqlc_rc.o: src/win/sqlc.rc src/win/sqlc.manifest
	@mkdir -p $(WIN_BUILD)
	$(WIN_RC) -I src/win -O coff -i $< -o $@

WIN_OBJ := $(patsubst %.c,$(WIN_BUILD)/%.o,$(WIN_SRC))

$(WIN_BUILD)/src/%.o: src/%.c $(WIN_HDR)
	@mkdir -p $(dir $@)
	$(WIN_CC) $(WIN_CFLAGS) $(WIN_DEPS_CFLAGS) -c $< -o $@

windows: $(WIN_BIN)

# Linked with g++: libui-ng's Windows code is C++.
$(WIN_BIN): $(WIN_OBJ) $(WIN_LIBUI_LIB) $(WIN_BUILD)/sqlc_rc.o
	$(WIN_CXX) -o $@ $(WIN_OBJ) $(WIN_BUILD)/sqlc_rc.o -mwindows -static-libgcc -static-libstdc++ \
		$(WIN_LIBUI_LIB) $(WIN_DEPS_LIBS) $(WIN_SYSLIBS) -Wl,-Bstatic -lpthread -Wl,-Bdynamic

# The database-free unit tests that build with MinGW, as Windows programs:
# run natively (MSYS2, CI). No sanitizers there.
WIN_TEST_CFLAGS := -std=c11 -Wall -Wextra -g -O1 -D__USE_MINGW_ANSI_STDIO=1 -D_GNU_SOURCE -Isrc

win-test:
	@mkdir -p $(WIN_BUILD)
	$(WIN_CC) $(WIN_TEST_CFLAGS) -o $(WIN_BUILD)/platform_test.exe tests/platform_test.c src/platform.c
	$(WIN_BUILD)/platform_test.exe $(WIN_BUILD)/test-platform
	$(WIN_CC) $(WIN_TEST_CFLAGS) -o $(WIN_BUILD)/profiles_test.exe tests/profiles_test.c src/profiles.c src/platform.c
	$(WIN_BUILD)/profiles_test.exe $(WIN_BUILD)/test-profiles
	$(WIN_CC) $(WIN_TEST_CFLAGS) $(WIN_DEPS_CFLAGS) -o $(WIN_BUILD)/row_edit_test.exe tests/row_edit_test.c src/row_edit.c
	$(WIN_BUILD)/row_edit_test.exe
	$(WIN_CC) $(WIN_TEST_CFLAGS) $(WIN_DEPS_CFLAGS) -o $(WIN_BUILD)/browse_test.exe tests/browse_test.c src/browse.c
	$(WIN_BUILD)/browse_test.exe
	$(WIN_CC) $(WIN_TEST_CFLAGS) -o $(WIN_BUILD)/sql_tips_test.exe tests/sql_tips_test.c src/sql_tips.c
	$(WIN_BUILD)/sql_tips_test.exe
	$(WIN_CC) $(WIN_TEST_CFLAGS) -o $(WIN_BUILD)/tabs_test.exe tests/tabs_test.c src/tabs.c
	$(WIN_BUILD)/tabs_test.exe
	$(WIN_CC) $(WIN_TEST_CFLAGS) -o $(WIN_BUILD)/sidebar_test.exe tests/sidebar_test.c src/sidebar.c
	$(WIN_BUILD)/sidebar_test.exe

win-check: $(WIN_SRC) $(WIN_HDR) $(WIN_LIBUI_LIB) $(WIN_BUILD)/sqlc_rc.o
	$(WIN_CC) $(WIN_CFLAGS) $(WIN_DEPS_CFLAGS) -fsyntax-only $(WIN_SRC)
	@echo "win-check: compiled for Windows without warnings or errors"

# ---- dev helpers ----
# Scripted GUI smoke test with sanitizers; needs a database with tables
# customers, orders and a view (see README).
gui-test: $(GUI_SRC) $(GUI_HDR) $(LIBUI_LIB)
	$(CC) $(CPPFLAGS) -I$(LIBUI_DIR) -std=c11 -Wall -Wextra -g -O0 -fsanitize=address,undefined \
		-DSQLC_AUTOTEST -o $(BUILD)/sqlc-gui-test $(GUI_SRC) $(GUI_LIBS) $(LDLIBS)
	SQLC_CONFIG_DIR=$(BUILD)/test-config SQLC_TEST_DB=$${SQLC_TEST_DB:-test_sqlc_demo} \
		$(BUILD)/sqlc-gui-test

# Unit tests: saved-connections file format, export writers, the AI request /
# response handling, the row-editing SQL, the browse page SQL, the SQL tips and the tab texts (no database or network needed).
TEST_CFLAGS := -std=c11 -Wall -Wextra -g -O0 -D_DARWIN_C_SOURCE -fsanitize=address,undefined -Isrc

test: tests/profiles_test.c src/profiles.c src/profiles.h tests/export_test.c src/export.c src/export.h \
      src/platform.c src/platform.h tests/platform_test.c \
      tests/ai_test.c src/ai.c src/ai.h src/json.c src/json.h \
      tests/row_edit_test.c src/row_edit.c src/row_edit.h \
      tests/browse_test.c src/browse.c src/browse.h \
      tests/sql_tips_test.c src/sql_tips.c src/sql_tips.h \
      tests/tabs_test.c src/tabs.c src/tabs.h \
      tests/sidebar_test.c src/sidebar.c src/sidebar.h \
      tests/db_lost_test.c src/db.h
	@mkdir -p $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $(BUILD)/platform_test tests/platform_test.c src/platform.c
	$(BUILD)/platform_test $(BUILD)/test-platform
	$(CC) $(TEST_CFLAGS) -o $(BUILD)/profiles_test tests/profiles_test.c src/profiles.c src/platform.c
	$(BUILD)/profiles_test $(BUILD)/test-profiles
	$(CC) $(TEST_CFLAGS) -o $(BUILD)/export_test tests/export_test.c src/export.c src/platform.c
	$(BUILD)/export_test
	$(CC) $(TEST_CFLAGS) -o $(BUILD)/ai_test tests/ai_test.c src/ai.c src/json.c -lcurl
	$(BUILD)/ai_test
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -o $(BUILD)/row_edit_test tests/row_edit_test.c src/row_edit.c
	$(BUILD)/row_edit_test
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -o $(BUILD)/browse_test tests/browse_test.c src/browse.c
	$(BUILD)/browse_test
	$(CC) $(TEST_CFLAGS) -o $(BUILD)/sql_tips_test tests/sql_tips_test.c src/sql_tips.c
	$(BUILD)/sql_tips_test
	$(CC) $(TEST_CFLAGS) -o $(BUILD)/tabs_test tests/tabs_test.c src/tabs.c
	$(BUILD)/tabs_test
	$(CC) $(TEST_CFLAGS) -o $(BUILD)/sidebar_test tests/sidebar_test.c src/sidebar.c
	$(BUILD)/sidebar_test
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -o $(BUILD)/db_lost_test tests/db_lost_test.c
	$(BUILD)/db_lost_test

# Export round trip against a real server: SQL re-import, XML and CSV checks.
# Creates and drops the databases test_sqlc_export_src and test_sqlc_export_dst.
export-test: tests/export_db_test.c tests/export_roundtrip.sh src/export.c src/export_db.c src/db.c src/platform.c
	@mkdir -p $(BUILD)
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -o $(BUILD)/export_db_test tests/export_db_test.c \
		src/export.c src/export_db.c src/db.c src/platform.c $(LDLIBS)
	sh tests/export_roundtrip.sh $(BUILD)/export_db_test $(BUILD)/export-test

debug: CFLAGS = -std=c11 -Wall -Wextra -g -O0 -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean $(BIN) $(GUI_BIN)

# Editor support (clangd): writes the include paths for this machine.
compile_flags.txt:
	printf "%s\n" -std=c11 -D_DARWIN_C_SOURCE -I$(LIBUI_DIR) $(shell mariadb_config --cflags) | tr " " "\n" > $@

clean:
	rm -f $(BIN) $(GUI_BIN) $(WIN_BIN)

distclean: clean
	rm -rf $(BUILD)

.PHONY: all gui gui-test test export-test clean distclean debug compile_flags.txt windows win-check win-test
