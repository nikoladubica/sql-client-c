CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CPPFLAGS += -D_DARWIN_C_SOURCE $(shell mariadb_config --cflags)
LDLIBS  += $(shell mariadb_config --libs) -ledit

BIN := sqlc
SRC := src/main.c src/complete.c
HDR := src/complete.h

$(BIN): $(SRC) $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

debug: CFLAGS = -std=c11 -Wall -Wextra -g -O0 -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean $(BIN)

# Editor support (clangd): writes the MariaDB include paths for this machine.
compile_flags.txt:
	printf "%s\n" -std=c11 -D_DARWIN_C_SOURCE $(shell mariadb_config --cflags) | tr " " "\n" > $@

clean:
	rm -f $(BIN)

.PHONY: clean debug compile_flags.txt
