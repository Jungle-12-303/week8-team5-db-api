/*
 * benchmark/benchmark_main.c
 *
 * 이 파일은 일반 CLI와 분리된 성능 측정 전용 진입점이다.
 *
 * 7주차 발표 흐름에 맞춰 두 모드를 지원한다.
 * - prepare: 대량 데이터를 미리 생성하고 INSERT 시간만 측정
 * - query-only: 이미 준비된 데이터셋을 그대로 사용해 조회 시간만 비교
 */
#include "sqlparser/benchmark/benchmark.h"

#include "sqlparser/common/util.h"
#include "sqlparser/execution/executor.h"
#include "sqlparser/index/table_index.h"
#include "sqlparser/storage/schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 벤치마크 시작 전에 CSV를 헤더만 남긴 초기 상태로 다시 만든다. */
static int write_csv_header(const Schema *schema, const char *data_dir, char *message, size_t message_size) {
    char *path;
    FILE *file;
    int index;

    /* benchmark-workdir/data/<table>.csv 경로를 만든다. */
    path = build_path(data_dir, schema->storage_name, ".csv");
    if (path == NULL) {
        snprintf(message, message_size, "out of memory while building benchmark CSV path");
        return 0;
    }

    /* wb 모드로 열면 기존 내용을 지우고 새 파일처럼 다시 쓸 수 있다. */
    file = fopen(path, "wb");
    free(path);
    if (file == NULL) {
        snprintf(message, message_size, "failed to open benchmark CSV for reset");
        return 0;
    }

    /* 스키마에 정의된 컬럼 순서 그대로 CSV 헤더 한 줄을 다시 쓴다. */
    for (index = 0; index < schema->columns.count; index++) {
        if (index > 0) {
            fputc(',', file);
        }
        fputs(schema->columns.items[index], file);
    }
    fputc('\n', file);

    if (fclose(file) != 0) {
        snprintf(message, message_size, "failed to finalize benchmark CSV reset");
        return 0;
    }

    return 1;
}

/* 벤치마크용 더미 데이터 값을 컬럼 이름과 행 번호 기준으로 재현 가능하게 만든다. */
static char *make_value_text(const char *column_name, int row_number) {
    char buffer[128];

    /*
     * age는 숫자 컬럼처럼 보이도록 정수 값을 만들고,
     * 나머지 컬럼은 columnName_rowNumber 규칙으로 반복 가능한 더미 데이터를 만든다.
     */
    if (strcmp(column_name, "age") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", 20 + (row_number % 50));
    } else {
        snprintf(buffer, sizeof(buffer), "%s_%d", column_name, row_number);
    }

    return copy_string(buffer);
}

/* 벤치마크 중 임시로 만든 Statement를 정리하고 구조체를 초기화한다. */
static void free_generated_statement(Statement *statement) {
    free_statement(statement);
    memset(statement, 0, sizeof(*statement));
}

/* 벤치마크용 INSERT Statement 하나를 자동으로 구성한다. */
static int build_insert_statement(const Schema *schema, int row_number, Statement *statement, char *message, size_t message_size) {
    int index;

    /* 매 반복마다 새 Statement를 깨끗한 상태에서 시작한다. */
    memset(statement, 0, sizeof(*statement));
    statement->type = STATEMENT_INSERT;
    statement->as.insert_statement.table_name = copy_string(schema->table_name);
    if (statement->as.insert_statement.table_name == NULL) {
        snprintf(message, message_size, "out of memory while building benchmark insert");
        return 0;
    }

    /*
     * 벤치마크 INSERT도 일반 SQL 처리기 경로를 그대로 타게 하기 위해
     * parser 결과와 같은 형태의 Statement를 직접 구성한다.
     */
    for (index = 0; index < schema->columns.count; index++) {
        char *value_text;

        if (!string_list_push(&statement->as.insert_statement.columns, schema->columns.items[index])) {
            free_generated_statement(statement);
            snprintf(message, message_size, "out of memory while building benchmark insert");
            return 0;
        }

        value_text = make_value_text(schema->columns.items[index], row_number);
        if (value_text == NULL || !string_list_push(&statement->as.insert_statement.values, value_text)) {
            free(value_text);
            free_generated_statement(statement);
            snprintf(message, message_size, "out of memory while building benchmark insert");
            return 0;
        }
        free(value_text);
    }

    return 1;
}

/* 벤치마크용 SELECT ... WHERE ... Statement 하나를 자동으로 구성한다. */
static int build_select_statement(const char *table_name, const char *column_name, const char *value, Statement *statement) {
    /* query-only 모드에서는 SELECT * FROM table WHERE column = value 형태를 직접 만든다. */
    memset(statement, 0, sizeof(*statement));
    statement->type = STATEMENT_SELECT;
    statement->as.select_statement.table_name = copy_string(table_name);
    statement->as.select_statement.where_column = copy_string(column_name);
    statement->as.select_statement.where_value = copy_string(value);
    statement->as.select_statement.select_all = 1;
    statement->as.select_statement.has_where = 1;

    return statement->as.select_statement.table_name != NULL &&
           statement->as.select_statement.where_column != NULL &&
           statement->as.select_statement.where_value != NULL;
}

/* 같은 질의를 여러 번 실행해 평균 시간을 계산한다. */
static double run_query_benchmark(const Statement *statement, const char *schema_dir, const char *data_dir, int repeat_count, char *message, size_t message_size) {
    FILE *sink;
    clock_t started;
    int index;

    /*
     * 실제 CLI 표 출력을 섞으면 시간 측정이 왜곡되므로
     * 임시 파일(tmpfile)을 sink로 두고 질의만 반복 실행한다.
     */
    sink = tmpfile();
    if (sink == NULL) {
        snprintf(message, message_size, "failed to create temporary output sink for benchmark query");
        return -1.0;
    }

    /*
     * 같은 질의를 여러 번 실행해 평균 시간을 구한다.
     * 한 번만 재면 캐시/스케줄링 영향이 커질 수 있기 때문이다.
     */
    started = clock();
    for (index = 0; index < repeat_count; index++) {
        ExecResult result = execute_statement(statement, schema_dir, data_dir, sink);
        if (!result.ok) {
            snprintf(message, message_size, "%s", result.message);
            fclose(sink);
            return -1.0;
        }
        /* 다음 반복에서도 같은 sink를 재사용할 수 있도록 파일 상태를 초기화한다. */
        clearerr(sink);
        rewind(sink);
    }

    fclose(sink);
    return ((double)(clock() - started) / (double)CLOCKS_PER_SEC) / (double)repeat_count;
}

/* 스키마에서 첫 번째 사용자 컬럼 이름을 찾는다. */
static const char *find_first_non_id_column(const Schema *schema) {
    int index;

    for (index = 0; index < schema->columns.count; index++) {
        return schema->columns.items[index];
    }

    return NULL;
}

/* prepare 모드 사용법을 출력한다. */
static void print_prepare_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s prepare <schema_dir> <data_dir> <table_name> <row_count>\n", program_name);
}

/* query-only 모드 사용법을 출력한다. */
static void print_query_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s query-only <schema_dir> <data_dir> <table_name> <target_id> [query_repeat]\n", program_name);
}

/* 명시적 모드 기반 전체 사용법을 출력한다. */
static void print_benchmark_usage(const char *program_name) {
    print_prepare_usage(program_name);
    print_query_usage(program_name);
}

/* prepare 모드: CSV를 초기화하고 대량 INSERT를 수행한다. */
static int run_prepare_mode(const char *schema_dir, const char *data_dir, const char *table_name, int row_count) {
    SchemaResult schema_result;
    Statement statement = {0};
    int index;
    double insert_time;
    clock_t insert_started;

    /* 대상 테이블 스키마를 읽어야 어떤 컬럼에 어떤 더미 값을 넣을지 결정할 수 있다. */
    schema_result = load_schema(schema_dir, data_dir, table_name);
    if (!schema_result.ok) {
        fprintf(stderr, "error: %s\n", schema_result.message);
        return 1;
    }

    /* 기존 benchmark CSV는 버리고 헤더만 남긴 새 데이터셋으로 시작한다. */
    if (!write_csv_header(&schema_result.schema, data_dir, schema_result.message, sizeof(schema_result.message))) {
        fprintf(stderr, "error: %s\n", schema_result.message);
        free_schema(&schema_result.schema);
        return 1;
    }

    /*
     * prepare는 "깨끗한 CSV + 빈 메모리 인덱스"에서 시작해야
     * 항상 같은 입력이면 같은 데이터셋을 만들 수 있다.
     */
    table_index_registry_reset();
    insert_started = clock();
    for (index = 0; index < row_count; index++) {
        ExecResult result;
        /* 1행씩 일반 INSERT 경로를 호출해 실제 시스템과 같은 삽입 과정을 재현한다. */
        if (!build_insert_statement(&schema_result.schema, index + 1, &statement, schema_result.message, sizeof(schema_result.message))) {
            fprintf(stderr, "error: %s\n", schema_result.message);
            free_schema(&schema_result.schema);
            return 1;
        }

        result = execute_statement(&statement, schema_dir, data_dir, stdout);
        free_generated_statement(&statement);
        if (!result.ok) {
            fprintf(stderr, "error: %s\n", result.message);
            free_schema(&schema_result.schema);
            return 1;
        }
    }
    insert_time = (double)(clock() - insert_started) / (double)CLOCKS_PER_SEC;

    printf("Prepared rows: %d\n", row_count);
    printf("Insert time: %.6f sec\n", insert_time);

    free_schema(&schema_result.schema);
    table_index_registry_reset();
    return 0;
}

/* query-only 모드: 이미 준비된 데이터셋에 대해 조회 시간만 측정한다. */
static int run_query_only_mode(const char *schema_dir, const char *data_dir, const char *table_name, int target_id, int query_repeat) {
    SchemaResult schema_result;
    Statement id_select = {0};
    Statement other_select = {0};
    char id_text[32];
    const char *other_column = NULL;
    char *other_value = NULL;
    double indexed_time;
    double linear_time;
    char indexed_error[256] = {0};
    char linear_error[256] = {0};

    /* query-only는 이미 준비된 CSV를 그대로 읽기만 하므로 우선 스키마만 로드한다. */
    schema_result = load_schema(schema_dir, data_dir, table_name);
    if (!schema_result.ok) {
        fprintf(stderr, "error: %s\n", schema_result.message);
        return 1;
    }

    /* 선형 탐색 비교를 위해 첫 번째 사용자 컬럼을 찾는다. */
    other_column = find_first_non_id_column(&schema_result.schema);
    if (other_column == NULL) {
        fprintf(stderr, "error: benchmark table must have at least one user column\n");
        free_schema(&schema_result.schema);
        return 1;
    }

    /*
     * 같은 레코드를 두 방식으로 찾기 위해
     * - WHERE id = <target_id>
     * - WHERE other_column = <generated value>
     * 두 질의를 만든다.
     */
    snprintf(id_text, sizeof(id_text), "%d", target_id);
    other_value = make_value_text(other_column, target_id);
    if (other_value == NULL ||
        !build_select_statement(schema_result.schema.table_name, "id", id_text, &id_select) ||
        !build_select_statement(schema_result.schema.table_name, other_column, other_value, &other_select)) {
        fprintf(stderr, "error: out of memory while preparing benchmark queries\n");
        free(other_value);
        free_generated_statement(&id_select);
        free_generated_statement(&other_select);
        free_schema(&schema_result.schema);
        return 1;
    }

    /*
     * 첫 id 질의가 인덱스 재구성 비용까지 먹지 않도록
     * 측정 시작 전 런타임 인덱스 상태를 명시적으로 초기화한다.
     */
    table_index_registry_reset();
    indexed_time = run_query_benchmark(&id_select, schema_dir, data_dir, query_repeat, indexed_error, sizeof(indexed_error));
    if (indexed_time < 0.0) {
        fprintf(stderr, "error: indexed benchmark query failed: %s\n", indexed_error);
        free(other_value);
        free_generated_statement(&id_select);
        free_generated_statement(&other_select);
        free_schema(&schema_result.schema);
        return 1;
    }

    linear_time = run_query_benchmark(&other_select, schema_dir, data_dir, query_repeat, linear_error, sizeof(linear_error));
    if (linear_time < 0.0) {
        fprintf(stderr, "error: linear benchmark query failed: %s\n", linear_error);
        free(other_value);
        free_generated_statement(&id_select);
        free_generated_statement(&other_select);
        free_schema(&schema_result.schema);
        return 1;
    }

    printf("Query target id: %d\n", target_id);
    printf("Query target column: %s\n", other_column);
    printf("Query target value: %s\n", other_value);
    printf("Query repeats: %d\n", query_repeat);
    printf("Indexed query avg time: %.6f sec\n", indexed_time);
    printf("Linear query avg time: %.6f sec\n", linear_time);

    free(other_value);
    free_generated_statement(&id_select);
    free_generated_statement(&other_select);
    free_schema(&schema_result.schema);
    table_index_registry_reset();
    return 0;
}

/*
 * 벤치마크 프로그램의 실제 메인 로직이다.
 *
 * prepare:
 * 1. schema 로딩
 * 2. CSV 초기화
 * 3. 대량 INSERT
 * 4. 삽입 시간 출력
 *
 * query-only:
 * 1. 기존 데이터셋 로딩
 * 2. 인덱스 조회와 일반 컬럼 조회 반복 측정
 * 3. 조회 시간만 출력
 */
int benchmark_main(int argc, char *argv[]) {
    const char *mode;
    int row_count;
    int target_id;
    int query_repeat = 100;

    if (argc < 2) {
        print_benchmark_usage(argv[0]);
        return 1;
    }

    mode = argv[1];

    if (strcmp(mode, "prepare") == 0) {
        if (argc != 6) {
            print_prepare_usage(argv[0]);
            return 1;
        }

        if (!parse_int_strict(argv[5], &row_count) || row_count <= 0) {
            fprintf(stderr, "error: row_count must be a positive integer\n");
            return 1;
        }

        return run_prepare_mode(argv[2], argv[3], argv[4], row_count);
    }

    if (strcmp(mode, "query-only") == 0) {
        if (argc != 6 && argc != 7) {
            print_query_usage(argv[0]);
            return 1;
        }

        if (!parse_int_strict(argv[5], &target_id) || target_id <= 0) {
            fprintf(stderr, "error: target_id must be a positive integer\n");
            return 1;
        }
        if (argc == 7 && (!parse_int_strict(argv[6], &query_repeat) || query_repeat <= 0)) {
            fprintf(stderr, "error: query_repeat must be a positive integer\n");
            return 1;
        }

        return run_query_only_mode(argv[2], argv[3], argv[4], target_id, query_repeat);
    }

    fprintf(stderr, "error: unknown benchmark mode: %s\n", mode);
    print_benchmark_usage(argv[0]);
    return 1;
}

#ifndef SQLPARSER_BENCHMARK_NO_MAIN
/* 독립 실행 바이너리일 때 사용하는 실제 main 함수다. */
int main(int argc, char *argv[]) {
    return benchmark_main(argc, argv);
}
#endif
