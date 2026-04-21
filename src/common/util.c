// util.h에 선언된 공통 함수들의 실제 구현 파일이다.
/*
 * common/util.c
 *
 * 이 파일은 여러 계층에서 공통으로 사용하는 보조 함수를 모아 둔 곳이다.
 * 직접 SQL을 해석하거나 실행하지는 않지만,
 * 문자열 복사, 파일 읽기, 경로 만들기, 에러 메시지 정리 같은 반복 작업을 맡는다.
 */
#include "sqlparser/common/util.h"

// 공백 판별과 소문자 변환 함수를 쓰기 위해 포함한다.
#include <ctype.h>
#include <errno.h>
#include <limits.h>
// 파일 입출력과 snprintf를 쓰기 위해 포함한다.
#include <stdio.h>
// malloc, free, realloc을 쓰기 위해 포함한다.
#include <stdlib.h>
// strlen, memcpy 같은 문자열 함수를 쓰기 위해 포함한다.
#include <string.h>

/* 파일 전체를 읽어 하나의 C 문자열로 반환한다. */
char *read_entire_file(const char *path, char *error, size_t error_size) {
    // 읽을 파일 핸들이다.
    FILE *file;
    // 파일 전체 크기를 long 타입으로 받는다.
    long file_size;
    // 실제로 몇 바이트를 읽었는지 저장한다.
    size_t read_size;
    // 파일 내용을 담을 동적 버퍼다.
    char *buffer;

    // 바이너리 모드로 파일을 연다.
    file = fopen(path, "rb");
    // 파일을 못 열면 원인을 메시지로 남기고 NULL을 반환한다.
    if (file == NULL) {
        format_system_error(error, error_size, "failed to open SQL file", path);
        return NULL;
    }

    // 파일 끝으로 이동해 전체 크기를 구할 준비를 한다.
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        snprintf(error, error_size, "failed to seek SQL file: %s", path);
        return NULL;
    }

    // 현재 위치를 읽으면 파일 크기가 된다.
    file_size = ftell(file);
    // 음수가 나오면 파일 크기 측정에 실패한 것이다.
    if (file_size < 0) {
        fclose(file);
        snprintf(error, error_size, "failed to measure SQL file: %s", path);
        return NULL;
    }

    // 다시 파일 처음으로 돌아가 실제 내용을 읽을 준비를 한다.
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        snprintf(error, error_size, "failed to rewind SQL file: %s", path);
        return NULL;
    }

    // 파일 크기 + 문자열 끝 표시용 '\0' 만큼 메모리를 잡는다.
    buffer = (char *)malloc((size_t)file_size + 1);
    // 메모리 확보에 실패하면 파일을 닫고 NULL을 반환한다.
    if (buffer == NULL) {
        fclose(file);
        snprintf(error, error_size, "out of memory while reading SQL file");
        return NULL;
    }

    // 파일 전체를 한 번에 읽어 버퍼에 채운다.
    read_size = fread(buffer, 1, (size_t)file_size, file);
    // 읽기가 끝났으니 파일 핸들은 바로 닫는다.
    fclose(file);

    // 읽은 바이트 수가 예상한 파일 크기와 다르면 읽기 실패로 본다.
    if (read_size != (size_t)file_size) {
        free(buffer);
        snprintf(error, error_size, "failed to read SQL file: %s", path);
        return NULL;
    }

    // C 문자열로 다루기 위해 마지막에 '\0'을 붙인다.
    buffer[file_size] = '\0';
    // 완성된 문자열 버퍼를 호출한 쪽으로 돌려준다.
    return buffer;
}

/* 문자열을 새 메모리에 복사해 소유권을 분리한다. */
char *copy_string(const char *source) {
    // 원본 문자열 길이다.
    size_t length;
    // 복사본을 담을 새 포인터다.
    char *copy;

    // NULL을 복사하려고 하면 그대로 NULL을 돌려준다.
    if (source == NULL) {
        return NULL;
    }

    // 문자열 길이를 구한다. '\0'은 제외된 길이다.
    length = strlen(source);
    // 길이 + '\0' 만큼 메모리를 새로 잡는다.
    copy = (char *)malloc(length + 1);
    // 메모리 확보 실패 시 NULL을 반환한다.
    if (copy == NULL) {
        return NULL;
    }

    // 실제 문자열 내용을 '\0'까지 통째로 복사한다.
    memcpy(copy, source, length + 1);
    return copy;
}

/* 대소문자를 무시하고 두 문자열이 같은지 비교한다. */
int strings_equal_ignore_case(const char *left, const char *right) {
    // 현재 비교 중인 왼쪽 문자다.
    unsigned char left_char;
    // 현재 비교 중인 오른쪽 문자다.
    unsigned char right_char;

    // 둘 중 하나라도 NULL이면 같은 문자열로 보지 않는다.
    if (left == NULL || right == NULL) {
        return 0;
    }

    // 두 문자열 끝에 도달할 때까지 한 글자씩 비교한다.
    while (*left != '\0' && *right != '\0') {
        left_char = (unsigned char)*left;
        right_char = (unsigned char)*right;

        // 소문자로 바꿨을 때 다르면 다른 문자열이다.
        if (tolower(left_char) != tolower(right_char)) {
            return 0;
        }

        // 다음 문자로 이동한다.
        left++;
        right++;
    }

    // 둘 다 동시에 끝나야 완전히 같은 문자열이다.
    return *left == '\0' && *right == '\0';
}

/* 문자열 앞뒤 공백을 제거한 실제 내용의 시작 위치를 돌려준다. */
char *trim_whitespace(char *text) {
    // 문자열 끝을 가리킬 포인터다.
    char *end;

    // NULL 입력은 그대로 NULL을 반환한다.
    if (text == NULL) {
        return NULL;
    }

    // 앞쪽 공백을 건너뛰어 실제 내용이 시작하는 위치로 이동한다.
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    // 현재 내용 기준으로 문자열 끝 주소를 잡는다.
    end = text + strlen(text);
    // 뒤쪽 공백을 거꾸로 지워 나간다.
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }

    // 마지막 유효 문자 뒤를 '\0'로 바꿔 공백 제거를 완료한다.
    *end = '\0';
    // 공백이 제거된 문자열 시작 위치를 반환한다.
    return text;
}

/* 줄 끝의 \r, \n 문자를 제거해 파싱하기 쉬운 형태로 만든다. */
void strip_line_endings(char *text) {
    // 현재 문자열 길이다.
    size_t length;

    // NULL이면 처리할 것이 없으므로 종료한다.
    if (text == NULL) {
        return;
    }

    // 문자열 전체 길이를 가져온다.
    length = strlen(text);
    // 맨 뒤에 \n 또는 \r 이 있으면 하나씩 없앤다.
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[length - 1] = '\0';
        length--;
    }
}

/* StringList 동적 배열 끝에 문자열 하나를 복사해 추가한다. */
int string_list_push(StringList *list, const char *value) {
    // 배열이 커질 때 새로 잡을 크기다.
    int new_capacity;
    // realloc 후 새 배열 포인터를 잠시 담는다.
    char **new_items;
    // value의 복사본을 담을 포인터다.
    char *copy;

    // 현재 배열이 꽉 찼으면 더 큰 배열로 늘린다.
    if (list->count == list->capacity) {
        // 처음이면 4칸, 그 뒤로는 두 배씩 늘린다.
        new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        new_items = (char **)realloc(list->items, (size_t)new_capacity * sizeof(char *));
        // realloc 실패 시 그대로 0을 반환한다.
        if (new_items == NULL) {
            return 0;
        }

        // 새 배열을 실제 리스트에 반영한다.
        list->items = new_items;
        list->capacity = new_capacity;
    }

    // 원본 문자열을 독립적으로 보관하기 위해 복사본을 만든다.
    copy = copy_string(value);
    if (copy == NULL) {
        return 0;
    }

    // 새 문자열을 배열 끝에 넣고 개수를 하나 늘린다.
    list->items[list->count] = copy;
    list->count++;
    return 1;
}

/* StringList에서 특정 문자열의 위치를 찾고, 없으면 -1을 반환한다. */
int string_list_index_of(const StringList *list, const char *value) {
    // 리스트를 순회할 인덱스다.
    int index;

    // 앞에서부터 끝까지 같은 문자열이 있는지 찾는다.
    for (index = 0; index < list->count; index++) {
        if (strcmp(list->items[index], value) == 0) {
            return index;
        }
    }

    // 못 찾았으면 -1을 반환한다.
    return -1;
}

/* StringList가 소유한 모든 문자열과 배열 메모리를 해제한다. */
void string_list_free(StringList *list) {
    // 배열 안의 각 문자열을 순회하기 위한 인덱스다.
    int index;

    // 먼저 각 문자열 메모리를 하나씩 해제한다.
    for (index = 0; index < list->count; index++) {
        free(list->items[index]);
    }

    // 그다음 문자열 포인터 배열 자체를 해제한다.
    free(list->items);
    // 해제 후에는 안전하게 초기 상태로 되돌린다.
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* dir/name.extension 형태의 경로 문자열을 만든다. */
char *build_path(const char *dir, const char *name, const char *extension) {
    // 완성될 경로 문자열 길이다.
    size_t length;
    // 새로 만들 경로 문자열 버퍼다.
    char *path;

    // "dir/name.extension" 형태가 되도록 필요한 전체 길이를 계산한다.
    length = strlen(dir) + strlen(name) + strlen(extension) + 3;
    // 계산된 길이만큼 메모리를 잡는다.
    path = (char *)malloc(length);
    if (path == NULL) {
        return NULL;
    }

    // 실제 경로 문자열을 만들어 버퍼에 저장한다.
    snprintf(path, length, "%s/%s%s", dir, name, extension);
    return path;
}

/* 문자열이 정확한 정수인지 엄격하게 검사하고 int로 변환한다. */
int parse_int_strict(const char *text, int *value) {
    char *end = NULL;
    long parsed;

    if (text == NULL || value == NULL || *text == '\0') {
        return 0;
    }

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }

    if (parsed < INT_MIN || parsed > INT_MAX) {
        return 0;
    }

    *value = (int)parsed;
    return 1;
}

/* errno를 포함한 시스템 오류를 사람이 읽기 쉬운 문장으로 정리한다. */
void format_system_error(char *error, size_t error_size, const char *action, const char *path) {
    const char *reason = strerror(errno);

    if (reason == NULL) {
        reason = "unknown system error";
    }

    if (path == NULL || *path == '\0') {
        snprintf(error, error_size, "%s: %s", action, reason);
        return;
    }

    snprintf(error, error_size, "%s '%s': %s", action, path, reason);
}
