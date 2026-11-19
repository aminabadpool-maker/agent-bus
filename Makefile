CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pthread -Iinclude

.PHONY: all clean
all: build/agent_busd

build/agent_busd: src/main.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf build
