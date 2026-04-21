# Makefile 학습 가이드

## 1. 이 문서의 목적

이 문서는 이 프로젝트에 들어 있는 `Makefile`이 무엇인지 설명하고, 실제로 어떻게 사용하는지 초심자 기준으로 정리한 학습용 문서입니다.

설명 대상은 루트 경로의 [Makefile](/C:/developer_folder/jungle-sql-processor-2nd/Makefile)이며, 현재 프로젝트의 빌드 방식은 이 파일을 기준으로 이해하는 것이 가장 좋습니다.

## 2. Makefile이란 무엇인가

`Makefile`은 프로젝트를 빌드하거나 테스트할 때 필요한 명령을 정리해 둔 파일입니다.

쉽게 말하면 아래 같은 명령을 매번 길게 직접 치지 않도록 묶어 둔 자동화 스크립트입니다.

```bash
gcc -Wall -Wextra -std=c11 -Iinclude -o build/bin/sqlparser ...
```

`make`라는 도구는 `Makefile`을 읽고, 그 안에 적힌 규칙에 따라 필요한 파일을 빌드합니다.

즉:

- 어떤 소스 파일들을 컴파일할지
- 어떤 이름의 실행 파일을 만들지
- 테스트는 어떻게 실행할지
- 빌드 결과물은 어디에 둘지

를 한 곳에서 관리할 수 있게 해 줍니다.

## 3. 왜 이 프로젝트에서 Makefile이 필요한가

이 프로젝트는 C 파일이 여러 개로 나뉘어 있습니다.

- `src/app`
- `src/sql`
- `src/execution`
- `src/storage`
- `src/index`
- `src/benchmark`

직접 `gcc` 명령으로 빌드하려면 소스 파일 목록을 길게 모두 적어야 하고, 테스트 바이너리와 벤치마크 바이너리도 각각 따로 빌드해야 합니다.

그래서 `Makefile`이 있으면 아래처럼 짧은 명령으로 작업할 수 있습니다.

```bash
make all
make test
make benchmark
```

즉, 이 프로젝트에서 `Makefile`은 다음 역할을 합니다.

- 빌드 명령을 표준화
- 팀원마다 같은 방식으로 빌드 가능
- 앱 실행 파일, 테스트 실행 파일, 벤치마크 실행 파일을 구분해 생성
- 소스 목록을 한 곳에서 관리

## 4. 현재 프로젝트 Makefile의 핵심 구조

현재 [Makefile](/C:/developer_folder/jungle-sql-processor-2nd/Makefile)은 크게 아래 요소로 이루어져 있습니다.

### 4.1 변수

예를 들면 아래와 같은 변수들이 있습니다.

```makefile
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -Iinclude
TEST_CFLAGS = $(CFLAGS) -DSQLPARSER_BENCHMARK_NO_MAIN
```

의미:

- `CC`
  사용할 컴파일러입니다. 현재는 `gcc`입니다.
- `CFLAGS`
  공통 컴파일 옵션입니다.
- `TEST_CFLAGS`
  테스트 빌드 전용 옵션입니다.

`-Wall -Wextra`는 경고를 많이 보여 주는 옵션이고, `-std=c11`은 C11 표준으로 컴파일하겠다는 뜻입니다.

### 4.2 경로 변수

```makefile
BUILD_DIR = build
BIN_DIR = $(BUILD_DIR)/bin
```

의미:

- `build/`
  빌드 결과물을 넣는 폴더
- `build/bin/`
  실제 실행 파일이 들어가는 폴더

### 4.3 소스 파일 묶음

이 프로젝트는 공통 소스와 용도별 소스를 나눠 관리합니다.

```makefile
COMMON_SOURCES = \
	src/common/util.c \
	src/storage/schema.c \
	...
```

그리고 그 위에 아래처럼 용도별 묶음을 만듭니다.

```makefile
APP_SOURCES = src/app/main.c $(COMMON_SOURCES)
TEST_SOURCES = tests/test_runner.c src/benchmark/benchmark_main.c $(COMMON_SOURCES)
BENCHMARK_SOURCES = src/benchmark/benchmark_main.c $(COMMON_SOURCES)
```

의미:

- `APP_SOURCES`
  실제 SQL 처리기 CLI 빌드에 필요한 소스
- `TEST_SOURCES`
  테스트 러너 빌드에 필요한 소스
- `BENCHMARK_SOURCES`
  벤치마크 실행 파일 빌드에 필요한 소스

이렇게 해 두면 공통 소스 목록을 한 번만 관리하면 됩니다.

## 5. 타깃이란 무엇인가

`Makefile`에서 타깃은 `make`가 실행할 작업 이름입니다.

예를 들어:

```makefile
all: $(APP_BIN)
```

여기서 `all`이 타깃입니다.

즉, 사용자가 아래처럼 입력하면:

```bash
make all
```

`APP_BIN`을 만들기 위한 규칙을 따라 컴파일이 진행됩니다.

## 6. 현재 프로젝트의 주요 타깃

### 6.1 `make all`

```bash
make all
```

역할:

- 메인 SQL 처리기 실행 파일을 빌드합니다.

결과물:

- `build/bin/sqlparser`

이 바이너리는 실제 CLI 프로그램입니다.

### 6.2 `make test`

```bash
make test
```

역할:

- `sqlparser` 실행 파일과 `test_runner`를 빌드합니다.
- 그 뒤 테스트를 바로 실행합니다.

현재 `Makefile`에서는 아래처럼 정의돼 있습니다.

```makefile
test: $(APP_BIN) $(TEST_BIN)
	./$(TEST_BIN)
```

즉:

1. `sqlparser` 빌드
2. `test_runner` 빌드
3. `test_runner` 실행

순서로 진행됩니다.

결과물:

- `build/bin/sqlparser`
- `build/bin/test_runner`

### 6.3 `make benchmark`

```bash
make benchmark
```

역할:

- 벤치마크 실행 파일을 빌드합니다.

결과물:

- `build/bin/benchmark_runner`

이 파일은 대량 삽입과 조회 시간을 측정하는 별도 프로그램입니다.

### 6.4 `make clean`

```bash
make clean
```

역할:

- `build/` 폴더를 삭제해 빌드 결과물을 정리합니다.

현재 정의:

```makefile
clean:
	rm -rf $(BUILD_DIR)
```

주의:

- 이 명령은 Linux/Docker 환경 기준입니다.
- Windows 기본 셸에서는 그대로 동작하지 않을 수 있습니다.

## 7. 실제 사용 예시

### 7.1 일반 빌드

```bash
make all
./build/bin/sqlparser --help
```

### 7.2 테스트 실행

```bash
make test
```

### 7.3 벤치마크 빌드 후 실행

```bash
make benchmark
./build/bin/benchmark_runner benchmark-workdir/schema benchmark-workdir/data student 1000000 100
```

## 8. Docker 환경에서 왜 더 유용한가

이 프로젝트는 Docker/Linux 기준으로 실행하는 흐름을 권장합니다.

그 이유는:

- 팀원마다 OS가 달라도 같은 빌드 명령 사용 가능
- `gcc`, `make` 환경을 통일 가능
- README, 테스트, 벤치마크 실행법을 하나로 맞출 수 있음

즉, Docker 안에서 `make all`, `make test`, `make benchmark`만 기억하면 대부분의 작업이 정리됩니다.

## 9. Windows에서는 어떻게 쓰는가

Windows에서는 환경에 따라 아래처럼 사용할 수 있습니다.

```powershell
mingw32-make all
mingw32-make test
mingw32-make benchmark
```

다만 현재 `clean` 타깃은 Linux 명령 `rm -rf`를 사용하므로, Windows에서는 별도 보완이 필요할 수 있습니다.

그래서 이 프로젝트는 가능하면 Docker/Linux 환경에서 `make`를 쓰는 것이 가장 안전합니다.

## 10. 이 Makefile을 읽는 법

현재 `Makefile`을 읽을 때는 아래 순서가 좋습니다.

1. `CC`, `CFLAGS` 같은 공통 변수 보기
2. `COMMON_SOURCES`, `APP_SOURCES`, `TEST_SOURCES`, `BENCHMARK_SOURCES` 보기
3. `APP_BIN`, `TEST_BIN`, `BENCHMARK_BIN` 보기
4. `all`, `test`, `benchmark`, `clean` 타깃 보기

이 순서로 보면:

- 무엇을 빌드하는지
- 어떤 파일이 포함되는지
- 어떤 명령이 실행되는지

를 빠르게 이해할 수 있습니다.

## 11. 수정할 때 주의할 점

`Makefile`을 수정할 때는 아래를 주의해야 합니다.

- 새 소스 파일을 추가했으면 `COMMON_SOURCES` 또는 해당 타깃 소스 목록에 넣어야 합니다.
- 테스트에서 쓰는 바이너리가 있으면 `test` 타깃 의존성에 반영해야 합니다.
- 벤치마크 전용 코드는 `BENCHMARK_SOURCES`에 포함해야 합니다.
- Linux 명령과 Windows 명령이 다를 수 있으므로, 실행 셸 환경을 고려해야 합니다.

예를 들어 소스 파일은 추가했는데 `Makefile`에 넣지 않으면, 코드는 있어도 실제 빌드 결과물에는 반영되지 않을 수 있습니다.

## 12. 요약

이 프로젝트에서 `Makefile`은 빌드와 테스트의 출발점입니다.

핵심만 기억하면 아래와 같습니다.

- `make all`
  메인 SQL 처리기 빌드
- `make test`
  테스트 빌드 + 실행
- `make benchmark`
  벤치마크 실행 파일 빌드
- `make clean`
  빌드 산출물 정리

즉, `Makefile`은 이 프로젝트의 "컴파일과 테스트를 위한 작업 메뉴판"이라고 이해하면 됩니다.
