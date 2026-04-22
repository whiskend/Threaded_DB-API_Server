CC := clang
CFLAGS := -std=c99 -Wall -Wextra -Werror -Iinclude
BUILD_DIR := build

SRC_FILES := $(wildcard src/*.c)
LIB_SRC_FILES := $(filter-out src/main.c,$(SRC_FILES))
LIB_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(LIB_SRC_FILES))
APP_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC_FILES))
TEST_SOURCES := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/%,$(TEST_SOURCES))
TEST_SHELLS := $(wildcard tests/test_*.sh)
TOOL_SOURCES := $(wildcard tools/*.c)
TOOL_BINS := $(patsubst tools/%.c,$(BUILD_DIR)/%,$(TOOL_SOURCES))

FULL_APP_SRCS := src/main.c src/schema.c src/storage.c src/executor.c src/result.c src/runtime.c src/bptree.c src/benchmark.c
APP_TARGET :=

ifeq ($(words $(wildcard $(FULL_APP_SRCS))),$(words $(FULL_APP_SRCS)))
APP_TARGET := sql_processor
endif

.PHONY: all test clean

all: $(TEST_BINS) $(APP_TARGET) $(TOOL_BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_%: tests/test_%.c $(LIB_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $< -o $@

$(BUILD_DIR)/%: tools/%.c $(LIB_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $< -o $@

sql_processor: $(APP_OBJECTS)
	$(CC) $(CFLAGS) $(APP_OBJECTS) -o $@

test: $(TEST_BINS) $(APP_TARGET)
	@for bin in $(TEST_BINS); do \
		$$bin; \
	done
	@for script in $(TEST_SHELLS); do \
		sh $$script; \
	done

clean:
	rm -rf $(BUILD_DIR) sql_processor
