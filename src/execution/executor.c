/*
 * execution/executor.c
 *
 * executor는 parser가 만든 AST를 실제 동작으로 연결하는 계층이다.
 * 초심자 관점에서는 "SQL 문장의 의미를 실제 파일 작업으로 번역하는 오케스트레이터"다.
 *
 * 이 파일의 핵심 책임:
 * - INSERT / SELECT 분기
 * - INSERT 시 자동 id 생성 흐름 조정
 * - WHERE id일 때 인덱스 경로 선택
 * - 일반 SELECT / 일반 WHERE일 때 기존 CSV 스캔 경로 유지
 */
#include "sqlparser/execution/executor.h"

#include "sqlparser/common/util.h"
#include "sqlparser/index/table_index.h"
#include "sqlparser/storage/schema.h"
#include "sqlparser/storage/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    StringList *rows;
    int count;
    int capacity;
} SelectRowBuffer;

/*
 * SELECT 목록에서만 쓰는 가상 인덱스 값이다.
 *
 * 일반 컬럼은 schema.columns 안의 실제 위치(0 이상)를 사용하지만,
 * `SELECT id`는 CSV에 없는 숨은 내부 PK `__internal_id`를 뜻하므로
 * 실제 스키마 인덱스로 표현할 수 없다.
 *
 * 그래서 -1을 "실제 CSV 컬럼이 아닌 내부 id를 출력하라"는 특수 표식으로 쓴다.
 */
#define INTERNAL_ID_SELECT_INDEX (-1)

/* 실행 실패 결과를 공통 형식으로 채우는 작은 헬퍼 함수다. */
static void set_exec_error(ExecResult *result, const char *message) {
    result->ok = 0;
    snprintf(result->message, sizeof(result->message), "%s", message);
}

/* CSV 한 칸 값에 줄바꿈이 들어 있는지 검사한다. */
static int contains_newline(const char *value) {
    return strchr(value, '\n') != NULL || strchr(value, '\r') != NULL;
}

/* INSERT에 들어온 컬럼 목록과 값 목록이 스키마 기준으로 유효한지 검사한다. */
static int validate_insert_columns(const InsertStatement *statement, const Schema *schema, char *message, size_t message_size) {
    int index;

    /* INSERT 문법상 컬럼 수와 값 수가 다르면 어떤 행도 만들 수 없다. */
    if (statement->columns.count != statement->values.count) {
        snprintf(message, message_size, "column count and value count do not match");
        return 0;
    }

    for (index = 0; index < statement->columns.count; index++) {
        /*
         * 7주차 설계에서는 `id`를 사용자 컬럼이 아니라 시스템 내부 키로 본다.
         * 따라서 사용자가 INSERT 목록에 id를 직접 넣는 것은 허용하지 않는다.
         */
        if (strcmp(statement->columns.items[index], "id") == 0) {
            snprintf(message, message_size, "explicit id column is not allowed");
            return 0;
        }

        /* 사용자 스키마에 존재하지 않는 컬럼을 INSERT 대상으로 쓰면 오류다. */
        if (schema_find_column(schema, statement->columns.items[index]) < 0) {
            snprintf(message, message_size, "unknown column in INSERT: %s", statement->columns.items[index]);
            return 0;
        }

        /*
         * 현재 CSV writer는 한 셀 안의 줄바꿈을 지원하지 않는다.
         * 이 제약을 여기서 먼저 막아 두면 storage 계층이 단순해진다.
         */
        if (contains_newline(statement->values.items[index])) {
            snprintf(message, message_size, "newline in value is not supported");
            return 0;
        }
    }

    return 1;
}

/*
 * 사용자가 준 INSERT 입력을 스키마 순서에 맞는 "완성 행"으로 바꾼다.
 *
 * 예:
 * - 사용자는 name만 넣을 수 있다.
 * - 누락 컬럼은 빈 문자열로 채운다.
 * - 내부 PK는 CSV 바깥의 인덱스 계층이 관리하므로 CSV 행에는 쓰지 않는다.
 */
static int build_insert_row(const InsertStatement *statement, const Schema *schema, StringList *row_values, char *message, size_t message_size) {
    int *assigned = NULL;
    int schema_index;
    int value_index;

    /*
     * schema.columns.count 길이만큼 "이 컬럼에 값이 이미 들어갔는지" 표시하는 배열이다.
     * INSERT 문에 같은 컬럼이 두 번 나오면 duplicate column 오류를 내기 위해 쓴다.
     */
    assigned = (int *)calloc((size_t)schema->columns.count, sizeof(int));
    if (assigned == NULL) {
        snprintf(message, message_size, "out of memory while preparing INSERT row");
        return 0;
    }

    /*
     * 먼저 스키마 순서대로 빈 문자열 슬롯을 전부 만든다.
     * 이후 사용자가 실제로 지정한 컬럼 위치만 덮어쓴다.
     *
     * 이렇게 해 두면 부분 INSERT도 항상 "CSV 헤더 순서와 같은 길이의 완성 행"으로 만들 수 있다.
     */
    for (schema_index = 0; schema_index < schema->columns.count; schema_index++) {
        if (!string_list_push(row_values, "")) {
            free(assigned);
            string_list_free(row_values);
            snprintf(message, message_size, "out of memory while preparing INSERT row");
            return 0;
        }
    }

    for (value_index = 0; value_index < statement->columns.count; value_index++) {
        /* 사용자가 준 컬럼명을 실제 스키마 인덱스로 바꾼다. */
        schema_index = schema_find_column(schema, statement->columns.items[value_index]);
        if (schema_index < 0) {
            free(assigned);
            string_list_free(row_values);
            snprintf(message, message_size, "unknown column in INSERT: %s", statement->columns.items[value_index]);
            return 0;
        }

        /* 같은 컬럼이 두 번 나오면 어느 값을 써야 할지 모호하므로 즉시 실패한다. */
        if (assigned[schema_index]) {
            free(assigned);
            string_list_free(row_values);
            snprintf(message, message_size, "duplicate column in INSERT: %s", statement->columns.items[value_index]);
            return 0;
        }

        assigned[schema_index] = 1;

        /*
         * 기본값("")으로 채워 둔 슬롯을 실제 INSERT 값으로 교체한다.
         * copy_string을 쓰는 이유는 AST 내부 문자열 수명과 CSV용 행 버퍼 수명을 분리하기 위해서다.
         */
        free(row_values->items[schema_index]);
        row_values->items[schema_index] = copy_string(statement->values.items[value_index]);
        if (row_values->items[schema_index] == NULL) {
            free(assigned);
            string_list_free(row_values);
            snprintf(message, message_size, "out of memory while preparing INSERT row");
            return 0;
        }
    }

    free(assigned);
    return 1;
}

/*
 * INSERT 실행의 전체 흐름을 담당한다.
 *
 * 순서:
 * 1. schema 로딩
 * 2. 컬럼/값 검증
 * 3. next_id 계산
 * 4. CSV에 쓸 행 구성
 * 5. CSV append
 * 6. 인덱스 등록
 */
static ExecResult execute_insert(const InsertStatement *statement, const char *schema_dir, const char *data_dir) {
    ExecResult result = {0};
    SchemaResult schema_result;
    StringList row_values = {0};
    StorageResult storage_result;
    int next_id;

    /* 1. 대상 테이블 스키마를 읽고, 사용자 컬럼 구조를 확정한다. */
    schema_result = load_schema(schema_dir, data_dir, statement->table_name);
    if (!schema_result.ok) {
        set_exec_error(&result, schema_result.message);
        return result;
    }

    /* 2. INSERT 컬럼/값이 현재 숨은 내부 PK 정책과 스키마 제약을 만족하는지 검증한다. */
    if (!validate_insert_columns(statement, &schema_result.schema, result.message, sizeof(result.message))) {
        free_schema(&schema_result.schema);
        return result;
    }

    /*
     * 3. 다음 내부 PK를 구한다.
     * 이 값은 CSV 셀로 저장하지 않고, append 성공 후 인덱스 등록에만 사용한다.
     */
    if (!table_index_get_next_id(&schema_result.schema, data_dir, &next_id, result.message, sizeof(result.message))) {
        free_schema(&schema_result.schema);
        return result;
    }

    /* 4. 사용자 컬럼 기준 CSV 한 행을 조립한다. */
    if (!build_insert_row(statement, &schema_result.schema, &row_values, result.message, sizeof(result.message))) {
        free_schema(&schema_result.schema);
        return result;
    }

    /* 5. 실제 CSV 파일 끝에 한 행을 append 한다. */
    storage_result = append_row_csv(data_dir, schema_result.schema.storage_name, &row_values);
    string_list_free(&row_values);
    if (!storage_result.ok) {
        free_schema(&schema_result.schema);
        set_exec_error(&result, storage_result.message);
        return result;
    }

    /*
     * 6. append가 성공했으면 "내부 id -> CSV offset" 매핑을 메모리 인덱스에 등록한다.
     * 등록이 실패하면 CSV는 이미 써졌으므로, 인덱스만 invalidate 하여 다음 조회 때 재구성하게 만든다.
     */
    if (!table_index_register_row(&schema_result.schema, data_dir, next_id, storage_result.row_offset, result.message, sizeof(result.message))) {
        table_index_invalidate(schema_result.schema.storage_name);
        free_schema(&schema_result.schema);
        return result;
    }

    free_schema(&schema_result.schema);
    result.ok = 1;
    result.affected_rows = storage_result.affected_rows;
    snprintf(result.message, sizeof(result.message), "%s", storage_result.message);
    return result;
}

/* SELECT 결과에서 어떤 컬럼을 어떤 순서로 출력할지 계산한다. */
static int build_select_indexes(const SelectStatement *statement, const Schema *schema, StringList *selected_headers, int *selected_indexes, char *message, size_t message_size) {
    int index;
    int schema_index;

    if (statement->select_all) {
        /*
         * SELECT * 는 "사용자 스키마에 있는 컬럼 전체"를 뜻한다.
         * 숨은 내부 PK는 사용자 컬럼이 아니므로 여기서 자동 포함하지 않는다.
         */
        for (index = 0; index < schema->columns.count; index++) {
            if (!string_list_push(selected_headers, schema->columns.items[index])) {
                snprintf(message, message_size, "out of memory while preparing SELECT");
                return 0;
            }
            selected_indexes[index] = index;
        }
        return schema->columns.count;
    }

    for (index = 0; index < statement->columns.count; index++) {
        /*
         * SELECT id 는 예외적으로 허용한다.
         * 이 경우 id는 실제 CSV 컬럼이 아니라 내부 `__internal_id`를 보여 달라는 의미다.
         */
        if (strcmp(statement->columns.items[index], "id") == 0) {
            if (!string_list_push(selected_headers, "id")) {
                snprintf(message, message_size, "out of memory while preparing SELECT");
                return 0;
            }

            selected_indexes[index] = INTERNAL_ID_SELECT_INDEX;
            continue;
        }

        /*
         * 일반 컬럼은 실제 스키마 위치를 찾아서 그대로 출력 대상으로 등록한다.
         * 나중에 row 조립 단계에서는 이 인덱스를 사용해 CSV 필드 배열에서 값을 꺼낸다.
         */
        schema_index = schema_find_column(schema, statement->columns.items[index]);
        if (schema_index < 0) {
            snprintf(message, message_size, "unknown column in SELECT: %s", statement->columns.items[index]);
            return 0;
        }

        if (!string_list_push(selected_headers, statement->columns.items[index])) {
            snprintf(message, message_size, "out of memory while preparing SELECT");
            return 0;
        }

        selected_indexes[index] = schema_index;
    }

    return statement->columns.count;
}

/* WHERE 절에 사용된 컬럼이 스키마에서 몇 번째인지 찾는다. */
static int resolve_where_index(const SelectStatement *statement, const Schema *schema, int *where_index, char *message, size_t message_size) {
    if (!statement->has_where) {
        *where_index = -1;
        return 1;
    }

    /*
     * WHERE id 는 사용자 컬럼 검색이 아니라 숨은 내부 PK 검색이다.
     * 그래서 일반 스키마 인덱스를 찾지 않고, 별도 인덱스 경로로 넘기기 위해 -1을 유지한다.
     */
    if (strcmp(statement->where_column, "id") == 0) {
        *where_index = -1;
        return 1;
    }

    /* 그 밖의 WHERE 는 실제 스키마 컬럼이어야 한다. */
    *where_index = schema_find_column(schema, statement->where_column);
    if (*where_index < 0) {
        snprintf(message, message_size, "unknown column in WHERE: %s", statement->where_column);
        return 0;
    }

    return 1;
}

/* 현재 CSV 행이 WHERE 조건과 일치하는지 검사한다. */
static int row_matches_where(const SelectStatement *statement, const StringList *fields, int where_index) {
    if (!statement->has_where) {
        return 1;
    }

    /* 일반 WHERE 는 CSV에서 읽은 셀 문자열을 그대로 비교한다. */
    return strcmp(fields->items[where_index], statement->where_value) == 0;
}

/* 표 렌더링이 끝나면 버퍼에 임시로 모아 둔 행 복사본을 해제한다. */
static void select_row_buffer_free(SelectRowBuffer *buffer) {
    int index;

    for (index = 0; index < buffer->count; index++) {
        string_list_free(&buffer->rows[index]);
    }

    free(buffer->rows);
    buffer->rows = NULL;
    buffer->count = 0;
    buffer->capacity = 0;
}

/* 현재 행의 선택된 컬럼만 복사해 표 버퍼에 보관한다. */
static int select_row_buffer_push(SelectRowBuffer *buffer, const StringList *fields, const int *selected_indexes, int selected_count, int internal_id, char *message, size_t message_size) {
    int new_capacity;
    StringList *new_rows;
    StringList row = {0};
    int index;
    char internal_id_text[32];

    if (buffer->count == buffer->capacity) {
        /* 출력 대상 행 수를 미리 알 수 없으므로, 벡터처럼 점진적으로 버퍼를 늘린다. */
        new_capacity = buffer->capacity == 0 ? 4 : buffer->capacity * 2;
        new_rows = (StringList *)realloc(buffer->rows, (size_t)new_capacity * sizeof(StringList));
        if (new_rows == NULL) {
            snprintf(message, message_size, "out of memory while preparing SELECT output");
            return 0;
        }
        buffer->rows = new_rows;
        buffer->capacity = new_capacity;
    }

    /*
     * fields는 "CSV 한 줄 전체"이고, row는 "사용자가 실제로 보고 싶어 하는 출력 컬럼만
     * 골라 담은 결과 행"이다.
     *
     * 이 둘을 분리해 두면:
     * - SELECT * 와 SELECT id, name 같은 경우를 같은 코드로 처리할 수 있고
     * - 내부 id처럼 CSV에 없는 가상 컬럼도 자연스럽게 끼워 넣을 수 있다.
     */
    for (index = 0; index < selected_count; index++) {
        if (selected_indexes[index] == INTERNAL_ID_SELECT_INDEX) {
            /*
             * 가상 컬럼 id 는 현재 행의 내부 PK를 문자열로 바꿔 넣는다.
             * CSV에는 존재하지 않는 값이므로 여기서 합성한다.
             */
            snprintf(internal_id_text, sizeof(internal_id_text), "%d", internal_id);
            if (!string_list_push(&row, internal_id_text)) {
                string_list_free(&row);
                snprintf(message, message_size, "out of memory while preparing SELECT output");
                return 0;
            }
            continue;
        }

        /* 일반 컬럼은 CSV에서 읽은 필드를 그대로 복사한다. */
        if (!string_list_push(&row, fields->items[selected_indexes[index]])) {
            string_list_free(&row);
            snprintf(message, message_size, "out of memory while preparing SELECT output");
            return 0;
        }
    }

    buffer->rows[buffer->count] = row;
    buffer->count++;
    return 1;
}

/* 단순 문자열 길이를 기준으로 각 컬럼의 출력 폭을 계산한다. */
static void compute_table_widths(const StringList *headers, const SelectRowBuffer *rows, int *column_widths) {
    int column_index;
    int row_index;
    size_t width;

    for (column_index = 0; column_index < headers->count; column_index++) {
        /* 처음에는 헤더 길이를 최소 너비로 잡는다. */
        column_widths[column_index] = (int)strlen(headers->items[column_index]);
    }

    for (row_index = 0; row_index < rows->count; row_index++) {
        for (column_index = 0; column_index < headers->count; column_index++) {
            /* 데이터 셀이 더 길면 그 길이에 맞춰 폭을 넓힌다. */
            width = strlen(rows->rows[row_index].items[column_index]);
            if ((int)width > column_widths[column_index]) {
                column_widths[column_index] = (int)width;
            }
        }
    }
}

/* +-----+ 같은 ASCII 테두리 한 줄을 출력한다. */
static void print_table_border(FILE *out, const int *column_widths, int column_count) {
    int column_index;
    int width_index;

    fputc('+', out);
    for (column_index = 0; column_index < column_count; column_index++) {
        for (width_index = 0; width_index < column_widths[column_index] + 2; width_index++) {
            fputc('-', out);
        }
        fputc('+', out);
    }
    fputc('\n', out);
}

/* 표의 한 행을 고정 폭 셀 형식으로 출력한다. */
static void print_table_row(FILE *out, const StringList *row, const int *column_widths) {
    int column_index;
    int padding;
    size_t cell_width;

    fputc('|', out);
    for (column_index = 0; column_index < row->count; column_index++) {
        fprintf(out, " %s", row->items[column_index]);
        cell_width = strlen(row->items[column_index]);
        padding = column_widths[column_index] - (int)cell_width;
        while (padding-- > 0) {
            fputc(' ', out);
        }
        fprintf(out, " |");
    }
    fputc('\n', out);
}

/* 헤더와 데이터 전체를 받아 ASCII 테두리 표를 한 번에 출력한다. */
static int print_result_table(FILE *out, const StringList *headers, const SelectRowBuffer *rows, char *message, size_t message_size) {
    int *column_widths;
    int row_index;

    column_widths = (int *)calloc((size_t)headers->count, sizeof(int));
    if (column_widths == NULL) {
        snprintf(message, message_size, "out of memory while preparing SELECT output");
        return 0;
    }

    /*
     * 출력 로직은 "실제 조회"와 분리해 두었다.
     * 이렇게 하면 일반 SELECT와 인덱스 SELECT가 같은 표 렌더러를 재사용할 수 있다.
     */
    /*
     * 표 출력은 항상
     * 1. 열 너비 계산
     * 2. 상단선
     * 3. 헤더
     * 4. 헤더 아래 구분선
     * 5. 데이터 행들
     * 6. 하단선
     * 순서로 고정한다.
     *
     * 빈 결과도 같은 형식을 유지하므로, 사용자는 "데이터가 0건"인 상황과
     * "출력 자체가 깨진 상황"을 쉽게 구분할 수 있다.
     */
    compute_table_widths(headers, rows, column_widths);
    print_table_border(out, column_widths, headers->count);
    print_table_row(out, headers, column_widths);
    print_table_border(out, column_widths, headers->count);
    for (row_index = 0; row_index < rows->count; row_index++) {
        print_table_row(out, &rows->rows[row_index], column_widths);
    }
    print_table_border(out, column_widths, headers->count);

    free(column_widths);
    return 1;
}

/*
 * WHERE id = ... 전용 인덱스 조회 경로를 수행한다.
 *
 * 일반 SELECT와 달리:
 * - id 값을 정수로 검증하고
 * - B+ 트리에서 오프셋을 찾고
 * - 해당 행 하나만 읽는다.
 */
static int execute_index_select(const SelectStatement *statement, const Schema *schema, const char *data_dir, FILE *out, const StringList *headers, const int *selected_indexes, int selected_count, ExecResult *result) {
    TableIndexLookupResult lookup;
    StorageReadResult read_result;
    SelectRowBuffer rows = {0};
    int where_id;

    /*
     * WHERE id 경로는 숨은 내부 PK를 정수 키로 사용한다.
     * 따라서 비교 값이 정수가 아니면 B+ 트리 검색 자체를 수행할 수 없다.
     */
    if (!parse_int_strict(statement->where_value, &where_id)) {
        set_exec_error(result, "WHERE id value must be an integer");
        return 0;
    }

    /* B+ 트리에서 내부 id에 해당하는 CSV row offset을 찾는다. */
    lookup = table_index_find_row(schema, data_dir, where_id);
    if (!lookup.ok) {
        set_exec_error(result, lookup.message);
        return 0;
    }

    if (!lookup.found) {
        /* 인덱스에 키가 없으면 빈 표를 출력하되, 오류는 아니므로 SELECT 0으로 처리한다. */
        if (!print_result_table(out, headers, &rows, result->message, sizeof(result->message))) {
            return 0;
        }
        result->ok = 1;
        result->affected_rows = 0;
        snprintf(result->message, sizeof(result->message), "SELECT 0");
        return 1;
    }

    /* offset 기반 읽기는 "해당 한 행만" 읽으므로, 일반 CSV 전체 순회보다 빠르다. */
    read_result = read_row_at_offset_csv(data_dir, schema->storage_name, lookup.row_offset);
    if (!read_result.ok) {
        set_exec_error(result, read_result.message);
        return 0;
    }

    if (read_result.fields.count != schema->columns.count) {
        /* CSV 저장 형식이 스키마와 어긋나면 결과를 신뢰할 수 없으므로 즉시 실패한다. */
        string_list_free(&read_result.fields);
        set_exec_error(result, "CSV row does not match schema column count");
        return 0;
    }

    /* 단건 조회 결과를 공통 출력 버퍼 형식으로 변환한다. */
    if (!select_row_buffer_push(&rows, &read_result.fields, selected_indexes, selected_count, where_id, result->message, sizeof(result->message))) {
        string_list_free(&read_result.fields);
        return 0;
    }

    if (!print_result_table(out, headers, &rows, result->message, sizeof(result->message))) {
        string_list_free(&read_result.fields);
        select_row_buffer_free(&rows);
        return 0;
    }

    string_list_free(&read_result.fields);
    select_row_buffer_free(&rows);
    result->ok = 1;
    result->affected_rows = 1;
    snprintf(result->message, sizeof(result->message), "SELECT 1");
    return 1;
}

/*
 * SELECT 실행의 전체 흐름을 담당한다.
 *
 * 여기서 가장 중요한 분기:
 * - WHERE id 이면 execute_index_select
 * - 아니면 CSV 전체 스캔
 */
static ExecResult execute_select(const SelectStatement *statement, const char *schema_dir, const char *data_dir, FILE *out) {
    ExecResult result = {0};
    SchemaResult schema_result;
    char *path;
    FILE *file;
    char line[4096];
    StringList fields = {0};
    StringList headers = {0};
    SelectRowBuffer rows = {0};
    int *selected_indexes = NULL;
    int selected_count;
    int where_index = -1;
    int row_count = 0;
    int current_internal_id = 1;
    int row_internal_id;

    schema_result = load_schema(schema_dir, data_dir, statement->table_name);
    if (!schema_result.ok) {
        set_exec_error(&result, schema_result.message);
        return result;
    }

    /*
     * selected_indexes는 "출력 컬럼 목록"을 실행기용 인덱스로 바꾼 배열이다.
     * 여기에는 실제 CSV 컬럼 인덱스와 가상 INTERNAL_ID_SELECT_INDEX가 함께 들어갈 수 있다.
     */
    selected_indexes = (int *)calloc((size_t)schema_result.schema.columns.count, sizeof(int));
    if (selected_indexes == NULL) {
        free_schema(&schema_result.schema);
        set_exec_error(&result, "out of memory while preparing SELECT");
        return result;
    }

    selected_count = build_select_indexes(statement, &schema_result.schema, &headers, selected_indexes, result.message, sizeof(result.message));
    if (selected_count == 0) {
        free(selected_indexes);
        string_list_free(&headers);
        free_schema(&schema_result.schema);
        return result;
    }

    if (!resolve_where_index(statement, &schema_result.schema, &where_index, result.message, sizeof(result.message))) {
        free(selected_indexes);
        string_list_free(&headers);
        free_schema(&schema_result.schema);
        return result;
    }

    /*
     * WHERE id 는 일반 WHERE 와 완전히 다른 경로다.
     * CSV 전체를 순회하지 않고, 테이블 인덱스에서 바로 한 행을 찾는다.
     */
    if (statement->has_where && strcmp(statement->where_column, "id") == 0) {
        execute_index_select(statement, &schema_result.schema, data_dir, out, &headers, selected_indexes, selected_count, &result);
        free(selected_indexes);
        string_list_free(&headers);
        free_schema(&schema_result.schema);
        return result;
    }

    path = build_path(data_dir, schema_result.schema.storage_name, ".csv");
    if (path == NULL) {
        free(selected_indexes);
        string_list_free(&headers);
        free_schema(&schema_result.schema);
        set_exec_error(&result, "out of memory while opening table data");
        return result;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        free(selected_indexes);
        string_list_free(&headers);
        free_schema(&schema_result.schema);
        format_system_error(result.message, sizeof(result.message), "failed to open table data file", path);
        free(path);
        result.ok = 0;
        return result;
    }
    free(path);

    /*
     * 첫 줄은 CSV 헤더다.
     * load_schema() 단계에서 이미 스키마와 헤더의 일치 여부를 검증했으므로,
     * 여기서는 "비어 있는 파일인지" 정도만 다시 확인한다.
     */
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        free(selected_indexes);
        string_list_free(&headers);
        free_schema(&schema_result.schema);
        set_exec_error(&result, "table data file is empty");
        return result;
    }

    /*
     * 일반 SELECT 경로에서는 CSV 데이터 행을 앞에서부터 읽으면서
     * "몇 번째 데이터 행인가"를 기준으로 내부 PK를 다시 계산한다.
     *
     * 이 설계는 append-only 전제를 사용한다.
     * 즉 CSV 파일 안에 내부 id를 저장하지 않더라도,
     * 같은 파일을 같은 순서로 읽으면 같은 내부 PK를 다시 얻을 수 있다.
     */
    while (fgets(line, sizeof(line), file) != NULL) {
        strip_line_endings(line);
        if (line[0] == '\0') {
            continue;
        }

        /* 일반 SELECT 경로에서는 CSV를 한 줄씩 읽을 때마다 행 순서 기반 내부 id를 계산한다. */
        if (!csv_parse_line(line, &fields, result.message, sizeof(result.message))) {
            fclose(file);
            free(selected_indexes);
            string_list_free(&headers);
            string_list_free(&fields);
            free_schema(&schema_result.schema);
            result.ok = 0;
            return result;
        }

        if (fields.count != schema_result.schema.columns.count) {
            fclose(file);
            free(selected_indexes);
            string_list_free(&headers);
            string_list_free(&fields);
            free_schema(&schema_result.schema);
            set_exec_error(&result, "CSV row does not match schema column count");
            return result;
        }

        /*
         * 현재 데이터 행의 내부 PK를 확정한다.
         * 헤더 다음 첫 번째 데이터 행은 1, 그 다음은 2 ... 식으로 증가한다.
         */
        row_internal_id = current_internal_id;
        current_internal_id++;

        /* 일반 WHERE 조건이 있으면 현재 행을 통과시킬지 먼저 판단한다. */
        if (!row_matches_where(statement, &fields, where_index)) {
            string_list_free(&fields);
            continue;
        }

        /*
         * 출력 버퍼에는 CSV 행 그 자체가 아니라 "사용자가 요청한 컬럼만 뽑은 결과 행"을 넣는다.
         * 그래서 SELECT id, name 같은 경우도 같은 경로로 처리할 수 있다.
         */
        if (!select_row_buffer_push(&rows, &fields, selected_indexes, selected_count, row_internal_id, result.message, sizeof(result.message))) {
            fclose(file);
            free(selected_indexes);
            string_list_free(&headers);
            string_list_free(&fields);
            select_row_buffer_free(&rows);
            free_schema(&schema_result.schema);
            result.ok = 0;
            return result;
        }

        string_list_free(&fields);
        row_count++;
    }

    fclose(file);

    /*
     * 조회 자체가 끝난 뒤에는 항상 공통 표 렌더러로 마무리한다.
     * 그래서 일반 WHERE, 전체 조회, 0건 조회가 모두 같은 출력 형식을 공유한다.
     */
    if (!print_result_table(out, &headers, &rows, result.message, sizeof(result.message))) {
        free(selected_indexes);
        string_list_free(&headers);
        select_row_buffer_free(&rows);
        free_schema(&schema_result.schema);
        result.ok = 0;
        return result;
    }

    free(selected_indexes);
    string_list_free(&headers);
    select_row_buffer_free(&rows);
    free_schema(&schema_result.schema);

    result.ok = 1;
    result.affected_rows = row_count;
    snprintf(result.message, sizeof(result.message), "SELECT %d", row_count);
    return result;
}

/* Statement 종류에 따라 INSERT 또는 SELECT 실행 함수로 분기하는 진입점이다. */
ExecResult execute_statement(const Statement *statement, const char *schema_dir, const char *data_dir, FILE *out) {
    /* AST 문장 종류에 따라 실행 경로를 한 번만 결정하는 진입점이다. */
    if (statement->type == STATEMENT_INSERT) {
        return execute_insert(&statement->as.insert_statement, schema_dir, data_dir);
    }

    return execute_select(&statement->as.select_statement, schema_dir, data_dir, out);
}

/* 실행 계층이 내부적으로 쓰는 런타임 상태(현재는 인덱스 레지스트리)를 정리한다. */
void execution_runtime_reset(void) {
    /*
     * execution 계층이 가진 런타임 상태는 현재 테이블 인덱스 레지스트리다.
     * 프로그램 종료나 테스트 사이클마다 이를 비워 두어 다음 실행이 깨끗한 상태에서 시작하게 한다.
     */
    table_index_registry_reset();
}
