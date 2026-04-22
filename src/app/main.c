// AST 구조체를 해제하기 위한 선언입니다.
/*
 * app/main.c
 *
 * 이 파일은 프로그램의 시작점이다.
 * 사용자가 CLI 인자를 줬는지, 파일을 실행하려는지, SQL 문자열을 직접 넣었는지,
 * 아니면 REPL로 대화형 입력을 원하는지를 판단해 전체 흐름을 시작한다.
 *
 * 아키텍처 관점에서 app 계층은 "입력 방식 결정"만 담당하고,
 * 실제 SQL 해석과 실행은 각각 lexer/parser/executor에 맡긴다.
 */
#include "sqlparser/sql/ast.h"
// 파싱된 SQL을 실제로 실행하기 위한 선언입니다.
#include "sqlparser/execution/executor.h"
// SQL 문자열을 토큰으로 나누기 위한 선언입니다.
#include "sqlparser/sql/lexer.h"
// 토큰 목록을 INSERT / SELECT 구조로 해석하기 위한 선언입니다.
#include "sqlparser/sql/parser.h"
// 파일 읽기와 문자열 복사 같은 공통 유틸 함수를 쓰기 위한 선언입니다.
#include "sqlparser/common/util.h"

#include <ctype.h>
#include <errno.h>
// 표준 입출력과 fopen을 사용합니다.
#include <stdio.h>
// free, malloc 같은 메모리 함수들을 사용합니다.
#include <stdlib.h>
// strlen, memcpy 같은 문자열 함수를 사용합니다.
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _MSC_VER
#include <io.h>
#define ISATTY _isatty
#define STAT_STRUCT struct _stat
#define STAT_FUNC _stat
#define IS_REGULAR_FILE(mode) (((mode) & _S_IFMT) == _S_IFREG)
#define IS_DIRECTORY_FILE(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#else
#include <unistd.h>
#define ISATTY isatty
#define STAT_STRUCT struct stat
#define STAT_FUNC stat
#define IS_REGULAR_FILE(mode) S_ISREG(mode)
#define IS_DIRECTORY_FILE(mode) S_ISDIR(mode)
#endif

/*
 * bare argument가 "파일 경로인지" 아니면 "그냥 SQL 문자열인지" 판단한다.
 *
 * 예:
 * - users.sql  -> 파일로 읽기
 * - SELECT ... -> SQL 문자열로 간주
 * - 디렉터리 경로 -> 오류
 */
static int resolve_bare_argument_file(const char *path, int *should_read_file, char *error, size_t error_size) {
    STAT_STRUCT info;

    /*
     * 기본값은 "파일로 읽지 않음"이다.
     * 즉, 이 함수가 특별히 파일이라고 판정하지 않으면
     * 호출자는 이 인자를 SQL 문자열로 해석하게 된다.
     */
    *should_read_file = 0;

    if (STAT_FUNC(path, &info) == 0) {
        /* 존재하는 일반 파일이면 파일 입력 모드로 확정한다. */
        if (IS_REGULAR_FILE(info.st_mode)) {
            *should_read_file = 1;
            return 1;
        }

        /* 디렉터리를 SQL 파일처럼 넘긴 경우는 명확한 사용자 입력 오류다. */
        if (IS_DIRECTORY_FILE(info.st_mode)) {
            snprintf(error, error_size, "path is a directory, not a SQL file: %s", path);
            return 0;
        }

        /*
         * 디렉터리도 아니고 일반 파일도 아니면
         * 특수 파일/장치 파일 같은 비정상 입력으로 보고 실패시킨다.
         */
        if (!IS_REGULAR_FILE(info.st_mode)) {
            snprintf(error, error_size, "path is not a regular file: %s", path);
            return 0;
        }

        /*
         * 여기까지 왔다는 것은 경로는 존재하지만
         * 정상적인 SQL 파일로 접근할 수 없는 상황으로 간주한다.
         * 예: 권한 문제 같은 시스템 수준 오류
         */
        format_system_error(error, error_size, "failed to access SQL file", path);
        return 0;
    }

    /*
     * stat() 자체가 실패했더라도 아래 경우는 "파일이 아닌 일반 문자열"일 수 있다.
     *
     * - ENOENT: 파일이 없음
     * - ENOTDIR: 경로 중간이 디렉터리가 아님
     * - EILSEQ: 파일 경로로 보기 어려운 문자열
     *
     * 이런 경우는 에러로 처리하지 않고,
     * 호출자가 이 값을 SQL 문자열로 해석하도록 그대로 통과시킨다.
     */
    if (errno == ENOENT || errno == ENOTDIR || errno == EILSEQ) {
        return 1;
    }

    /*
     * 그 외 stat() 실패는 단순 "없는 파일"이 아니라
     * 실제 경로 접근 오류일 가능성이 크므로 에러로 반환한다.
     * 이 경우 SQL 문자열로 오해하면 잘못된 동작이 되기 때문이다.
     */
    format_system_error(error, error_size, "failed to access SQL path", path);
    return 0;
}

// 표준입력이 터미널에 연결된 대화형 환경인지 확인합니다.
/* 표준입력이 터미널에 연결됐는지 확인해 REPL 여부를 판단한다. */
static int stdin_is_interactive(void) {
#ifdef _MSC_VER
    /* Windows에서는 _fileno(stdin)으로 표준입력의 파일 번호를 구한 뒤 콘솔 연결 여부를 확인한다. */
    return ISATTY(_fileno(stdin)) != 0;
#else
    /* POSIX 환경에서는 STDIN_FILENO를 그대로 사용해 터미널 연결 여부를 확인한다. */
    return ISATTY(STDIN_FILENO) != 0;
#endif
}

// 공백만 있는 문자열인지 검사합니다.
/* 공백만 있는 입력인지 검사해 빈 SQL을 빠르게 걸러낸다. */
static int is_blank_string(const char *text) {
    /*
     * 문자열 끝까지 훑으면서 공백이 아닌 문자가 하나라도 나오면
     * "비어 있지 않은 입력"으로 판단한다.
     */
    while (*text != '\0') {
        if (!isspace((unsigned char)*text)) {
            return 0;
        }
        text++;
    }

    return 1;
}

// 여러 개로 나뉘어 들어온 명령줄 인자를 하나의 SQL 문자열로 합칩니다.
/* 여러 CLI 인자를 하나의 SQL 문자열로 합친다. */
static char *join_arguments_as_sql(int argc, char *argv[], int start_index, char *error, size_t error_size) {
    // 최종 SQL 문자열에 필요한 전체 길이입니다.
    size_t total_length = 1;
    // 반복문에 사용할 인덱스입니다.
    int index;
    // 최종 SQL 문자열 버퍼입니다.
    char *sql_text;
    // 버퍼의 현재 작성 위치입니다.
    size_t offset = 0;
    // 현재 인자 조각 길이입니다.
    size_t piece_length;

    // 합칠 SQL 인자가 비어 있으면 오류입니다.
    if (start_index >= argc) {
        snprintf(error, error_size, "missing SQL statement");
        return NULL;
    }

    // 모든 인자 길이와 중간 공백 하나를 포함해 필요한 크기를 계산합니다.
    for (index = start_index; index < argc; index++) {
        total_length += strlen(argv[index]);
        if (index + 1 < argc) {
            total_length += 1;
        }
    }

    // 계산된 길이만큼 버퍼를 할당합니다.
    sql_text = (char *)malloc(total_length);
    if (sql_text == NULL) {
        snprintf(error, error_size, "out of memory while building SQL statement");
        return NULL;
    }

    // 빈 문자열로 시작합니다.
    sql_text[0] = '\0';

    // 각 인자를 공백 하나로 이어 붙여 하나의 SQL 문장으로 만듭니다.
    for (index = start_index; index < argc; index++) {
        piece_length = strlen(argv[index]);
        memcpy(sql_text + offset, argv[index], piece_length);
        offset += piece_length;

        if (index + 1 < argc) {
            sql_text[offset] = ' ';
            offset++;
        }
    }

    // C 문자열 종료 표시를 붙입니다.
    sql_text[offset] = '\0';
    return sql_text;
}

// 파일이나 파이프처럼 길이를 알 수 없는 입력 스트림 전체를 메모리로 읽습니다.
/* stdin 같은 스트림 전체를 읽어 하나의 문자열로 만든다. */
static char *read_stream(FILE *stream, char *error, size_t error_size) {
    /* 한 번 fread() 할 때 임시로 데이터를 받아 둘 고정 크기 조각 버퍼다. */
    char chunk[1024];
    /* 최종적으로 "스트림 전체 내용"이 모이게 될 동적 버퍼다. */
    char *buffer = NULL;
    /* 현재까지 buffer에 누적된 실제 데이터 길이다. */
    size_t length = 0;
    /* 현재 buffer가 확보하고 있는 총 메모리 크기다. */
    size_t capacity = 0;

    /*
     * 파일/표준입력 끝(EOF)에 도달할 때까지 조금씩 읽는다.
     * 길이를 미리 알 수 없기 때문에 "읽고, 부족하면 늘리고"를 반복한다.
     */
    while (!feof(stream)) {
        /* 이번 차수에 최대 chunk 크기만큼 읽는다. */
        size_t bytes_read = fread(chunk, 1, sizeof(chunk), stream);

        if (bytes_read > 0) {
            /* 이번에 읽은 데이터와 마지막 문자열 종료 문자('\0')까지 담기 위한 최소 필요 크기다. */
            size_t required = length + bytes_read + 1;
            char *new_buffer;
            /* 처음에는 1KB부터 시작하고, 이후에는 기존 capacity를 두 배씩 키운다. */
            size_t new_capacity = capacity == 0 ? 1024 : capacity;

            /* 필요한 크기를 만족할 때까지 버퍼 크기를 점진적으로 증가시킨다. */
            while (new_capacity < required) {
                new_capacity *= 2;
            }

            /*
             * realloc()은 기존 내용을 유지한 채 버퍼 크기를 늘릴 수 있다.
             * 처음 buffer가 NULL이면 malloc처럼 동작한다.
             */
            new_buffer = (char *)realloc(buffer, new_capacity);
            if (new_buffer == NULL) {
                free(buffer);
                snprintf(error, error_size, "out of memory while reading standard input");
                return NULL;
            }

            buffer = new_buffer;
            capacity = new_capacity;
            /* 방금 읽은 조각을 현재 누적 위치 뒤에 이어 붙인다. */
            memcpy(buffer + length, chunk, bytes_read);
            length += bytes_read;
            /* C 문자열로 사용할 수 있도록 마지막에 항상 종료 문자를 유지한다. */
            buffer[length] = '\0';
        }

        /* fread() 도중 읽기 오류 플래그가 켜졌다면 즉시 실패 처리한다. */
        if (ferror(stream)) {
            free(buffer);
            snprintf(error, error_size, "failed to read standard input");
            return NULL;
        }
    }

    /*
     * 아무 것도 읽지 못한 경우에도 NULL 대신 빈 문자열을 돌려준다.
     * 호출자는 이 값을 일반 문자열처럼 동일하게 처리할 수 있다.
     */
    if (buffer == NULL) {
        buffer = copy_string("");
        if (buffer == NULL) {
            snprintf(error, error_size, "out of memory while reading standard input");
            return NULL;
        }
    }

    return buffer;
}

// 파일 경로인지 SQL 문자열인지 판단해 실제 SQL 본문을 읽어 옵니다.
/* 인자를 파일로 읽을지, SQL 문자열로 그대로 쓸지 결정해 SQL 본문을 준비한다. */
static char *load_sql_from_argument(const char *value, int force_file, char *error, size_t error_size) {
    int should_read_file = 0;

    /* -f 옵션처럼 호출자가 이미 "이건 파일이다"라고 확정한 경우는 곧바로 파일 전체를 읽는다. */
    if (force_file) {
        return read_entire_file(value, error, error_size);
    }

    /*
     * bare argument인 경우:
     * - 실제 파일이면 파일 내용을 읽고
     * - 아니면 SQL 문자열 자체를 복사해 사용한다.
     */
    if (!resolve_bare_argument_file(value, &should_read_file, error, error_size)) {
        return NULL;
    }

    if (should_read_file) {
        return read_entire_file(value, error, error_size);
    }

    return copy_string(value);
}

// 사용자에게 보여줄 CLI 사용법을 출력합니다.
/* 사용자가 --help 또는 잘못된 인자를 넣었을 때 보여 줄 도움말을 출력한다. */
static void print_usage(FILE *stream, const char *program_name) {
    /* 콘솔에서 눈에 잘 들어오도록 상단 제목과 구분선을 먼저 출력한다. */
    fprintf(stream, "============================================================\n");
    fprintf(stream, " SQL Processor 도움말\n");
    fprintf(stream, "============================================================\n");
    fprintf(stream, "\n");

    /* 실행 형식 요약이다. bare argument도 허용한다는 점을 함께 보여 준다. */
    fprintf(stream, "[사용법]\n");
    fprintf(stream, "  %s [옵션]... [SQL_또는_파일]\n", program_name);
    fprintf(stream, "  %s\n", program_name);
    fprintf(stream, "\n");

    /* 지원하는 CLI 옵션 목록이다. */
    fprintf(stream, "[옵션]\n");
    fprintf(stream, "  -e, --execute SQL   SQL 문장을 직접 실행합니다\n");
    fprintf(stream, "  -f, --file PATH     PATH에서 SQL 파일을 읽어 실행합니다\n");
    fprintf(stream, "  -h, --help          이 도움말을 출력합니다\n");
    fprintf(stream, "\n");

    /* 인자 없이 실행했을 때 들어가는 REPL 사용법도 같이 보여 준다. */
    fprintf(stream, "[대화형 모드]\n");
    fprintf(stream, "  터미널에서 인자 없이 실행하면 대화형 모드가 시작됩니다.\n");
    fprintf(stream, "  대화형 모드에서는 SQL 문장 또는 SQL 파일 경로를 입력할 수 있습니다.\n");
    fprintf(stream, "  프롬프트를 종료하려면 .exit, .quit, exit, quit 중 하나를 입력하세요.\n");
    fprintf(stream, "\n");

    /* 사용자가 바로 따라 할 수 있는 대표 예시를 마지막에 모아 보여 준다. */
    fprintf(stream, "[예시]\n");
    fprintf(stream, "  %s -e \"SELECT * FROM student;\"\n", program_name);
    fprintf(stream, "  %s -f examples/select_name_age.sql\n", program_name);
    fprintf(stream, "  echo \"SELECT name FROM student;\" | %s\n", program_name);
    fprintf(stream, "\n");
    fprintf(stream, "============================================================\n");
}

// SQL 문자열 하나를 lexer -> parser -> executor 순서로 처리합니다.
/*
 * SQL 문자열 하나를 끝까지 실행한다.
 *
 * 흐름:
 * 1. 빈 입력 검사
 * 2. lexer
 * 3. parser
 * 4. executor
 * 5. 결과 메시지 출력
 */
static int execute_sql_text(const char *sql_text, FILE *out, char *error, size_t error_size) {
    TokenArray tokens = {0};
    ParseResult parse_result;
    ExecResult exec_result;
    clock_t started;
    double elapsed_seconds;

    /* 공백만 있는 입력은 lexer/parser까지 보내지 않고 초기에 걸러낸다. */
    if (is_blank_string(sql_text)) {
        snprintf(error, error_size, "missing SQL statement");
        return 0;
    }

    /* 사용자 체감 실행 시간을 보여 주기 위해 전체 처리 시작 시각을 기록한다. */
    started = clock();

    /* 1단계: SQL 문자열을 토큰 배열로 분해한다. */
    if (!lex_sql(sql_text, &tokens, error, error_size)) {
        return 0;
    }

    /* 2단계: 토큰 배열을 INSERT/SELECT AST로 해석한다. */
    parse_result = parse_statement(&tokens);
    if (!parse_result.ok) {
        snprintf(error, error_size, "%s", parse_result.message);
        free_tokens(&tokens);
        return 0;
    }

    /* 3단계: 해석된 AST를 실제 schema/data 경로 기준으로 실행한다. */
    exec_result = execute_statement(&parse_result.statement, "schema", "data", out);
    if (!exec_result.ok) {
        snprintf(error, error_size, "%s", exec_result.message);
        free_statement(&parse_result.statement);
        free_tokens(&tokens);
        return 0;
    }

    /* executor가 남긴 요약 메시지(예: SELECT 1, INSERT 1)를 사용자에게 보여 준다. */
    if (exec_result.message[0] != '\0') {
        fprintf(out, "%s\n", exec_result.message);
    }

    /* 입력부터 결과 출력 직전까지 걸린 전체 시간을 초 단위로 계산해 덧붙인다. */
    elapsed_seconds = (double)(clock() - started) / (double)CLOCKS_PER_SEC;
    fprintf(out, "Elapsed time: %.6f sec\n", elapsed_seconds);

    free_statement(&parse_result.statement);
    free_tokens(&tokens);
    return 1;
}

// 한 줄 입력이 파일 경로인지 SQL인지 판별해 실행합니다.
/* REPL에서 받은 입력을 파일 경로 또는 SQL 문자열로 해석해 실행한다. */
static int execute_argument_or_sql(const char *value, int force_file, FILE *out, FILE *err) {
    char error[256];
    /* 파일 또는 직접 입력 SQL을 실제 SQL 본문 문자열로 변환한다. */
    char *sql_text = load_sql_from_argument(value, force_file, error, sizeof(error));
    int ok;

    /* 파일 판별/읽기 단계에서 실패하면 즉시 사용자에게 오류를 출력한다. */
    if (sql_text == NULL) {
        fprintf(err, "error: %s\n", error);
        return 0;
    }

    /* SQL 실행 실패도 동일한 에러 출력 형식으로 통일한다. */
    ok = execute_sql_text(sql_text, out, error, sizeof(error));
    if (!ok) {
        fprintf(err, "error: %s\n", error);
    }

    free(sql_text);
    return ok;
}

// 대화형 프롬프트를 제공해 SQL 또는 파일 경로를 반복 실행합니다.
/*
 * 대화형 프롬프트를 실행한다.
 *
 * 사용자는 한 줄씩 SQL 또는 파일 경로를 입력할 수 있고,
 * .exit / .quit / help 같은 간단한 명령도 사용할 수 있다.
 */
/*
 * 접속 모드 설명:
 * - 이 함수는 REPL(대화형 모드)을 담당한다.
 * - 조건: 인자 없이 실행했고, 표준입력이 터미널에 직접 연결된 경우
 * - 특징: `sqlparser>` 프롬프트를 반복 출력하며 여러 번 질의할 수 있다.
 * - 지원 입력: SQL 문장, SQL 파일 경로, `.help`, `.exit`, `.quit`
 */
static int run_repl(FILE *out, FILE *err, const char *program_name) {
    /* 한 줄씩 입력받기 위한 REPL 전용 고정 버퍼다. */
    char line[4096];
    /* 세션 중 한 번이라도 오류가 나면 종료 코드에 반영하기 위해 기록한다. */
    int had_error = 0;

    while (1) {
        char *input;

        /* 프롬프트를 출력하고 즉시 flush해서 사용자가 바로 입력할 수 있게 한다. */
        fprintf(out, "sqlparser> ");
        fflush(out);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* 입력 실패 자체는 에러로 처리한다. */
            if (ferror(stdin)) {
                fprintf(err, "error: failed to read interactive input\n");
                return 1;
            }
            /* Ctrl+Z / Ctrl+D 같은 EOF 종료는 자연스러운 종료로 본다. */
            fprintf(out, "\n");
            break;
        }

        /* 줄 끝 개행을 제거하고 좌우 공백을 정리해 비교/실행이 쉬운 형태로 만든다. */
        strip_line_endings(line);
        input = trim_whitespace(line);

        /* 빈 줄은 아무 일도 하지 않고 다음 프롬프트로 넘어간다. */
        if (*input == '\0') {
            continue;
        }

        /* 종료 명령은 루프를 빠져나가 REPL을 끝낸다. */
        if (strcmp(input, ".exit") == 0 || strcmp(input, ".quit") == 0 ||
            strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0) {
            break;
        }

        /* help 명령은 REPL 안에서도 전체 도움말을 다시 볼 수 있게 한다. */
        if (strcmp(input, ".help") == 0 || strcmp(input, "help") == 0) {
            print_usage(out, program_name);
            continue;
        }

        /* 그 외 입력은 "파일 경로 또는 SQL"로 해석해서 실행한다. */
        if (!execute_argument_or_sql(input, 0, out, err)) {
            had_error = 1;
        }
    }

    return had_error;
}

// 명령줄 옵션과 표준입력을 해석해 실행할 SQL 문자열을 준비합니다.
/* 비대화형 실행에서 CLI 인자를 해석해 최종 SQL 본문을 결정한다. */
/*
 * 비대화형 실행 모드 설명:
 * - help 모드: `sqlparser --help`
 * - 직접 실행 모드: `sqlparser -e "SELECT ..."`
 * - 파일 실행 모드: `sqlparser -f query.sql`
 * - stdin 모드: 파이프/리다이렉션, `-f -`, `sqlparser -`
 * - bare argument 자동 판별 모드: 파일 경로 또는 SQL 문자열 하나
 * - 여러 인자 결합 모드: SQL이 여러 argv 칸으로 들어온 경우
 */
static char *load_noninteractive_sql(int argc, char *argv[], char *error, size_t error_size, int *show_help) {
    /*
     * 인자가 없는데 표준입력이 터미널이 아니면
     * 파이프 입력 모드로 보고 stdin 전체를 읽는다.
     */
    if (argc < 2) {
        if (stdin_is_interactive()) {
            return NULL;
        }
        return read_stream(stdin, error, error_size);
    }

    /* 도움말 옵션은 실제 SQL 실행 대신 main() 쪽에 "usage만 출력하라"는 신호를 준다. */
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        *show_help = 1;
        return NULL;
    }

    /* -e 뒤의 여러 인자를 하나의 SQL 문장으로 합쳐 실행한다. */
    if (strcmp(argv[1], "-e") == 0 || strcmp(argv[1], "--execute") == 0) {
        return join_arguments_as_sql(argc, argv, 2, error, error_size);
    }

    /* -f는 명시적인 파일 입력 모드다. */
    if (strcmp(argv[1], "-f") == 0 || strcmp(argv[1], "--file") == 0) {
        if (argc < 3) {
            snprintf(error, error_size, "missing file path after %s", argv[1]);
            return NULL;
        }

        /* -f - 형태는 파일 대신 표준입력 전체를 파일처럼 읽겠다는 뜻이다. */
        if (strcmp(argv[2], "-") == 0) {
            return read_stream(stdin, error, error_size);
        }

        /* 파일 경로 뒤에 추가 인자가 붙으면 사용법 오류로 처리한다. */
        if (argc > 3) {
            snprintf(error, error_size, "unexpected arguments after file path");
            return NULL;
        }

        return read_entire_file(argv[2], error, error_size);
    }

    /* 알 수 없는 옵션은 조용히 무시하지 않고 명시적 오류로 처리한다. */
    if (argv[1][0] == '-' && strcmp(argv[1], "-") != 0) {
        snprintf(error, error_size, "unknown option: %s", argv[1]);
        return NULL;
    }

    /* 인자 하나가 '-' 뿐이면 비대화형 stdin 입력을 의미한다. */
    if (argc == 2 && strcmp(argv[1], "-") == 0) {
        return read_stream(stdin, error, error_size);
    }

    /* 인자 하나만 있으면 파일 경로인지 SQL 문자열인지 자동 판별한다. */
    if (argc == 2) {
        return load_sql_from_argument(argv[1], 0, error, error_size);
    }

    /* 그 외 여러 개 인자는 공백으로 이어 붙여 하나의 SQL 문장으로 취급한다. */
    return join_arguments_as_sql(argc, argv, 1, error, error_size);
}

/*
 * 프로그램 시작점.
 *
 * 크게 보면 아래 분기만 담당한다.
 * - REPL로 들어갈지
 * - help만 출력할지
 * - SQL을 한 번 실행하고 끝낼지
 */
/*
 * main()은 입력 방식에 따라 모드를 고르는 진입점이다.
 *
 * 전체 모드:
 * - REPL 모드
 * - help 모드
 * - 비대화형 1회 실행 모드(-e, -f, stdin, bare argument)
 *
 * 즉, main()의 핵심 책임은 SQL 처리 자체보다
 * "사용자가 어떤 방식으로 프로그램에 들어왔는지 분기하는 것"이다.
 */
int main(int argc, char *argv[]) {
    char error[256];
    char *sql_text;
    int show_help = 0;
    int exit_code;

    /* 인자 없이 터미널에서 실행된 경우는 REPL 모드로 진입한다. */
    if (argc == 1 && stdin_is_interactive()) {
        exit_code = run_repl(stdout, stderr, argv[0]);
        execution_runtime_reset();
        return exit_code;
    }

    /* 비대화형 모드에서는 먼저 입력 원본을 실제 SQL 문자열로 정규화한다. */
    sql_text = load_noninteractive_sql(argc, argv, error, sizeof(error), &show_help);
    if (show_help) {
        print_usage(stdout, argv[0]);
        execution_runtime_reset();
        return 0;
    }

    /* SQL을 준비하지 못한 경우는 사용법과 함께 오류를 출력한다. */
    if (sql_text == NULL) {
        fprintf(stderr, "error: %s\n", error);
        print_usage(stderr, argv[0]);
        execution_runtime_reset();
        return 1;
    }

    /* SQL 실행 자체가 실패하면 오류 메시지만 출력하고 종료한다. */
    if (!execute_sql_text(sql_text, stdout, error, sizeof(error))) {
        fprintf(stderr, "error: %s\n", error);
        free(sql_text);
        execution_runtime_reset();
        return 1;
    }

    /* 성공 경로에서는 동적 할당한 SQL 문자열과 런타임 인덱스를 정리하고 종료한다. */
    free(sql_text);
    execution_runtime_reset();
    return 0;
}
