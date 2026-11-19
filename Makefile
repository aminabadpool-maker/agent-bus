CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pthread -Iinclude
SAN_FLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -Iinclude

.PHONY: all test test-asan stress clean
all: build/agent_busd build/load_sim

build/agent_busd: src/main.c src/ab_frame.c src/ab_net.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

build/load_sim: tests/load_sim.c src/ab_frame.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

test: build/frame_test
	./build/frame_test

build/frame_test: tests/frame_test.c src/ab_frame.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

test-asan: build/frame_test_san build/stress_test_san
	./build/frame_test_san
	./build/stress_test_san

build/frame_test_san: tests/frame_test.c src/ab_frame.c
	@mkdir -p build
	$(CC) $(SAN_FLAGS) $^ -o $@

build/stress_test_san: tests/stress_test.c src/ab_frame.c
	@mkdir -p build
	$(CC) $(SAN_FLAGS) $^ -o $@

stress: build/stress_test build/load_sim
	./build/stress_test

build/stress_test: tests/stress_test.c src/ab_frame.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -rf build
