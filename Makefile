CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pthread -Iinclude
SAN_FLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -Iinclude

.PHONY: all test test-asan stress test-route test-heartbeat clean
all: build/agent_busd build/load_sim build/route_test build/heartbeat_test

build/agent_busd: src/main.c src/ab_frame.c src/ab_net.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

build/load_sim: tests/load_sim.c src/ab_frame.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

build/route_test: tests/route_test.c src/ab_frame.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

build/heartbeat_test: tests/heartbeat_test.c src/ab_frame.c src/ab_net.c
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

test-route: build/agent_busd build/route_test
	@./build/agent_busd >/dev/null 2>&1 & \
	PID=$$!; \
	sleep 0.2; \
	./build/route_test 127.0.0.1 9090; \
	RC=$$?; \
	kill -TERM $$PID 2>/dev/null || true; \
	wait $$PID 2>/dev/null || true; \
	exit $$RC

test-heartbeat: build/heartbeat_test
	./build/heartbeat_test

clean:
	rm -rf build
