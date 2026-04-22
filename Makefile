CC := clang
BUILD_DIR := build

CFLAGS_COMMON := -std=c99 -Wall -Wextra -Werror -Iinclude
CFLAGS_SERVER := $(CFLAGS_COMMON) -D_XOPEN_SOURCE=700 -pthread

ENGINE_SRCS := \
	src/utils.c \
	src/lexer.c \
	src/parser.c \
	src/schema.c \
	src/storage.c \
	src/runtime.c \
	src/executor.c \
	src/result.c \
	src/bptree.c \
	src/benchmark.c

CLI_SRCS := \
	src/main.c \
	src/cli.c

SERVER_SRCS := \
	src/server_main.c \
	src/server.c \
	src/http.c \
	src/thread_pool.c \
	src/task_queue.c \
	src/db_api.c \
	src/json_parser.c \
	src/json_writer.c

ENGINE_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(ENGINE_SRCS))
CLI_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(CLI_SRCS))
SERVER_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/server_%.o,$(SERVER_SRCS))

TEST_SOURCES := $(sort $(wildcard tests/test_*.c))
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/%,$(TEST_SOURCES))
TEST_SHELLS := $(sort $(wildcard tests/test_*.sh))
TOOL_SOURCES := $(sort $(wildcard tools/*.c))
TOOL_BINS := $(patsubst tools/%.c,$(BUILD_DIR)/%,$(TOOL_SOURCES))

.PHONY: all test clean

all: $(TEST_BINS) sql_processor mini_db_server $(TOOL_BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS_COMMON) -c $< -o $@

$(BUILD_DIR)/server_%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS_SERVER) -c $< -o $@

$(BUILD_DIR)/test_%: tests/test_%.c $(ENGINE_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CFLAGS_COMMON) $(ENGINE_OBJECTS) $< -o $@

$(BUILD_DIR)/%: tools/%.c $(ENGINE_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CFLAGS_COMMON) $(ENGINE_OBJECTS) $< -o $@

sql_processor: $(ENGINE_OBJECTS) $(CLI_OBJECTS)
	$(CC) $(CFLAGS_COMMON) $(ENGINE_OBJECTS) $(CLI_OBJECTS) -o $@

mini_db_server: $(ENGINE_OBJECTS) $(SERVER_OBJECTS)
	$(CC) $(CFLAGS_SERVER) $(ENGINE_OBJECTS) $(SERVER_OBJECTS) -o $@

test: $(TEST_BINS) sql_processor mini_db_server
	@set -e; for bin in $(TEST_BINS); do \
		$$bin; \
	done
	@set -e; for script in $(TEST_SHELLS); do \
		sh $$script; \
	done

clean:
	rm -rf $(BUILD_DIR) sql_processor mini_db_server
