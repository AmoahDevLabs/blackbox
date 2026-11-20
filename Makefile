CC ?= cc
CFLAGS ?= -O2 -g
WARNINGS := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic \
            -Wshadow -Wconversion -Werror

BUILD_DIR := build
BIN := $(BUILD_DIR)/blackbox
SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
HDRS := $(wildcard src/*.h)

.PHONY: all check clean install

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(WARNINGS) -o $@ $^

$(BUILD_DIR)/%.o: src/%.c $(HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

check: $(BIN)
	./$(BIN) --version
	./$(BIN) --help >/dev/null
	! ./$(BIN) definitely-not-a-command >/dev/null 2>&1

clean:
	rm -rf $(BUILD_DIR)

# Overwrites the old shell-script symlinks in ~/.local/bin.
install: $(BIN)
	mkdir -p $(HOME)/.local/bin
	install -m 0755 $(BIN) $(HOME)/.local/bin/blackbox-c
	@for n in blackboxd zed-doctor zed-recover opencode-doctor opencode-safe firefox-doctor; do \
		ln -sfn blackbox-c $(HOME)/.local/bin/$$n; \
	done
	@echo "Installed blackbox-c and command symlinks into $(HOME)/.local/bin"
