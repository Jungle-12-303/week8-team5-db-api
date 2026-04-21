#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/*
 * common/util.h
 *
 * 여러 계층에서 공통으로 쓰는 작은 보조 함수들을 모아 둔 헤더다.
 * 문자열 복사, 경로 조합, 정수 변환, 에러 메시지 포맷팅처럼
 * 특정 계층에 속하지 않는 기능을 여기서 제공한다.
 */

/* 문자열 목록을 동적으로 저장하는 가장 단순한 가변 배열 구조체다. */
typedef struct {
    /* 각 문자열의 시작 주소를 담는 배열 */
    char **items;
    /* 현재 실제로 저장된 문자열 개수 */
    int count;
    /* items 배열이 현재 확보한 전체 슬롯 수 */
    int capacity;
} StringList;

/*
 * 파일 전체를 한 번에 읽어 새 버퍼에 담아 돌려준다.
 * 반환된 문자열은 호출자가 free()로 해제해야 한다.
 * 실패하면 NULL을 반환하고 error 버퍼에 원인을 기록한다.
 */
char *read_entire_file(const char *path, char *error, size_t error_size);

/* NUL 종료 문자열을 새 메모리에 복사해 돌려준다. 호출자가 free() 해야 한다. */
char *copy_string(const char *source);

/* 대소문자를 무시하고 두 문자열이 같은지 비교한다. */
int strings_equal_ignore_case(const char *left, const char *right);

/* 문자열 앞뒤 공백을 잘라 내고, 잘린 시작 위치를 가리키는 포인터를 돌려준다. */
char *trim_whitespace(char *text);

/* 문자열 끝에 붙은 \r, \n 같은 줄 끝 문자를 제거한다. */
void strip_line_endings(char *text);

/*
 * StringList 끝에 새 문자열을 복사해서 추가한다.
 * value 원본은 그대로 두고, 내부에 별도 복사본을 저장한다.
 */
int string_list_push(StringList *list, const char *value);

/* StringList 안에서 column/value 같은 문자열을 찾아 인덱스를 돌려준다. 없으면 -1이다. */
int string_list_index_of(const StringList *list, const char *value);

/* StringList 내부에 할당된 모든 문자열과 배열 메모리를 정리한다. */
void string_list_free(StringList *list);

/*
 * dir + name + extension을 이어 붙여 새 경로 문자열을 만든다.
 * 예: ("schema", "student", ".meta") -> "schema/student.meta"
 * 반환된 문자열은 호출자가 free() 해야 한다.
 */
char *build_path(const char *dir, const char *name, const char *extension);

/*
 * 문자열을 엄격하게 int로 변환한다.
 * 숫자가 아닌 문자가 섞여 있으면 실패한다.
 */
int parse_int_strict(const char *text, int *value);

/*
 * 시스템 에러(errno)를 포함한 사용자용 오류 메시지를 만든다.
 * action에는 "failed to open SQL file" 같은 설명,
 * path에는 문제를 일으킨 경로를 넣는다.
 */
void format_system_error(char *error, size_t error_size, const char *action, const char *path);

#endif
