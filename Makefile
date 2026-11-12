CC = gcc
CPPFLAGS = -Iinclude
CFLAGS = -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror
LDFLAGS =
BUILD = build

.PHONY: all test sanitize native smoke benchmark clean

all: $(BUILD)/bench $(BUILD)/test_link $(BUILD)/test_perf $(BUILD)/demo

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bench: src/bench.c src/link.c src/perf_metrics.c include/link.h include/perf_metrics.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/bench.c src/link.c src/perf_metrics.c $(LDFLAGS) -o $@

$(BUILD)/test_link: tests/test_link.c src/link.c include/link.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_link.c src/link.c $(LDFLAGS) -Wl,--wrap=malloc -o $@

$(BUILD)/test_perf: tests/test_perf.c src/perf_metrics.c include/perf_metrics.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_perf.c src/perf_metrics.c $(LDFLAGS) -o $@

$(BUILD)/demo: examples/demo.c src/link.c include/link.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) examples/demo.c src/link.c $(LDFLAGS) -o $@

test: all
	./$(BUILD)/test_link
	./$(BUILD)/test_perf
	./$(BUILD)/demo
	sh tests/test_cli.sh ./$(BUILD)/bench

sanitize:
	$(MAKE) BUILD=build/sanitize CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test

native:
	$(MAKE) BUILD=build/native CFLAGS='-O2 -g -march=native -std=c11 -Wall -Wextra -Wpedantic -Werror' all

smoke: all
	./$(BUILD)/bench --nodes 1000 --queries 1000 --repeats 3

benchmark: all
	sh scripts/run_bench.sh ./$(BUILD)/bench

clean:
	rm -f build/bench build/test_link build/test_perf build/demo
