CC = gcc
THREAD_FLAGS = -pthread
CFLAGS = -Wall -Wextra -std=c11 -Iinclude $(THREAD_FLAGS)
TEST_CFLAGS = $(CFLAGS) -DSQLPARSER_BENCHMARK_NO_MAIN

ifeq ($(OS),Windows_NT)
SOCKET_LIBS = -lws2_32
else
SOCKET_LIBS =
endif

BUILD_DIR = build
BIN_DIR = $(BUILD_DIR)/bin

COMMON_SOURCES = \
	src/common/platform.c \
	src/common/util.c \
	src/storage/schema.c \
	src/storage/storage.c \
	src/sql/ast.c \
	src/sql/lexer.c \
	src/sql/parser.c \
	src/execution/executor.c \
	src/index/bptree.c \
	src/index/table_index.c

APP_SOURCES = src/app/main.c $(COMMON_SOURCES)
TEST_SOURCES = tests/test_runner.c src/benchmark/benchmark_main.c $(COMMON_SOURCES)
BENCHMARK_SOURCES = src/benchmark/benchmark_main.c $(COMMON_SOURCES)
SERVER_LIBRARY_SOURCES = \
	src/server/server.c \
	src/server/worker_pool.c \
	src/server/task_queue.c \
	src/http/http_request.c \
	src/http/http_response.c \
	src/http/router.c \
	src/api/health_handler.c \
	src/api/query_handler.c \
	src/service/db_service.c \
	src/engine/engine_lock_manager.c \
	src/engine/sql_engine_adapter.c
SERVER_SOURCES = \
	src/app/sqlapi_server_main.c \
	$(SERVER_LIBRARY_SOURCES) \
	$(COMMON_SOURCES)
TEST_SOURCES = tests/test_runner.c tests/test_api_server.c src/benchmark/benchmark_main.c $(SERVER_LIBRARY_SOURCES) $(COMMON_SOURCES)

APP_BIN = $(BIN_DIR)/sqlparser
TEST_BIN = $(BIN_DIR)/test_runner
BENCHMARK_BIN = $(BIN_DIR)/benchmark_runner
SERVER_BIN = $(BIN_DIR)/sqlapi_server

.PHONY: all test benchmark clean

all: $(APP_BIN) $(SERVER_BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(APP_BIN): $(APP_SOURCES) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(APP_SOURCES) $(SOCKET_LIBS)

$(TEST_BIN): $(TEST_SOURCES) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_SOURCES) $(SOCKET_LIBS)

$(BENCHMARK_BIN): $(BENCHMARK_SOURCES) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(BENCHMARK_SOURCES) $(SOCKET_LIBS)

$(SERVER_BIN): $(SERVER_SOURCES) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SOURCES) $(SOCKET_LIBS)

test: $(APP_BIN) $(SERVER_BIN) $(TEST_BIN)
	./$(TEST_BIN)

benchmark: $(BENCHMARK_BIN)

ifeq ($(OS),Windows_NT)
clean:
	cmd /c "if exist \"$(BUILD_DIR)\" rmdir /s /q \"$(BUILD_DIR)\""
else
clean:
	rm -rf $(BUILD_DIR)
endif
