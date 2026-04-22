#include "sqlparser/common/util.h"
#include "sqlparser/benchmark/benchmark.h"
#include "sqlparser/execution/executor.h"
#include "sqlparser/index/bptree.h"
#include "sqlparser/index/table_index.h"
#include "sqlparser/sql/lexer.h"
#include "sqlparser/sql/parser.h"
#include "sqlparser/storage/schema.h"
#include "sqlparser/storage/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int run_api_server_tests(void);

#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(path) _mkdir(path)
#define CLI_BINARY_PATH ".\\build\\bin\\sqlparser.exe"
#define SYSTEM_EXIT_CODE(status) (status)
#else
#include <sys/stat.h>
#include <sys/wait.h>
#define MAKE_DIR(path) mkdir(path, 0755)
#define CLI_BINARY_PATH "./build/bin/sqlparser"
#define SYSTEM_EXIT_CODE(status) (WIFEXITED(status) ? WEXITSTATUS(status) : (status))
#endif

static int tests_run = 0;
static int tests_failed = 0;
static int temp_dir_counter = 0;

static void build_child_path(char *buffer, size_t size, const char *root, const char *child);

static void expect_true(int condition, const char *name) {
    tests_run++;
    if (!condition) {
        tests_failed++;
        fprintf(stderr, "[FAIL] %s\n", name);
    } else {
        printf("[PASS] %s\n", name);
    }
}

static void reset_runtime_state(void) {
    table_index_registry_reset();
}

static int write_text_file(const char *path, const char *content) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }

    fputs(content, file);
    fclose(file);
    return 1;
}

static int run_cli_command(const char *root, const char *arguments, const char *stdin_text,
                           char *stdout_text, size_t stdout_size,
                           char *stderr_text, size_t stderr_size,
                           int *exit_code) {
    char stdout_path[192];
    char stderr_path[192];
    char stdin_path[192];
    char command[1024];
    char error[256];
    char *stdout_file_text;
    char *stderr_file_text;
    int status;

    build_child_path(stdout_path, sizeof(stdout_path), root, "cli_stdout.txt");
    build_child_path(stderr_path, sizeof(stderr_path), root, "cli_stderr.txt");
    build_child_path(stdin_path, sizeof(stdin_path), root, "cli_stdin.txt");

    if (!write_text_file(stdout_path, "") || !write_text_file(stderr_path, "")) {
        return 0;
    }

    if (stdin_text != NULL && !write_text_file(stdin_path, stdin_text)) {
        return 0;
    }

#ifdef _WIN32
    if (stdin_text != NULL) {
        snprintf(command, sizeof(command),
                 "cmd /c \"\"%s\" %s < \"%s\" > \"%s\" 2> \"%s\"\"",
                 CLI_BINARY_PATH, arguments, stdin_path, stdout_path, stderr_path);
    } else {
        snprintf(command, sizeof(command),
                 "cmd /c \"\"%s\" %s > \"%s\" 2> \"%s\"\"",
                 CLI_BINARY_PATH, arguments, stdout_path, stderr_path);
    }
#else
    if (stdin_text != NULL) {
        snprintf(command, sizeof(command),
                 "\"%s\" %s < \"%s\" > \"%s\" 2> \"%s\"",
                 CLI_BINARY_PATH, arguments, stdin_path, stdout_path, stderr_path);
    } else {
        snprintf(command, sizeof(command),
                 "\"%s\" %s > \"%s\" 2> \"%s\"",
                 CLI_BINARY_PATH, arguments, stdout_path, stderr_path);
    }
#endif

    status = system(command);
    if (status < 0) {
        return 0;
    }

    stdout_file_text = read_entire_file(stdout_path, error, sizeof(error));
    if (stdout_file_text == NULL) {
        return 0;
    }

    stderr_file_text = read_entire_file(stderr_path, error, sizeof(error));
    if (stderr_file_text == NULL) {
        free(stdout_file_text);
        return 0;
    }

    snprintf(stdout_text, stdout_size, "%s", stdout_file_text);
    snprintf(stderr_text, stderr_size, "%s", stderr_file_text);
    *exit_code = SYSTEM_EXIT_CODE(status);

    free(stdout_file_text);
    free(stderr_file_text);
    return 1;
}

static int count_lines(const char *text) {
    int count = 0;
    int saw_character = 0;
    while (*text != '\0') {
        saw_character = 1;
        if (*text == '\n') {
            count++;
        }
        text++;
    }
    if (saw_character && *(text - 1) != '\n') {
        count++;
    }
    return count;
}

static void build_child_path(char *buffer, size_t size, const char *root, const char *child) {
    snprintf(buffer, size, "%s/%s", root, child);
}

static int create_test_dirs(char *root, size_t root_size, char *schema_dir, size_t schema_size, char *data_dir, size_t data_size) {
    long suffix = (long)time(NULL);
    temp_dir_counter++;

    snprintf(root, root_size, "build/tests/tmp_%ld_%d", suffix, temp_dir_counter);
    build_child_path(schema_dir, schema_size, root, "schema");
    build_child_path(data_dir, data_size, root, "data");

    MAKE_DIR("build");
    MAKE_DIR("build/tests");
    if (MAKE_DIR(root) != 0) {
        return 0;
    }
    if (MAKE_DIR(schema_dir) != 0) {
        return 0;
    }
    if (MAKE_DIR(data_dir) != 0) {
        return 0;
    }

    return 1;
}

static int load_statement(const char *sql, Statement *statement) {
    TokenArray tokens = {0};
    ParseResult result;
    char error[256];

    if (!lex_sql(sql, &tokens, error, sizeof(error))) {
        fprintf(stderr, "lex failed: %s\n", error);
        return 0;
    }

    result = parse_statement(&tokens);
    if (!result.ok) {
        fprintf(stderr, "parse failed: %s\n", result.message);
        free_tokens(&tokens);
        return 0;
    }

    *statement = result.statement;
    free_tokens(&tokens);
    return 1;
}

static void test_bptree_insert_and_search(void) {
    BPlusTree tree;
    char error[256];
    long value;
    int index;

    bptree_init(&tree);
    for (index = 1; index <= 10; index++) {
        expect_true(bptree_insert(&tree, index, (long)(index * 100), error, sizeof(error)), "B+ tree inserts key");
    }

    for (index = 1; index <= 10; index++) {
        expect_true(bptree_search(&tree, index, &value), "B+ tree finds inserted key");
        if (bptree_search(&tree, index, &value)) {
            expect_true(value == (long)(index * 100), "B+ tree returns correct offset");
        }
    }

    expect_true(!bptree_insert(&tree, 5, 999L, error, sizeof(error)), "B+ tree rejects duplicate key");
    bptree_free(&tree);
}

static void test_bptree_split_preserves_searchability(void) {
    BPlusTree tree;
    char error[256];
    long value;
    int index;

    bptree_init(&tree);
    for (index = 1; index <= 32; index++) {
        expect_true(bptree_insert(&tree, index, (long)(index * 10), error, sizeof(error)), "B+ tree inserts enough keys to trigger splits");
    }

    for (index = 1; index <= 32; index++) {
        expect_true(bptree_search(&tree, index, &value), "B+ tree keeps keys searchable after splits");
        if (bptree_search(&tree, index, &value)) {
            expect_true(value == (long)(index * 10), "B+ tree preserves values after splits");
        }
    }

    bptree_free(&tree);
}

static void test_parser_where(void) {
    TokenArray tokens = {0};
    ParseResult result;
    char error[256];

    expect_true(lex_sql("SELECT name FROM users WHERE age = 20;", &tokens, error, sizeof(error)), "lexer parses SELECT with WHERE");
    result = parse_statement(&tokens);
    expect_true(result.ok, "parser accepts WHERE clause");
    if (result.ok) {
        expect_true(result.statement.as.select_statement.has_where == 1, "parser marks WHERE flag");
        expect_true(strcmp(result.statement.as.select_statement.where_column, "age") == 0, "parser reads WHERE column");
        expect_true(strcmp(result.statement.as.select_statement.where_value, "20") == 0, "parser reads WHERE value");
        free_statement(&result.statement);
    }
    free_tokens(&tokens);
}

static void test_parser_error_details(void) {
    TokenArray tokens = {0};
    ParseResult result;
    char error[256];

    expect_true(lex_sql("SELECT , name FROM users;", &tokens, error, sizeof(error)), "lexer parses malformed SELECT for parser detail");
    result = parse_statement(&tokens);
    expect_true(!result.ok, "parser rejects missing SELECT column");
    expect_true(strstr(result.message, "expected identifier, got COMMA(\",\") at position") != NULL, "parser error includes actual token for missing identifier");
    free_tokens(&tokens);

    tokens.items = NULL;
    tokens.count = 0;
    tokens.capacity = 0;
    expect_true(lex_sql("SELECT name users;", &tokens, error, sizeof(error)), "lexer parses malformed SELECT missing FROM");
    result = parse_statement(&tokens);
    expect_true(!result.ok, "parser rejects missing FROM keyword");
    expect_true(strstr(result.message, "expected keyword FROM, got IDENTIFIER(\"users\") at position") != NULL, "parser error includes actual token for missing keyword");
    free_tokens(&tokens);
}

static void test_parser_utf8_identifiers(void) {
    TokenArray tokens = {0};
    ParseResult result;
    char error[256];

    expect_true(lex_sql("SELECT department, name FROM 학생;", &tokens, error, sizeof(error)), "lexer parses UTF-8 identifiers");
    result = parse_statement(&tokens);
    expect_true(result.ok, "parser accepts UTF-8 identifiers");
    if (result.ok) {
        expect_true(strcmp(result.statement.as.select_statement.table_name, "학생") == 0, "parser reads UTF-8 table name");
        free_statement(&result.statement);
    }
    free_tokens(&tokens);
}

static void test_cli_error_messages(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char stdout_text[2048];
    char stderr_text[4096];
    int exit_code;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create CLI error test directories");

    expect_true(run_cli_command(root, "-e \"SELECT\"", NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with incomplete SELECT");
    expect_true(exit_code != 0, "CLI returns non-zero for incomplete SELECT");
    expect_true(strstr(stderr_text, "error: expected identifier, got EOF at position") != NULL, "CLI reports incomplete SELECT with EOF detail");
    expect_true(stdout_text[0] == '\0', "CLI incomplete SELECT does not print stdout");

    expect_true(run_cli_command(root, "-e \"DROP TABLE users;\"", NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with unsupported SQL command");
    expect_true(exit_code != 0, "CLI returns non-zero for unsupported SQL command");
    expect_true(strstr(stderr_text, "error: only INSERT and SELECT are supported, got IDENTIFIER(\"DROP\") at position 0") != NULL, "CLI reports unsupported SQL command with token detail");

    expect_true(run_cli_command(root, "-e \"SELECT name FROM users\"", NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with missing semicolon");
    expect_true(exit_code != 0, "CLI returns non-zero for missing semicolon");
    expect_true(strstr(stderr_text, "error: expected ';' at end of SQL statement, got EOF at position") != NULL, "CLI reports missing semicolon with EOF detail");

    expect_true(run_cli_command(root, "-e \"SELECT * FROM users WHERE id = 10 AND name = user10;\"", NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with unsupported AND condition");
    expect_true(exit_code != 0, "CLI returns non-zero for unsupported AND condition");
    expect_true(strstr(stderr_text, "error: AND/OR conditions are not supported; only a single WHERE condition is allowed") != NULL, "CLI reports unsupported AND condition directly");

    expect_true(run_cli_command(root, "-e \"SELECT * FROM users WHERE id = 10; user10;\"", NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with extra tokens after semicolon");
    expect_true(exit_code != 0, "CLI returns non-zero for extra tokens after semicolon");
    expect_true(strstr(stderr_text, "error: unexpected tokens after SQL statement, got IDENTIFIER(\"user10\") at position") != NULL, "CLI reports extra tokens after semicolon");

    expect_true(run_cli_command(root, "-e \"\"", NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with empty SQL");
    expect_true(exit_code != 0, "CLI returns non-zero for empty SQL");
    expect_true(strstr(stderr_text, "error: missing SQL statement") != NULL, "CLI reports empty SQL as missing SQL statement");

    expect_true(run_cli_command(root, "-e \"   \"", NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with whitespace SQL");
    expect_true(exit_code != 0, "CLI returns non-zero for whitespace SQL");
    expect_true(strstr(stderr_text, "error: missing SQL statement") != NULL, "CLI reports whitespace SQL as missing SQL statement");

    expect_true(run_cli_command(root, "-f", NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with missing file path");
    expect_true(exit_code != 0, "CLI returns non-zero for missing file path");
    expect_true(strstr(stderr_text, "error: missing file path after -f") != NULL, "CLI reports missing file path after -f");
    expect_true(strstr(stderr_text, "[사용법]") != NULL, "CLI prints usage after missing file path");
    expect_true(strstr(stderr_text, "============================================================") != NULL, "CLI usage includes visual separator after missing file path");

    expect_true(run_cli_command(root, "-f build/tests/does_not_exist/query.sql", NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with missing SQL file");
    expect_true(exit_code != 0, "CLI returns non-zero for missing SQL file");
    expect_true(strstr(stderr_text, "error: failed to open SQL file 'build/tests/does_not_exist/query.sql'") != NULL, "CLI reports missing SQL file path");
    expect_true(strstr(stderr_text, "[사용법]") != NULL, "CLI prints usage after missing SQL file");

    expect_true(run_cli_command(root, "--bogus", NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with unknown option");
    expect_true(exit_code != 0, "CLI returns non-zero for unknown option");
    expect_true(strstr(stderr_text, "error: unknown option: --bogus") != NULL, "CLI reports unknown option directly");
    expect_true(strstr(stderr_text, "[사용법]") != NULL, "CLI prints usage after unknown option");
    expect_true(strstr(stderr_text, "[옵션]") != NULL, "CLI usage includes options section after unknown option");

    expect_true(run_cli_command(root, "-f -", "SELECT", stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with invalid stdin SQL");
    expect_true(exit_code != 0, "CLI returns non-zero for invalid stdin SQL");
    expect_true(strstr(stderr_text, "error: expected identifier, got EOF at position") != NULL, "CLI reports invalid stdin SQL with parser detail");
}

static void test_cli_bare_directory_argument_is_not_sql(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char directory_path[192];
    char arguments[256];
    char stdout_text[2048];
    char stderr_text[4096];
    int exit_code;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create bare directory CLI test directories");
    build_child_path(directory_path, sizeof(directory_path), root, "not_sql_dir");
    expect_true(MAKE_DIR(directory_path) == 0, "create bare directory CLI argument");
    snprintf(arguments, sizeof(arguments), "\"%s\"", directory_path);

    expect_true(run_cli_command(root, arguments, NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with bare directory argument");
    expect_true(exit_code != 0, "CLI returns non-zero for bare directory argument");
    expect_true(strstr(stderr_text, "error: path is a directory, not a SQL file: ") != NULL, "CLI reports bare directory argument as file path error");
    expect_true(strstr(stderr_text, directory_path) != NULL, "CLI includes bare directory path in error");
    expect_true(strstr(stderr_text, "[사용법]") != NULL, "CLI prints usage after bare directory argument error");
}

static void test_cli_success_output_includes_elapsed_time(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char stdout_text[4096];
    char stderr_text[4096];
    int exit_code;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create CLI success timing test directories");

    expect_true(run_cli_command(root, "-e \"SELECT * FROM student WHERE id = 1;\"", NULL, stdout_text, sizeof(stdout_text), stderr_text, sizeof(stderr_text), &exit_code), "run CLI with successful SELECT");
    expect_true(exit_code == 0, "CLI returns zero for successful SELECT");
    expect_true(stderr_text[0] == '\0', "CLI successful SELECT does not print stderr");
    expect_true(strstr(stdout_text, "+-") != NULL, "CLI successful SELECT prints result table");
    expect_true(strstr(stdout_text, "SELECT 1") != NULL, "CLI successful SELECT prints affected row summary");
    expect_true(strstr(stdout_text, "Elapsed time: ") != NULL, "CLI successful SELECT prints elapsed time");
}

static void test_schema_loading_with_alias_filename(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    SchemaResult result;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create alias schema test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "student.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "student.csv");
    expect_true(write_text_file(schema_path, "table=학생\ncolumns=department,student_number,name,age\n"), "write alias schema meta");
    expect_true(write_text_file(data_path, "department,student_number,name,age\n컴퓨터공학과,2024001,김민수,20\n"), "write alias schema CSV");

    result = load_schema(schema_dir, data_dir, "학생");
    expect_true(result.ok, "load schema resolves alias table name");
    if (result.ok) {
        expect_true(strcmp(result.schema.storage_name, "student") == 0, "load schema keeps alias storage name");
        free_schema(&result.schema);
    }
}

static void test_schema_reports_missing_directory(void) {
    SchemaResult result;

    reset_runtime_state();
    result = load_schema("build/tests/does_not_exist/schema", "build/tests/does_not_exist/data", "users");
    expect_true(!result.ok, "load_schema fails for missing schema directory");
    expect_true(strstr(result.message, "failed to open schema directory 'build/tests/does_not_exist/schema'") != NULL, "load_schema reports missing schema directory path");
}

static void test_schema_reports_alias_candidate_open_failure(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char blocked_meta_path[192];
    SchemaResult result;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create alias candidate failure test directories");
    build_child_path(blocked_meta_path, sizeof(blocked_meta_path), schema_dir, "blocked.meta");
    expect_true(MAKE_DIR(blocked_meta_path) == 0, "create blocked meta directory");

    result = load_schema(schema_dir, data_dir, "users");
    expect_true(!result.ok, "load_schema fails when alias candidate meta cannot be opened");
    expect_true(strstr(result.message, "failed to open schema meta file") != NULL, "load_schema reports alias candidate open failure");
    expect_true(strstr(result.message, "blocked.meta") != NULL, "load_schema includes alias candidate path in error");
}

static void test_insert_auto_id(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char error[256];
    char *csv_text;
    Statement statement = {0};
    ExecResult result;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create auto id test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write auto id schema");
    expect_true(write_text_file(data_path, "name,age\n"), "write auto id CSV");
    expect_true(load_statement("INSERT INTO users (name) VALUES ('Alice');", &statement), "build auto id INSERT");

    result = execute_statement(&statement, schema_dir, data_dir, stdout);
    expect_true(result.ok, "execute INSERT with auto id");
    csv_text = read_entire_file(data_path, error, sizeof(error));
    expect_true(csv_text != NULL, "read CSV after auto id INSERT");
    if (csv_text != NULL) {
        expect_true(strstr(csv_text, "Alice,\"\"") != NULL, "INSERT writes user columns without explicit id");
        free(csv_text);
    }
    expect_true(table_index_is_loaded("users"), "INSERT loads table index");
    free_statement(&statement);
}

static void test_insert_overrides_user_id(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char error[256];
    char *csv_text;
    Statement statement = {0};
    ExecResult result;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create override id test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write override id schema");
    expect_true(write_text_file(data_path, "name,age\nBob,21\n"), "write existing CSV row");
    expect_true(load_statement("INSERT INTO users (id, name) VALUES (99, 'Alice');", &statement), "build INSERT with explicit id");

    result = execute_statement(&statement, schema_dir, data_dir, stdout);
    expect_true(!result.ok, "explicit id INSERT is rejected");
    csv_text = read_entire_file(data_path, error, sizeof(error));
    expect_true(csv_text != NULL, "read CSV after explicit id INSERT");
    if (csv_text != NULL) {
        expect_true(strstr(csv_text, "Alice") == NULL, "explicit id INSERT does not append row");
        free(csv_text);
    }
    free_statement(&statement);
}

static void test_select_execution_with_general_where(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char output_path[192];
    char error[256];
    char *output_text;
    Statement statement = {0};
    ExecResult result;
    FILE *output_file;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create general WHERE test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    build_child_path(output_path, sizeof(output_path), root, "select_where_output.txt");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write general WHERE schema");
    expect_true(write_text_file(data_path, "name,age\nAlice,20\nBob,21\nCarol,20\n"), "write general WHERE CSV");
    expect_true(load_statement("SELECT name FROM users WHERE age = 20;", &statement), "build general WHERE SELECT");

    output_file = fopen(output_path, "wb");
    expect_true(output_file != NULL, "open output file for general WHERE");
    if (output_file == NULL) {
        free_statement(&statement);
        return;
    }

    result = execute_statement(&statement, schema_dir, data_dir, output_file);
    fclose(output_file);
    expect_true(result.ok, "execute general WHERE SELECT");
    expect_true(result.affected_rows == 2, "general WHERE returns matching row count");
    output_text = read_entire_file(output_path, error, sizeof(error));
    expect_true(output_text != NULL, "read general WHERE output");
    if (output_text != NULL) {
        expect_true(strstr(output_text, "+-------+") != NULL, "general WHERE prints ASCII table border");
        expect_true(strstr(output_text, "| name  |") != NULL, "general WHERE prints padded table header");
        expect_true(strstr(output_text, "Alice") != NULL, "general WHERE prints first row");
        expect_true(strstr(output_text, "Carol") != NULL, "general WHERE prints second row");
        expect_true(strstr(output_text, "Bob") == NULL, "general WHERE excludes non-matching row");
        free(output_text);
    }
    expect_true(!table_index_is_loaded("users"), "general WHERE does not build id index");
    free_statement(&statement);
}

static void test_select_execution_with_utf8_display_width(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char output_path[192];
    char error[256];
    char *output_text;
    Statement statement = {0};
    ExecResult result;
    FILE *output_file;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create UTF-8 width SELECT test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    build_child_path(output_path, sizeof(output_path), root, "select_utf8_width_output.txt");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write UTF-8 width schema");
    expect_true(write_text_file(data_path, "name,age\nname_101010,25\n정영훈,25\n"), "write UTF-8 width CSV");
    expect_true(load_statement("SELECT name FROM users WHERE age = 25;", &statement), "build UTF-8 width SELECT");

    output_file = fopen(output_path, "wb");
    expect_true(output_file != NULL, "open output file for UTF-8 width SELECT");
    if (output_file == NULL) {
        free_statement(&statement);
        return;
    }

    result = execute_statement(&statement, schema_dir, data_dir, output_file);
    fclose(output_file);
    expect_true(result.ok, "execute UTF-8 width SELECT");
    output_text = read_entire_file(output_path, error, sizeof(error));
    expect_true(output_text != NULL, "read UTF-8 width SELECT output");
    if (output_text != NULL) {
        expect_true(strstr(output_text, "+-------------+") != NULL, "UTF-8 width SELECT prints widened border");
        expect_true(strstr(output_text, "| name        |") != NULL, "UTF-8 width SELECT prints padded header");
        expect_true(strstr(output_text, "| name_101010 |") != NULL, "UTF-8 width SELECT aligns ASCII row");
        expect_true(strstr(output_text, "| 정영훈      |") != NULL, "UTF-8 width SELECT aligns Korean row");
        free(output_text);
    }
    free_statement(&statement);
}

static void test_select_execution_with_id_index(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char output_path[192];
    char error[256];
    char *output_text;
    Statement statement = {0};
    ExecResult result;
    FILE *output_file;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create id WHERE test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    build_child_path(output_path, sizeof(output_path), root, "select_id_output.txt");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write id WHERE schema");
    expect_true(write_text_file(data_path, "name,age\nAlice,20\nBob,21\nCarol,22\n"), "write id WHERE CSV");
    expect_true(load_statement("SELECT name FROM users WHERE id = 2;", &statement), "build id WHERE SELECT");

    output_file = fopen(output_path, "wb");
    expect_true(output_file != NULL, "open output file for id WHERE");
    if (output_file == NULL) {
        free_statement(&statement);
        return;
    }

    result = execute_statement(&statement, schema_dir, data_dir, output_file);
    fclose(output_file);
    expect_true(result.ok, "execute indexed id SELECT");
    expect_true(result.affected_rows == 1, "indexed id SELECT returns one row");
    output_text = read_entire_file(output_path, error, sizeof(error));
    expect_true(output_text != NULL, "read indexed id output");
    if (output_text != NULL) {
        expect_true(strstr(output_text, "+------+") != NULL, "indexed id SELECT prints table border");
        expect_true(strstr(output_text, "| name |") != NULL, "indexed id SELECT prints table header");
        expect_true(strstr(output_text, "Bob") != NULL, "indexed id SELECT prints matching row");
        expect_true(strstr(output_text, "Alice") == NULL, "indexed id SELECT excludes non-matching rows");
        free(output_text);
    }
    expect_true(table_index_is_loaded("users"), "id WHERE builds table index");
    free_statement(&statement);
}

static void test_select_explicit_internal_id_column(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char output_path[192];
    char error[256];
    char *output_text;
    Statement statement = {0};
    ExecResult result;
    FILE *output_file;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create explicit internal id SELECT test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    build_child_path(output_path, sizeof(output_path), root, "select_internal_id_output.txt");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write explicit internal id SELECT schema");
    expect_true(write_text_file(data_path, "name,age\nAlice,20\nBob,21\nCarol,22\n"), "write explicit internal id SELECT CSV");
    expect_true(load_statement("SELECT id, name FROM users WHERE age = 21;", &statement), "build explicit internal id SELECT");

    output_file = fopen(output_path, "wb");
    expect_true(output_file != NULL, "open explicit internal id SELECT output");
    if (output_file == NULL) {
        free_statement(&statement);
        return;
    }

    result = execute_statement(&statement, schema_dir, data_dir, output_file);
    fclose(output_file);
    expect_true(result.ok, "execute explicit internal id SELECT");
    expect_true(result.affected_rows == 1, "explicit internal id SELECT returns one row");
    output_text = read_entire_file(output_path, error, sizeof(error));
    expect_true(output_text != NULL, "read explicit internal id SELECT output");
    if (output_text != NULL) {
        expect_true(strstr(output_text, "| id | name |") != NULL, "explicit internal id SELECT prints id header");
        expect_true(strstr(output_text, "| 2  | Bob  |") != NULL, "explicit internal id SELECT prints internal id value");
        expect_true(strstr(output_text, "Alice") == NULL, "explicit internal id SELECT excludes non-matching rows");
        free(output_text);
    }
    expect_true(!table_index_is_loaded("users"), "explicit internal id SELECT on general WHERE keeps linear path");
    free_statement(&statement);
}

static void test_select_star_hides_internal_id_column(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char output_path[192];
    char error[256];
    char *output_text;
    Statement statement = {0};
    ExecResult result;
    FILE *output_file;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create SELECT star hidden id test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    build_child_path(output_path, sizeof(output_path), root, "select_star_hidden_id_output.txt");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write SELECT star hidden id schema");
    expect_true(write_text_file(data_path, "name,age\nAlice,20\nBob,21\n"), "write SELECT star hidden id CSV");
    expect_true(load_statement("SELECT * FROM users WHERE id = 2;", &statement), "build SELECT star hidden id query");

    output_file = fopen(output_path, "wb");
    expect_true(output_file != NULL, "open SELECT star hidden id output");
    if (output_file == NULL) {
        free_statement(&statement);
        return;
    }

    result = execute_statement(&statement, schema_dir, data_dir, output_file);
    fclose(output_file);
    expect_true(result.ok, "execute SELECT star hidden id query");
    output_text = read_entire_file(output_path, error, sizeof(error));
    expect_true(output_text != NULL, "read SELECT star hidden id output");
    if (output_text != NULL) {
        expect_true(strstr(output_text, "| id |") == NULL, "SELECT star does not print hidden id header");
        expect_true(strstr(output_text, "Bob") != NULL, "SELECT star hidden id query still returns matching row");
        free(output_text);
    }
    free_statement(&statement);
}

static void test_index_rebuild_after_reset(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char output_path[192];
    char error[256];
    char *output_text;
    Statement statement = {0};
    ExecResult result;
    FILE *output_file;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create rebuild test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    build_child_path(output_path, sizeof(output_path), root, "rebuild_output.txt");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write rebuild schema");
    expect_true(write_text_file(data_path, "name,age\nAlice,20\nBob,21\n"), "write rebuild CSV");
    expect_true(load_statement("SELECT name FROM users WHERE id = 2;", &statement), "build rebuild SELECT");

    output_file = fopen(output_path, "wb");
    expect_true(output_file != NULL, "open rebuild output");
    if (output_file == NULL) {
        free_statement(&statement);
        return;
    }

    result = execute_statement(&statement, schema_dir, data_dir, output_file);
    fclose(output_file);
    expect_true(result.ok, "execute first indexed SELECT");
    expect_true(table_index_is_loaded("users"), "index loaded before reset");

    reset_runtime_state();
    output_file = fopen(output_path, "wb");
    expect_true(output_file != NULL, "reopen rebuild output");
    if (output_file == NULL) {
        free_statement(&statement);
        return;
    }

    result = execute_statement(&statement, schema_dir, data_dir, output_file);
    fclose(output_file);
    expect_true(result.ok, "execute indexed SELECT after reset");
    output_text = read_entire_file(output_path, error, sizeof(error));
    expect_true(output_text != NULL, "read rebuild output");
    if (output_text != NULL) {
        expect_true(strstr(output_text, "Bob") != NULL, "rebuild restores indexed result");
        free(output_text);
    }
    free_statement(&statement);
}

static void test_index_rebuild_after_invalidate(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char output_path[192];
    char error[256];
    char *output_text;
    Statement statement = {0};
    ExecResult result;
    FILE *output_file;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create invalidate rebuild test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    build_child_path(output_path, sizeof(output_path), root, "invalidate_rebuild_output.txt");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write invalidate rebuild schema");
    expect_true(write_text_file(data_path, "name,age\nAlice,20\nBob,21\n"), "write invalidate rebuild CSV");
    expect_true(load_statement("SELECT name FROM users WHERE id = 2;", &statement), "build invalidate rebuild SELECT");

    output_file = fopen(output_path, "wb");
    expect_true(output_file != NULL, "open invalidate rebuild output");
    if (output_file == NULL) {
        free_statement(&statement);
        return;
    }

    result = execute_statement(&statement, schema_dir, data_dir, output_file);
    fclose(output_file);
    expect_true(result.ok, "execute indexed SELECT before invalidate");
    expect_true(table_index_is_loaded("users"), "index loaded before invalidate");

    table_index_invalidate("users");
    expect_true(!table_index_is_loaded("users"), "invalidate unloads table index");

    output_file = fopen(output_path, "wb");
    expect_true(output_file != NULL, "reopen invalidate rebuild output");
    if (output_file == NULL) {
        free_statement(&statement);
        return;
    }

    result = execute_statement(&statement, schema_dir, data_dir, output_file);
    fclose(output_file);
    expect_true(result.ok, "execute indexed SELECT after invalidate");
    expect_true(table_index_is_loaded("users"), "invalidate path rebuilds table index");
    output_text = read_entire_file(output_path, error, sizeof(error));
    expect_true(output_text != NULL, "read invalidate rebuild output");
    if (output_text != NULL) {
        expect_true(strstr(output_text, "Bob") != NULL, "invalidate path returns rebuilt indexed result");
        free(output_text);
    }
    free_statement(&statement);
}

static void test_invalid_where_id_value(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    Statement statement = {0};
    ExecResult result;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create invalid id WHERE test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write invalid id WHERE schema");
    expect_true(write_text_file(data_path, "name,age\nAlice,20\n"), "write invalid id WHERE CSV");
    expect_true(load_statement("SELECT * FROM users WHERE id = abc;", &statement), "build invalid id WHERE SELECT");

    result = execute_statement(&statement, schema_dir, data_dir, stdout);
    expect_true(!result.ok, "non-integer id WHERE returns error");
    free_statement(&statement);
}

static void test_invalid_where_id_value_no_header_output(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char output_path[192];
    char error[256];
    char *output_text;
    Statement statement = {0};
    ExecResult result;
    FILE *output_file;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create invalid id WHERE output test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    build_child_path(output_path, sizeof(output_path), root, "invalid_id_output.txt");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write invalid id WHERE output schema");
    expect_true(write_text_file(data_path, "name,age\nAlice,20\n"), "write invalid id WHERE output CSV");
    expect_true(load_statement("SELECT name FROM users WHERE id = abc;", &statement), "build invalid id WHERE output SELECT");

    output_file = fopen(output_path, "wb");
    expect_true(output_file != NULL, "open output file for invalid id WHERE");
    if (output_file == NULL) {
        free_statement(&statement);
        return;
    }

    result = execute_statement(&statement, schema_dir, data_dir, output_file);
    fclose(output_file);
    expect_true(!result.ok, "invalid id WHERE fails before writing output");
    output_text = read_entire_file(output_path, error, sizeof(error));
    expect_true(output_text != NULL, "read invalid id WHERE output");
    if (output_text != NULL) {
        expect_true(output_text[0] == '\0', "invalid id WHERE does not print header");
        free(output_text);
    }
    free_statement(&statement);
}

static void test_invalid_rebuild_data(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    Statement statement = {0};
    ExecResult result;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create invalid rebuild test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write invalid rebuild schema");
    expect_true(write_text_file(data_path, "name,age\nAlice,20\nBob,21,extra\n"), "write malformed rebuild CSV");
    expect_true(load_statement("SELECT * FROM users WHERE id = 2;", &statement), "build malformed rebuild SELECT");

    result = execute_statement(&statement, schema_dir, data_dir, stdout);
    expect_true(!result.ok, "malformed row during indexed read returns error");
    free_statement(&statement);
}

static void test_schema_rejects_explicit_id_column(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    SchemaResult result;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create reserved id schema test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=id,name,age\n"), "write reserved id schema");
    expect_true(write_text_file(data_path, "id,name,age\n1,Alice,20\n"), "write reserved id CSV");

    result = load_schema(schema_dir, data_dir, "users");
    expect_true(!result.ok, "schema with explicit id column is rejected");
    expect_true(strstr(result.message, "reserved column name 'id'") != NULL, "schema rejection reports reserved id name");
}

static void test_insert_without_explicit_id_column(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    Statement statement = {0};
    ExecResult result;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create no id INSERT test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write no id INSERT schema");
    expect_true(write_text_file(data_path, "name,age\n"), "write no id INSERT CSV");
    expect_true(load_statement("INSERT INTO users (name) VALUES ('Alice');", &statement), "build no id INSERT");

    result = execute_statement(&statement, schema_dir, data_dir, stdout);
    expect_true(result.ok, "INSERT succeeds without explicit id column");
    free_statement(&statement);
}

static void test_select_where_id_without_explicit_id_column(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    Statement statement = {0};
    ExecResult result;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create no id SELECT test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write no id SELECT schema");
    expect_true(write_text_file(data_path, "name,age\nAlice,20\n"), "write no id SELECT CSV");
    expect_true(load_statement("SELECT * FROM users WHERE id = 1;", &statement), "build no id SELECT");

    result = execute_statement(&statement, schema_dir, data_dir, stdout);
    expect_true(result.ok, "WHERE id works without explicit id column");
    free_statement(&statement);
}

static void test_csv_escape(void) {
    StringList row = {0};
    char root[128];
    char data_dir[160];
    char schema_dir[160];
    char data_path[192];
    char error[256];
    char *csv_text;
    StorageResult result;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create CSV escape test directories");
    build_child_path(data_path, sizeof(data_path), data_dir, "notes.csv");
    expect_true(write_text_file(data_path, "text\n"), "write CSV escape header");
    expect_true(string_list_push(&row, "hello, \"world\""), "prepare CSV escape row");

    result = append_row_csv(data_dir, "notes", &row);
    expect_true(result.ok, "append CSV row with comma and quote");
    csv_text = read_entire_file(data_path, error, sizeof(error));
    expect_true(csv_text != NULL, "read CSV after escape write");
    if (csv_text != NULL) {
        expect_true(strstr(csv_text, "\"hello, \"\"world\"\"\"") != NULL, "CSV writer escapes quote and comma");
        free(csv_text);
    }
    string_list_free(&row);
}

static void test_storage_reports_missing_table_file(void) {
    StorageReadResult result;

    reset_runtime_state();
    result = read_row_at_offset_csv("build/tests/does_not_exist/data", "users", 0);
    expect_true(!result.ok, "read_row_at_offset_csv fails for missing table file");
    expect_true(strstr(result.message, "failed to open table file 'build/tests/does_not_exist/data/users.csv'") != NULL, "storage error reports missing table file path");
}

static void test_read_entire_file_reports_missing_sql_file(void) {
    char error[256];
    char *contents = read_entire_file("build/tests/does_not_exist/query.sql", error, sizeof(error));

    expect_true(contents == NULL, "read_entire_file fails for missing SQL file");
    expect_true(strstr(error, "failed to open SQL file 'build/tests/does_not_exist/query.sql'") != NULL, "read_entire_file reports missing SQL file path");
}

static void test_storage_row_offset_roundtrip(void) {
    StringList row = {0};
    char root[128];
    char data_dir[160];
    char schema_dir[160];
    char data_path[192];
    StorageResult append_result;
    StorageReadResult read_result;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create offset roundtrip test directories");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    expect_true(write_text_file(data_path, "id,name,age\n"), "write offset roundtrip header");
    expect_true(string_list_push(&row, "7"), "prepare offset row id");
    expect_true(string_list_push(&row, "Alice"), "prepare offset row name");
    expect_true(string_list_push(&row, "20"), "prepare offset row age");

    append_result = append_row_csv(data_dir, "users", &row);
    expect_true(append_result.ok, "append row for offset roundtrip");
    read_result = read_row_at_offset_csv(data_dir, "users", append_result.row_offset);
    expect_true(read_result.ok, "read row back by stored offset");
    if (read_result.ok) {
        expect_true(read_result.fields.count == 3, "offset roundtrip returns expected column count");
        expect_true(strcmp(read_result.fields.items[0], "7") == 0, "offset roundtrip preserves id");
        expect_true(strcmp(read_result.fields.items[1], "Alice") == 0, "offset roundtrip preserves name");
        expect_true(strcmp(read_result.fields.items[2], "20") == 0, "offset roundtrip preserves age");
        string_list_free(&read_result.fields);
    }

    string_list_free(&row);
}

static void test_insert_failure_recovers_via_rebuild(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char output_path[192];
    char error[256];
    char *csv_text;
    char *output_text;
    Statement insert_statement = {0};
    Statement select_statement = {0};
    ExecResult result;
    FILE *output_file;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create forced register failure test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    build_child_path(output_path, sizeof(output_path), root, "forced_rebuild_output.txt");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write forced register failure schema");
    expect_true(write_text_file(data_path, "name,age\nBob,21\n"), "write forced register failure CSV");
    expect_true(load_statement("INSERT INTO users (name) VALUES ('Alice');", &insert_statement), "build forced register failure INSERT");

    table_index_force_next_register_failure();
    result = execute_statement(&insert_statement, schema_dir, data_dir, stdout);
    expect_true(!result.ok, "forced index registration failure returns error");
    expect_true(!table_index_is_loaded("users"), "failed registration invalidates in-memory index");
    csv_text = read_entire_file(data_path, error, sizeof(error));
    expect_true(csv_text != NULL, "read CSV after forced index registration failure");
    if (csv_text != NULL) {
        expect_true(strstr(csv_text, "Alice,\"\"") != NULL, "CSV keeps appended row after registration failure");
        free(csv_text);
    }
    free_statement(&insert_statement);

    expect_true(load_statement("SELECT name FROM users WHERE id = 2;", &select_statement), "build SELECT after forced register failure");
    output_file = fopen(output_path, "wb");
    expect_true(output_file != NULL, "open output after forced register failure");
    if (output_file == NULL) {
        free_statement(&select_statement);
        return;
    }

    result = execute_statement(&select_statement, schema_dir, data_dir, output_file);
    fclose(output_file);
    expect_true(result.ok, "rebuild after forced registration failure succeeds");
    expect_true(table_index_is_loaded("users"), "rebuild after forced registration failure reloads index");
    output_text = read_entire_file(output_path, error, sizeof(error));
    expect_true(output_text != NULL, "read output after forced register failure recovery");
    if (output_text != NULL) {
        expect_true(strstr(output_text, "Alice") != NULL, "rebuild after forced registration failure finds appended row");
        free(output_text);
    }
    free_statement(&select_statement);
}

static void test_benchmark_main_resets_dataset(void) {
    char root[128];
    char schema_dir[160];
    char data_dir[160];
    char schema_path[192];
    char data_path[192];
    char error[256];
    char *prepared_text;
    char *query_before_text;
    char *query_after_text;
    char *prepare_argv[] = {"benchmark_runner", "prepare", schema_dir, data_dir, "users", "5"};
    char *query_argv[] = {"benchmark_runner", "query-only", schema_dir, data_dir, "users", "5", "3"};
    int result_code;

    reset_runtime_state();
    expect_true(create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir)), "create benchmark test directories");
    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    expect_true(write_text_file(schema_path, "table=users\ncolumns=name,age\n"), "write benchmark schema");
    expect_true(write_text_file(data_path, "name,age\nstale,55\n"), "write stale benchmark CSV");

    result_code = benchmark_main(6, prepare_argv);
    expect_true(result_code == 0, "benchmark prepare mode runs with dedicated benchmark dataset");
    prepared_text = read_entire_file(data_path, error, sizeof(error));
    expect_true(prepared_text != NULL, "read benchmark CSV after prepare mode");
    if (prepared_text != NULL) {
        expect_true(strstr(prepared_text, "99,stale,55") == NULL, "prepare mode resets old benchmark data");
        expect_true(count_lines(prepared_text) == 6, "prepare mode writes header plus requested rows");
        expect_true(strstr(prepared_text, "name_5,25") != NULL, "prepare mode writes reproducible generated rows");
    }

    query_before_text = read_entire_file(data_path, error, sizeof(error));
    expect_true(query_before_text != NULL, "read benchmark CSV before query-only mode");

    result_code = benchmark_main(7, query_argv);
    expect_true(result_code == 0, "benchmark query-only mode runs with prepared dataset");
    query_after_text = read_entire_file(data_path, error, sizeof(error));
    expect_true(query_after_text != NULL, "read benchmark CSV after query-only mode");
    if (query_before_text != NULL && query_after_text != NULL) {
        expect_true(strcmp(query_before_text, query_after_text) == 0, "query-only mode does not reset prepared dataset");
    }

    result_code = benchmark_main(7, query_argv);
    expect_true(result_code == 0, "benchmark query-only mode reruns on same dataset");

    free(prepared_text);
    free(query_before_text);
    free(query_after_text);
}

int main(void) {
    test_bptree_insert_and_search();
    test_bptree_split_preserves_searchability();
    test_parser_where();
    test_parser_error_details();
    test_parser_utf8_identifiers();
    test_cli_error_messages();
    test_cli_bare_directory_argument_is_not_sql();
    test_cli_success_output_includes_elapsed_time();
    test_schema_loading_with_alias_filename();
    test_schema_reports_missing_directory();
    test_schema_reports_alias_candidate_open_failure();
    test_insert_auto_id();
    test_insert_overrides_user_id();
    test_select_execution_with_general_where();
    test_select_execution_with_utf8_display_width();
    test_select_execution_with_id_index();
    test_select_explicit_internal_id_column();
    test_select_star_hides_internal_id_column();
    test_index_rebuild_after_reset();
    test_index_rebuild_after_invalidate();
    test_invalid_where_id_value();
    test_invalid_where_id_value_no_header_output();
    test_invalid_rebuild_data();
    test_schema_rejects_explicit_id_column();
    test_insert_without_explicit_id_column();
    test_select_where_id_without_explicit_id_column();
    test_csv_escape();
    test_storage_reports_missing_table_file();
    test_read_entire_file_reports_missing_sql_file();
    test_storage_row_offset_roundtrip();
    test_insert_failure_recovers_via_rebuild();
    test_benchmark_main_resets_dataset();
    tests_run++;
    if (run_api_server_tests() != 0) {
        tests_failed++;
        fprintf(stderr, "[FAIL] API server integration tests\n");
    } else {
        printf("[PASS] API server integration tests\n");
    }

    table_index_registry_reset();
    printf("Tests run: %d\n", tests_run);
    printf("Tests failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
