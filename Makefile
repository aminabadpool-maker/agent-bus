CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pthread -Iinclude
SAN_FLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -Iinclude

.PHONY: all test test-asan clean
all: build/agent_busd

build/agent_busd: src/main.c src/ab_frame.c src/ab_net.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

test: build/frame_test
	./build/frame_test

build/frame_test: tests/frame_test.c src/ab_frame.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

test-asan: build/frame_test_san
	./build/frame_test_san

build/frame_test_san: tests/frame_test.c src/ab_frame.c
	@mkdir -p build
	$(CC) $(SAN_FLAGS) $^ -o $@

clean:
	rm -rf build
