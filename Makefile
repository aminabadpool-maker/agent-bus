CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pthread -Iinclude
SAN_FLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -Iinclude
TMP_BASE := $(if $(TMPDIR),$(TMPDIR),build)

.PHONY: all test test-asan stress test-route clean
all: build/agent_busd build/load_sim build/route_test

build/agent_busd: src/main.c src/ab_frame.c src/ab_net.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

build/load_sim: tests/load_sim.c src/ab_frame.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

build/route_test: tests/route_test.c src/ab_frame.c
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
	@mkdir -p "$(TMP_BASE)" && \
	LOG_FILE="$(TMP_BASE)/agent_busd_route.log" && \
	./build/agent_busd >"$$LOG_FILE" 2>&1 & \
	PID=$$!; \
	READY=0; \
	for _ in 1 2 3 4 5 6 7 8 9 10; do \
		if ! kill -0 $$PID 2>/dev/null; then \
			break; \
		fi; \
		if [ -s "$$LOG_FILE" ]; then \
			READY=1; \
			break; \
		fi; \
		sleep 0.1; \
	done; \
	RC=1; \
	if kill -0 $$PID 2>/dev/null && [ "$$READY" -eq 1 ]; then \
		./build/route_test 127.0.0.1 9090; \
		RC=$$?; \
	fi; \
	if [ $$RC -ne 0 ] && [ -f "$$LOG_FILE" ]; then \
		echo "--- agent_busd log ---"; \
		cat "$$LOG_FILE"; \
	fi; \
	kill -TERM $$PID 2>/dev/null || true; \
	wait $$PID 2>/dev/null || true; \
	rm -f "$$LOG_FILE"; \
	exit $$RC

clean:
	rm -rf build
