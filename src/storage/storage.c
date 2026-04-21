/*
 * storage/storage.c
 *
 * 이 파일은 실제 CSV 읽기/쓰기 로직을 담당한다.
 * schema.c가 "이 테이블이 유효한가"를 확인한다면,
 * storage.c는 "실제 파일에 어떻게 저장하고 읽는가"를 담당한다고 보면 된다.
 */
#include "sqlparser/storage/storage.h"

#include "sqlparser/common/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 동적 문자열 버퍼 뒤에 문자 하나를 붙이는 내부 헬퍼 함수다. */
static int append_character(char **buffer, size_t *length, size_t *capacity, char value) {
    char *new_buffer;
    size_t new_capacity;

    /* 문자를 하나 더 붙일 공간이 부족하면 버퍼를 두 배씩 늘린다. */
    if (*length + 1 >= *capacity) {
        new_capacity = *capacity == 0 ? 16 : *capacity * 2;
        new_buffer = (char *)realloc(*buffer, new_capacity);
        if (new_buffer == NULL) {
            return 0;
        }

        *buffer = new_buffer;
        *capacity = new_capacity;
    }

    (*buffer)[*length] = value;
    (*length)++;
    (*buffer)[*length] = '\0';
    return 1;
}

/* data/<table>.csv 파일을 열고, 실패 시 표준화된 오류 메시지를 만든다. */
static FILE *open_table_file(const char *data_dir, const char *table_name, const char *mode, char *error, size_t error_size) {
    char *path = build_path(data_dir, table_name, ".csv");
    FILE *file;

    if (path == NULL) {
        snprintf(error, error_size, "out of memory while building table path");
        return NULL;
    }

    file = fopen(path, mode);
    if (file == NULL) {
        format_system_error(error, error_size, "failed to open table file", path);
        free(path);
        return NULL;
    }

    free(path);

    return file;
}

/*
 * CSV 한 줄을 StringList 필드 목록으로 파싱한다.
 *
 * 큰따옴표 처리, 쉼표 구분, escaped quote("") 처리까지 담당한다.
 */
int csv_parse_line(const char *line, StringList *fields, char *error, size_t error_size) {
    int in_quotes = 0;
    int just_closed_quote = 0;
    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;
    const char *cursor;

    /*
     * 이 파서는 CSV 한 줄을 왼쪽부터 한 글자씩 읽는다.
     * 큰 규칙은:
     * - 따옴표 밖의 쉼표는 필드 구분자
     * - 따옴표 안의 쉼표는 데이터 본문
     * - "" 는 실제 큰따옴표 한 글자
     */
    for (cursor = line; ; cursor++) {
        char current = *cursor;
        char *field_text;

        if (in_quotes) {
            /* 따옴표 안에서는 쉼표도 일반 문자처럼 그대로 누적한다. */
            if (current == '"') {
                if (cursor[1] == '"') {
                    /* CSV 규칙상 "" 는 실제 큰따옴표 한 글자를 뜻한다. */
                    if (!append_character(&buffer, &length, &capacity, '"')) {
                        free(buffer);
                        snprintf(error, error_size, "out of memory while parsing CSV");
                        return 0;
                    }
                    cursor++;
                } else {
                    /* 닫는 따옴표를 만나면 이제 quoted field가 끝났다고 본다. */
                    in_quotes = 0;
                    just_closed_quote = 1;
                }
            } else if (current == '\0') {
                free(buffer);
                snprintf(error, error_size, "unterminated quoted CSV field");
                return 0;
            } else if (!append_character(&buffer, &length, &capacity, current)) {
                free(buffer);
                snprintf(error, error_size, "out of memory while parsing CSV");
                return 0;
            }
            continue;
        }

        if (current == '\0' || current == ',') {
            /* 쉼표나 줄 끝을 만나면 지금까지 모은 내용을 필드 하나로 확정한다. */
            if (buffer == NULL) {
                buffer = copy_string("");
                if (buffer == NULL) {
                    snprintf(error, error_size, "out of memory while parsing CSV");
                    return 0;
                }
            }

            /* quoted field였다면 원문 그대로, 아니면 앞뒤 공백을 정리한 값을 저장한다. */
            field_text = just_closed_quote ? buffer : trim_whitespace(buffer);
            if (!string_list_push(fields, field_text)) {
                free(buffer);
                snprintf(error, error_size, "out of memory while parsing CSV");
                return 0;
            }

            free(buffer);
            buffer = NULL;
            length = 0;
            capacity = 0;
            just_closed_quote = 0;

            if (current == '\0') {
                break;
            }
            continue;
        }

        /* 필드의 첫 문자가 큰따옴표이면 quoted field 모드로 들어간다. */
        if (current == '"' && buffer == NULL) {
            in_quotes = 1;
            continue;
        }

        if (just_closed_quote && current != ' ' && current != '\t') {
            free(buffer);
            snprintf(error, error_size, "unexpected character after quoted CSV field");
            return 0;
        }

        if (current != '\r' && current != '\n') {
            if (!append_character(&buffer, &length, &capacity, current)) {
                free(buffer);
                snprintf(error, error_size, "out of memory while parsing CSV");
                return 0;
            }
        }
    }

    return 1;
}

/* 문자열 하나를 CSV 규칙에 맞는 필드 표현으로 바꾼다. */
char *csv_escape_field(const char *value) {
    int needs_quotes = 0;
    size_t index;
    size_t extra_quotes = 0;
    size_t length = strlen(value);
    char *escaped;
    size_t write_index = 0;

    if (length == 0) {
        return copy_string("\"\"");
    }

    for (index = 0; index < length; index++) {
        if (value[index] == '"' || value[index] == ',' || value[index] == '\n' || value[index] == '\r') {
            needs_quotes = 1;
        }
        if (value[index] == '"') {
            extra_quotes++;
        }
    }

    if (!needs_quotes) {
        return copy_string(value);
    }

    escaped = (char *)malloc(length + extra_quotes + 3);
    if (escaped == NULL) {
        return NULL;
    }

    escaped[write_index++] = '"';
    for (index = 0; index < length; index++) {
        if (value[index] == '"') {
            escaped[write_index++] = '"';
        }
        escaped[write_index++] = value[index];
    }
    escaped[write_index++] = '"';
    escaped[write_index] = '\0';
    return escaped;
}

/*
 * 완성된 행 하나를 CSV 끝에 추가한다.
 *
 * 성공하면:
 * - INSERT 1 메시지
 * - 영향을 받은 행 수
 * - 새 행의 시작 오프셋
 * 을 함께 반환한다.
 */
StorageResult append_row_csv(const char *data_dir, const char *table_name, const StringList *row_values) {
    StorageResult result = {0};
    FILE *file = open_table_file(data_dir, table_name, "r+b", result.message, sizeof(result.message));
    int index;
    long file_size;
    int last_character;

    result.row_offset = -1;
    if (file == NULL) {
        return result;
    }

    /* 기존 파일 끝으로 이동해 새 행을 맨 뒤에 이어 붙인다. */
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        snprintf(result.message, sizeof(result.message), "failed to seek table file");
        return result;
    }

    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        snprintf(result.message, sizeof(result.message), "failed to measure table file");
        return result;
    }

    /*
     * 기존 파일이 마지막에 개행 없이 끝났다면
     * 새 행을 쓰기 전에 줄바꿈을 하나 보정해 준다.
     */
    if (file_size > 0) {
        if (fseek(file, -1L, SEEK_END) != 0) {
            fclose(file);
            snprintf(result.message, sizeof(result.message), "failed to inspect table file");
            return result;
        }

        last_character = fgetc(file);
        if (fseek(file, 0, SEEK_END) != 0) {
            fclose(file);
            snprintf(result.message, sizeof(result.message), "failed to rewind table file");
            return result;
        }

        if (last_character != '\n') {
            if (fputc('\n', file) == EOF) {
                fclose(file);
                snprintf(result.message, sizeof(result.message), "failed to write line separator");
                return result;
            }
        }
    }

    /* 이 시점의 파일 위치가 곧 "방금 추가될 행의 시작 오프셋"이다. */
    result.row_offset = ftell(file);
    if (result.row_offset < 0) {
        fclose(file);
        snprintf(result.message, sizeof(result.message), "failed to determine row offset");
        return result;
    }

    for (index = 0; index < row_values->count; index++) {
        char *escaped = csv_escape_field(row_values->items[index]);
        if (escaped == NULL) {
            fclose(file);
            snprintf(result.message, sizeof(result.message), "out of memory while writing CSV row");
            return result;
        }

        /* CSV 규칙에 맞게 escape한 다음 각 필드를 쉼표로 이어 쓴다. */
        if (index > 0 && fputc(',', file) == EOF) {
            free(escaped);
            fclose(file);
            snprintf(result.message, sizeof(result.message), "failed to write CSV separator");
            return result;
        }

        if (fputs(escaped, file) == EOF) {
            free(escaped);
            fclose(file);
            snprintf(result.message, sizeof(result.message), "failed to write CSV field");
            return result;
        }

        free(escaped);
    }

    fclose(file);
    result.ok = 1;
    result.affected_rows = 1;
    snprintf(result.message, sizeof(result.message), "INSERT 1");
    return result;
}

/* 특정 바이트 오프셋에서 시작하는 CSV 행 하나만 읽어 오는 함수다. */
StorageReadResult read_row_at_offset_csv(const char *data_dir, const char *table_name, long row_offset) {
    StorageReadResult result = {0};
    FILE *file = open_table_file(data_dir, table_name, "rb", result.message, sizeof(result.message));
    char line[4096];

    if (file == NULL) {
        return result;
    }

    /* 인덱스가 알려 준 바이트 위치로 직접 점프해 한 행만 읽는다. */
    if (fseek(file, row_offset, SEEK_SET) != 0) {
        fclose(file);
        snprintf(result.message, sizeof(result.message), "failed to seek row offset");
        return result;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        snprintf(result.message, sizeof(result.message), "failed to read row at offset");
        return result;
    }

    fclose(file);
    strip_line_endings(line);
    if (line[0] == '\0') {
        snprintf(result.message, sizeof(result.message), "row at offset is empty");
        return result;
    }

    /* 오프셋에서 읽은 한 줄을 다시 CSV 필드 리스트로 파싱한다. */
    if (!csv_parse_line(line, &result.fields, result.message, sizeof(result.message))) {
        return result;
    }

    result.ok = 1;
    return result;
}

/*
 * CSV 전체를 한 줄씩 순회하며 visitor 콜백을 호출한다.
 * 인덱스 재구성처럼 "모든 행을 다시 읽어야 하는 작업"에 사용된다.
 */
int scan_rows_csv(const char *data_dir, const char *table_name, StorageRowVisitor visitor, void *context, char *error, size_t error_size) {
    FILE *file = open_table_file(data_dir, table_name, "rb", error, error_size);
    char line[4096];

    if (file == NULL) {
        return 0;
    }

    /* 첫 줄은 헤더이므로 읽고 버린 뒤 실제 데이터 행부터 순회한다. */
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        snprintf(error, error_size, "table data file is empty");
        return 0;
    }

    while (1) {
        /* 다음 줄을 읽기 직전의 파일 위치가 그 줄의 시작 오프셋이다. */
        long row_offset = ftell(file);
        StringList fields = {0};

        if (row_offset < 0) {
            fclose(file);
            snprintf(error, error_size, "failed to determine row offset");
            return 0;
        }

        if (fgets(line, sizeof(line), file) == NULL) {
            break;
        }

        strip_line_endings(line);
        /* 빈 줄은 무시하고 다음 행으로 넘어간다. */
        if (line[0] == '\0') {
            continue;
        }

        /* 현재 행을 필드 리스트로 바꾼 뒤 visitor 콜백에 넘긴다. */
        if (!csv_parse_line(line, &fields, error, error_size)) {
            fclose(file);
            return 0;
        }

        /* visitor가 실패하면 즉시 순회를 중단하고 에러를 전파한다. */
        if (!visitor(&fields, row_offset, context, error, error_size)) {
            string_list_free(&fields);
            fclose(file);
            return 0;
        }

        string_list_free(&fields);
    }

    fclose(file);
    return 1;
}
