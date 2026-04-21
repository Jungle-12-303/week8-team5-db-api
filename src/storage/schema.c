// schema.c는 meta 파일과 CSV 헤더를 읽어 테이블 구조를 검증한다.
/*
 * storage/schema.c
 *
 * 이 파일은 schema/<table>.meta 와 data/<table>.csv를 읽어
 * "이 테이블이 정말 유효한가?"를 검증하는 역할을 담당한다.
 *
 * 쉽게 말해:
 * - 테이블 정의 읽기
 * - 컬럼 목록 확인
 * - CSV 헤더와 스키마가 맞는지 검증
 */
#include "sqlparser/storage/schema.h"

// CSV 한 줄을 컬럼 목록으로 파싱하기 위해 사용한다.
#include "sqlparser/storage/storage.h"

// 스키마 디렉터리 안의 meta 파일을 탐색하기 위해 포함한다.
#include <dirent.h>
#include <errno.h>
// 파일 읽기와 메시지 생성을 위해 포함한다.
#include <stdio.h>
// free 함수를 쓰기 위해 포함한다.
#include <stdlib.h>
// strcmp, strchr를 쓰기 위해 포함한다.
#include <string.h>

/* SchemaResult 실패 메시지를 공통 형식으로 채우는 헬퍼 함수다. */
static void set_schema_error(SchemaResult *result, const char *message) {
    // 실패 플래그를 0으로 둔다.
    result->ok = 0;
    // 사람이 읽을 수 있는 오류 메시지를 저장한다.
    snprintf(result->message, sizeof(result->message), "%s", message);
}

/* meta 파일의 columns=... 값을 CSV 한 줄처럼 파싱해 컬럼 리스트로 만든다. */
static int parse_columns_value(const char *value, StringList *columns, char *message, size_t message_size) {
    // columns=id,name,age 값을 잠시 CSV처럼 파싱할 임시 리스트다.
    StringList parsed = {0};
    // csv_parse_line 성공 여부다.
    int ok;

    // 컬럼 목록도 쉼표로 구분돼 있으므로 CSV 파서를 재사용한다.
    ok = csv_parse_line(value, &parsed, message, message_size);
    if (!ok) {
        return 0;
    }

    *columns = parsed;
    return 1;
}

/* meta 파일 경로에서 실제 저장 파일의 basename을 추출한다. */
static char *extract_storage_name(const char *path) {
    // 마지막 디렉터리 구분자 위치다.
    const char *filename = strrchr(path, '/');
    // 파일명 끝 길이다.
    size_t length;
    // 확장자를 제거한 저장용 이름이다.
    char *name;

    if (filename == NULL) {
        filename = path;
    } else {
        filename++;
    }

    length = strlen(filename);
    if (length >= 5 && strcmp(filename + length - 5, ".meta") == 0) {
        length -= 5;
    }

    name = (char *)malloc(length + 1);
    if (name == NULL) {
        return NULL;
    }

    memcpy(name, filename, length);
    name[length] = '\0';
    return name;
}

/* 디렉터리 안의 자식 파일 경로를 "dir/filename" 형태로 만든다. */
static char *build_child_file_path(const char *dir, const char *filename) {
    // "dir/filename" 전체 길이를 계산한다.
    size_t length = strlen(dir) + strlen(filename) + 2;
    // 완성될 경로 문자열 버퍼다.
    char *path = (char *)malloc(length);

    if (path == NULL) {
        return NULL;
    }

    snprintf(path, length, "%s/%s", dir, filename);
    return path;
}

/* 파일명이 .meta 확장자로 끝나는지 검사한다. */
static int has_meta_extension(const char *filename) {
    // 파일명 길이를 구한다.
    size_t length = strlen(filename);

    // ".meta" 보다 짧으면 meta 파일이 아니다.
    if (length < 5) {
        return 0;
    }

    return strcmp(filename + length - 5, ".meta") == 0;
}

/*
 * 특정 meta 파일이 요청한 table=... 선언을 갖고 있는지 검사한다.
 * alias 파일명 지원을 위해 디렉터리 전체를 탐색할 때 사용된다.
 */
static int file_declares_table(const char *path, const char *table_name, int *matches, char *message, size_t message_size) {
    // meta 파일을 읽을 핸들이다.
    FILE *file;
    // 한 줄씩 읽을 버퍼다.
    char line[4096];
    // 앞뒤 공백을 제거한 문자열이다.
    char *trimmed;
    // key=value 구분 위치다.
    char *separator;
    // 일치 여부를 담는 플래그다.
    *matches = 0;

    file = fopen(path, "rb");
    if (file == NULL) {
        format_system_error(message, message_size, "failed to open schema meta file", path);
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        strip_line_endings(line);
        trimmed = trim_whitespace(line);
        if (*trimmed == '\0') {
            continue;
        }

        separator = strchr(trimmed, '=');
        if (separator == NULL) {
            continue;
        }

        *separator = '\0';
        separator++;
        separator = trim_whitespace(separator);

        if (strcmp(trimmed, "table") == 0 && strcmp(separator, table_name) == 0) {
            *matches = 1;
            break;
        }
    }

    fclose(file);
    return 1;
}

/*
 * 요청한 테이블 이름에 해당하는 schema meta 파일 경로를 찾는다.
 *
 * 1순위:
 * - schema_dir/table_name.meta
 *
 * 없으면:
 * - schema_dir 안의 모든 .meta를 열어 table=... 선언을 확인
 */
static char *find_schema_path(const char *schema_dir, const char *table_name, char *message, size_t message_size) {
    // 요청한 테이블명 기준 기본 meta 경로다.
    char *path = build_path(schema_dir, table_name, ".meta");
    // 기본 경로를 여는 파일 핸들이다.
    FILE *file;
    // 디렉터리 순회 핸들이다.
    DIR *directory;
    // 각 엔트리를 담는 포인터다.
    struct dirent *entry;

    if (path == NULL) {
        snprintf(message, message_size, "out of memory while building schema path");
        return NULL;
    }

    file = fopen(path, "rb");
    if (file != NULL) {
        fclose(file);
        return path;
    }
    if (errno != ENOENT && errno != ENOTDIR && errno != EILSEQ) {
        format_system_error(message, message_size, "failed to open schema meta file", path);
        free(path);
        return NULL;
    }
    free(path);

    directory = opendir(schema_dir);
    if (directory == NULL) {
        format_system_error(message, message_size, "failed to open schema directory", schema_dir);
        return NULL;
    }

    while ((entry = readdir(directory)) != NULL) {
        char *candidate_path;
        int matches = 0;

        if (!has_meta_extension(entry->d_name)) {
            continue;
        }

        candidate_path = build_child_file_path(schema_dir, entry->d_name);
        if (candidate_path == NULL) {
            closedir(directory);
            snprintf(message, message_size, "out of memory while building schema path");
            return NULL;
        }

        if (!file_declares_table(candidate_path, table_name, &matches, message, message_size)) {
            free(candidate_path);
            closedir(directory);
            return NULL;
        }

        if (matches) {
            closedir(directory);
            return candidate_path;
        }

        free(candidate_path);
    }

    closedir(directory);
    snprintf(message, message_size, "schema meta file does not exist");
    return NULL;
}

/* CSV 헤더가 schema의 컬럼 수와 순서를 정확히 따르는지 검사한다. */
static int validate_csv_header(const char *data_dir, const char *storage_name, const Schema *schema, char *message, size_t message_size) {
    // data/<storage>.csv 경로다.
    char *path;
    // CSV 파일 핸들이다.
    FILE *file;
    // 첫 줄 헤더를 읽어 올 버퍼다.
    char line[4096];
    // 파싱된 헤더 컬럼 목록이다.
    StringList header = {0};
    // 컬럼 순회를 위한 인덱스다.
    int index;

    // data 디렉터리 기준 CSV 경로를 만든다.
    path = build_path(data_dir, storage_name, ".csv");
    if (path == NULL) {
        snprintf(message, message_size, "out of memory while building CSV path");
        return 0;
    }

    // CSV 파일을 연다.
    file = fopen(path, "rb");
    // 경로 문자열은 더 이상 필요 없으므로 해제한다.
    if (file == NULL) {
        format_system_error(message, message_size, "failed to open table data file", path);
        free(path);
        return 0;
    }
    free(path);

    // 첫 줄이 없으면 헤더가 없는 잘못된 파일이다.
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        snprintf(message, message_size, "table data file is empty");
        return 0;
    }

    // 헤더 한 줄만 읽으면 되므로 파일을 닫는다.
    fclose(file);
    // 줄 끝의 \r, \n 을 지워 깔끔한 헤더 문자열로 만든다.
    strip_line_endings(line);

    // 헤더 문자열을 컬럼 리스트로 파싱한다.
    if (!csv_parse_line(line, &header, message, message_size)) {
        return 0;
    }

    // 헤더 컬럼 수와 schema 컬럼 수가 다르면 실패다.
    if (header.count != schema->columns.count) {
        string_list_free(&header);
        snprintf(message, message_size, "CSV header does not match schema column count");
        return 0;
    }

    // 컬럼 이름과 순서가 모두 같은지 하나씩 비교한다.
    for (index = 0; index < header.count; index++) {
        if (strcmp(header.items[index], schema->columns.items[index]) != 0) {
            string_list_free(&header);
            snprintf(message, message_size, "CSV header does not match schema column order");
            return 0;
        }
    }

    // 임시 헤더 리스트 메모리를 정리한다.
    string_list_free(&header);
    return 1;
}

/*
 * 테이블 하나의 schema를 로딩하고, 연결된 CSV까지 함께 검증하는 storage 진입점이다.
 */
SchemaResult load_schema(const char *schema_dir, const char *data_dir, const char *table_name) {
    // 반환할 최종 결과 구조체다.
    SchemaResult result = {0};
    // 실제로 읽을 schema meta 경로 문자열이다.
    char *path;
    // meta 파일 핸들이다.
    FILE *file;
    // meta 파일을 한 줄씩 읽을 버퍼다.
    char line[4096];
    // 공백 제거 후 실제 내용을 가리키는 포인터다.
    char *trimmed;
    // key=value 에서 '=' 위치를 찾는 데 쓸 포인터다.
    char *separator;

    // meta 파일 경로를 만든다.
    path = find_schema_path(schema_dir, table_name, result.message, sizeof(result.message));
    if (path == NULL) {
        result.ok = 0;
        return result;
    }

    // meta 파일명을 기준으로 실제 data 파일 basename도 함께 기억한다.
    result.schema.storage_name = extract_storage_name(path);
    if (result.schema.storage_name == NULL) {
        free(path);
        set_schema_error(&result, "out of memory while reading schema storage name");
        return result;
    }

    // meta 파일을 연다.
    file = fopen(path, "rb");
    // 경로 문자열은 이제 필요 없으므로 해제한다.
    if (file == NULL) {
        format_system_error(result.message, sizeof(result.message), "failed to open schema meta file", path);
        free(path);
        result.ok = 0;
        return result;
    }
    free(path);

    // meta 파일을 끝까지 한 줄씩 읽는다.
    while (fgets(line, sizeof(line), file) != NULL) {
        // 줄바꿈을 지우고 양쪽 공백도 제거한다.
        strip_line_endings(line);
        trimmed = trim_whitespace(line);
        // 빈 줄은 무시한다.
        if (*trimmed == '\0') {
            continue;
        }

        // key=value 형식인지 확인한다.
        separator = strchr(trimmed, '=');
        if (separator == NULL) {
            fclose(file);
            free_schema(&result.schema);
            set_schema_error(&result, "invalid schema meta format");
            return result;
        }

        // '='를 기준으로 key와 value를 분리한다.
        *separator = '\0';
        separator++;
        separator = trim_whitespace(separator);

        // table 키면 테이블 이름을 저장한다.
        if (strcmp(trimmed, "table") == 0) {
            free(result.schema.table_name);
            result.schema.table_name = copy_string(separator);
            if (result.schema.table_name == NULL) {
                fclose(file);
                free_schema(&result.schema);
                set_schema_error(&result, "out of memory while reading schema table name");
                return result;
            }
        // columns 키면 컬럼 목록을 파싱한다.
        } else if (strcmp(trimmed, "columns") == 0) {
            string_list_free(&result.schema.columns);
            if (!parse_columns_value(separator, &result.schema.columns, result.message, sizeof(result.message))) {
                fclose(file);
                free_schema(&result.schema);
                result.ok = 0;
                return result;
            }
        }
    }

    // 파일 읽기가 끝났으므로 닫는다.
    fclose(file);

    // 테이블명이나 컬럼 목록이 빠져 있으면 잘못된 meta 파일이다.
    if (result.schema.table_name == NULL || result.schema.storage_name == NULL || result.schema.columns.count == 0) {
        free_schema(&result.schema);
        set_schema_error(&result, "schema meta file is missing required fields");
        return result;
    }

    /*
     * `id`는 사용자 컬럼이 아니라 숨은 내부 PK를 가리키는 예약 이름이다.
     * 그래서 스키마 로딩 단계에서 먼저 막아 두어야 이후 parser/executor 쪽 해석이
     * 흔들리지 않는다.
     */
    if (schema_has_reserved_id_column(&result.schema)) {
        free_schema(&result.schema);
        set_schema_error(&result, "reserved column name 'id' is not allowed in user schema");
        return result;
    }

    // SQL에서는 선언된 table 이름과 실제 파일 basename 둘 다 허용한다.
    if (strcmp(result.schema.table_name, table_name) != 0 &&
        strcmp(result.schema.storage_name, table_name) != 0) {
        free_schema(&result.schema);
        set_schema_error(&result, "schema table name does not match requested table");
        return result;
    }

    // 같은 이름의 CSV 파일 헤더도 스키마와 맞는지 검증한다.
    if (!validate_csv_header(data_dir, result.schema.storage_name, &result.schema, result.message, sizeof(result.message))) {
        free_schema(&result.schema);
        result.ok = 0;
        return result;
    }

    // 여기까지 통과하면 유효한 스키마다.
    result.ok = 1;
    return result;
}

/* 특정 컬럼명이 schema에서 몇 번째에 있는지 찾는다. */
int schema_find_column(const Schema *schema, const char *column_name) {
    if (schema == NULL || column_name == NULL) {
        return -1;
    }

    return string_list_index_of(&schema->columns, column_name);
}

/* 사용자 스키마에 예약 이름 `id`가 들어 있는지 여부만 간단히 확인한다. */
int schema_has_reserved_id_column(const Schema *schema) {
    return schema_find_column(schema, "id") >= 0;
}

/* Schema 구조체가 소유한 문자열과 컬럼 리스트 메모리를 정리한다. */
void free_schema(Schema *schema) {
    // 테이블 이름 문자열을 해제한다.
    free(schema->table_name);
    schema->table_name = NULL;
    // 실제 파일명 기준 이름도 해제한다.
    free(schema->storage_name);
    schema->storage_name = NULL;
    // 컬럼 목록 문자열들도 해제한다.
    string_list_free(&schema->columns);
}
