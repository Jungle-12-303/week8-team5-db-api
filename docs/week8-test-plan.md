# SQL Processor 8주차 테스트 계획서

## 1. 문서 목적

이 문서는 8주차 `미니 DBMS - API 서버` 구현에 대한 테스트 전략, 범위, 환경, 시나리오, 완료 기준을 정의한다.

본 문서는 다음 문서를 기준으로 작성한다.

- `docs/week8-requirements.md`
- `docs/week8-architecture.md`
- `docs/week8-api-spec.md`

테스트 계획의 목적은 다음과 같다.

- 7주차 엔진 회귀가 깨지지 않았는지 확인한다.
- API 서버 기능이 외부 계약대로 동작하는지 검증한다.
- 멀티스레드 환경에서 동시성 정책이 실제로 지켜지는지 검증한다.
- 발표와 README에 포함할 검증 결과의 기준을 마련한다.

## 2. 테스트 원칙

- 테스트는 `엔진 회귀`, `API 기능`, `동시성`, `복구성`으로 나눠 관리한다.
- 단위 테스트는 빠르게 반복 가능해야 한다.
- API 테스트는 TCP 소켓을 통해 black-box로 검증한다.
- 자동 테스트는 기본적으로 인프로세스 서버를 띄워 HTTP 계약을 검증하고, CLI/프로세스 경계 검증은 별도 보강 항목으로 관리한다.
- 동시성 테스트는 정합성 위반 여부와 병렬성 보장 여부를 함께 본다.
- 테스트 데이터는 항상 격리된 임시 디렉터리에서 준비한다.
- 테스트 결과는 README와 발표에 재현 가능한 형태로 남긴다.

## 3. 테스트 범위

### 3.1 포함 범위

- 7주차 SQL 엔진 회귀 테스트
- HTTP 요청/응답 기능 테스트
- JSON 파싱 및 입력 검증 테스트
- `GET /`
- `GET /health`
- `POST /query`
- 지원 SQL 범위 검증
- 미지원 SQL 처리 검증
- 같은 테이블 직렬화 검증
- 다른 테이블 병렬 처리 검증
- 인덱스 rebuild 및 재시작 후 조회 검증
- queue full 처리 검증
- graceful shutdown 기본 동작 검증

### 3.2 제외 범위

- 장시간 soak test
- 운영 환경 배포 테스트
- 인증/인가 테스트
- TLS/HTTPS 테스트
- 초대형 body streaming 테스트

## 4. 테스트 환경

### 4.1 기본 서버 설정

- host: `127.0.0.1`
- port: `8080`
- worker count: `4`
- queue capacity: `64`
- schema dir: `schema`
- data dir: `data`
- request body max: `16 KiB`
- SQL max length: `8 KiB`
- request line + header max: `8 KiB`

header size 계산 규칙:

- request line + 각 header line + 마지막 빈 줄의 `CRLF`까지 포함한다.
- 본문 바이트는 제외한다.

### 4.2 서버 실행 방식

권장 서버 바이너리:

```text
build/bin/sqlapi_server
```

권장 실행 예시:

```bash
./build/bin/sqlapi_server \
  --host 127.0.0.1 \
  --port 8080 \
  --worker-count 4 \
  --queue-capacity 64 \
  --schema-dir <temp-schema-dir> \
  --data-dir <temp-data-dir>
```

테스트 원칙:

- 각 API 테스트는 임시 `schema/`, `data/` 디렉터리를 만든다.
- 자동 API 테스트는 해당 임시 경로를 `SqlApiServerConfig`에 직접 주입해 서버를 실행한다.
- 별도 프로세스 기반 검증을 추가하는 경우에는 동일한 임시 경로를 CLI 옵션으로 주입한다.
- 테스트는 운영 데이터나 저장소 루트의 기본 `schema/`, `data/`를 직접 오염시키지 않는다.

### 4.3 테스트용 데이터 경로

- 각 테스트는 임시 루트 디렉터리를 생성한다.
- 그 아래에 `schema/`, `data/`를 별도로 만든다.
- 테스트 종료 후 임시 디렉터리는 삭제 가능해야 한다.

### 4.4 테스트용 테이블 구성

동시성 검증을 위해 최소 2개 테이블을 사용한다.

권장 테이블:

- `student`
- `product`

권장 이유:

- 같은 테이블 요청 직렬화 검증이 가능하다.
- 서로 다른 테이블 요청 병렬 처리 검증이 가능하다.

## 5. 테스트 대상 분류

### 5.1 엔진 회귀 테스트

목적:

- API 서버 추가 이후에도 7주차 SQL 엔진 동작이 깨지지 않았는지 확인한다.

대상:

- lexer
- parser
- executor
- storage
- index
- benchmark helper

### 5.2 API 기능 테스트

목적:

- 외부 API 계약이 명세서대로 동작하는지 확인한다.

대상:

- route
- method validation
- header validation
- JSON parsing
- success/error response
- server start option validation

### 5.3 동시성 테스트

목적:

- table-level exclusive lock과 registry mutex 전략이 실제로 정합성과 병렬성을 보장하는지 확인한다.

대상:

- 같은 테이블 동시 `INSERT`
- 같은 테이블 read/write 혼합
- 다른 테이블 병렬 요청
- queue full

### 5.4 복구성 테스트

목적:

- 인덱스 무효화, rebuild, 서버 재시작 이후에도 조회가 가능한지 확인한다.

대상:

- lazy rebuild
- index invalidate 후 재조회
- 서버 재시작 후 재조회

## 6. API 테스트 체크리스트

이 체크리스트는 `docs/week8-requirements.md`, `docs/week8-api-spec.md`를 기준으로 만든다.

표기 규칙:

- `완료`: 현재 자동 테스트로 검증 중
- `추가 필요`: 문서에는 있으나 현재 자동 테스트가 부족함

### 6.1 기본 엔드포인트 계약

- `GET /health` 200 응답: `완료`
- `GET /` HTML 진입 페이지 응답: `완료`
- `GET /`에서 request body 금지: `완료`
- `GET /health` 응답 필드 `ok`, `status`, `worker_count`, `queue_depth`: `완료`
- `GET /health` 응답 `Content-Type: application/json; charset=utf-8`: `완료`
- `GET /health`에서 request body 금지: `완료`
- `GET /health`에서 `Content-Length: 0` 허용: `완료`
- `POST /query` 정상 `SELECT` 실행: `완료`
- `POST /query` 성공 응답 `Content-Type: application/json; charset=utf-8`: `완료`
- `POST /query`에서 `application/json; charset=utf-8` 허용: `완료`
- `POST /query` 응답 `output`이 LF 줄바꿈을 유지하고 CRLF로 바뀌지 않음: `완료`

### 6.2 HTTP 요청 형식 검증

- `POST /query`에서 `Content-Type` 누락/비지원 시 `400 INVALID_CONTENT_TYPE`: `완료`
- `charset=utf-8` 외 charset 거부: `완료`
- `Content-Length` 누락 시 `411 CONTENT_LENGTH_REQUIRED`: `완료`
- `Content-Length` 비정상 값 시 `400 INVALID_CONTENT_LENGTH`: `완료`
- body 길이와 `Content-Length` 불일치 시 `400 INVALID_CONTENT_LENGTH`: `완료`
- request body 제한 초과 시 `413 PAYLOAD_TOO_LARGE`: `완료`
- request line + header 제한 초과 시 `431 HEADER_TOO_LARGE`: `완료`
- `Transfer-Encoding: chunked` 사용 시 `501 CHUNKED_NOT_SUPPORTED`: `완료`
- 존재하지 않는 경로 `404 NOT_FOUND`: `완료`
- 존재하는 경로의 잘못된 메서드 `405 METHOD_NOT_ALLOWED`: `완료`

### 6.3 JSON 및 SQL 입력 검증

- 잘못된 JSON 시 `400 INVALID_JSON`: `완료`
- `sql` 필드 누락 시 `400 MISSING_SQL_FIELD`: `완료`
- parser 실패 시 `400 SQL_PARSE_ERROR`: `완료`
- 지원하지 않는 SQL 시 `400 UNSUPPORTED_SQL`: `완료`
- 잘못된 SQL 인자 시 `400 INVALID_SQL_ARGUMENT`: `완료`
- lexer 실패 시 `400 SQL_LEX_ERROR`: `완료`
- multi-statement 거부: `완료`

### 6.4 엔진/스토리지 오류 매핑

- 스키마 로딩 실패 시 `500 SCHEMA_LOAD_ERROR`: `완료`
- CSV 파일 I/O 실패 시 `500 STORAGE_IO_ERROR`: `완료`
- 인덱스 rebuild 실패 시 `500 INDEX_REBUILD_ERROR`: `추가 필요`
- 분류되지 않은 executor 실패 시 `500 ENGINE_EXECUTION_ERROR`: `추가 필요`
- 분류되지 않은 내부 오류 시 `500 INTERNAL_ERROR`: `추가 필요`

### 6.5 서버 시작/종료 및 환경 검증

- `--port` 범위 검증: `완료`
- `--worker-count >= 1` 검증: `완료`
- `--queue-capacity >= 1` 검증: `완료`
- 존재하지 않는 `--schema-dir` 거부: `완료`
- 존재하지 않는 `--data-dir` 거부: `완료`
- 종료 경로에서 accept thread / worker 정리: `추가 필요`

### 6.6 병렬성 및 동시성 검증

- 다중 동시 요청 처리: `추가 필요`
- 같은 테이블 동시 접근 직렬화: `추가 필요`
- 다른 테이블 요청 병렬 처리: `추가 필요`
- queue full 시 `503 QUEUE_FULL`: `완료`
- race condition / deadlock / unlock 누락 점검: `추가 필요`
- 재시작 후 데이터 조회 가능 여부: `추가 필요`
- 인덱스 기반 조회 경로 유지: `추가 필요`

## 7. 현재 자동 테스트 반영 전략

현재 `tests/test_api_server.c`는 인프로세스로 서버를 띄운 뒤, 실제 TCP 소켓으로 요청을 보내는 black-box 방식으로 아래 항목을 자동 검증한다.

- 헬스체크 정상/비정상 요청
- 루트 HTML 페이지 노출
- `POST /query` 정상 요청
- `POST /query` 응답 `output`의 LF 줄바꿈 유지
- 헤더, body, JSON, 경로, 메서드 관련 대표 오류 코드
- 대표 SQL 오류 매핑
- 대표 스토리지 I/O 오류 매핑
- 시작 옵션/데이터 경로 검증
- queue full 시 `503 QUEUE_FULL`

향후 별도 테스트 파일 또는 보조 스레드/프로세스 fixture가 필요한 항목은 다음과 같다.

- 병렬 요청 및 락 직렬화 검증
- 시작 옵션 실패를 별도 프로세스로 검증하는 테스트
- 재시작/복구성 중심 시나리오
- graceful shutdown drain 보장 검증

## 8. 테스트 코드 관리 원칙

- 엔진 테스트와 서버 테스트는 파일을 분리한다.
- 다만 실행은 하나의 테스트 타깃으로 통합할 수 있다.

권장 파일 구조:

```text
tests/
  test_sql_engine.c
  test_api_server.c
  test_concurrency.c
  test_recovery.c
  test_main.c
```

현재 `tests/test_runner.c`에 있는 7주차 엔진 회귀 테스트는 유지하거나 `test_sql_engine.c`로 분리한다.

## 9. 테스트 데이터셋

### 9.1 `student`

예시 스키마:

```text
table=student
columns=department,student_number,name,age
```

예시 CSV:

```csv
department,student_number,name,age
컴퓨터공학과,2024001,김민수,20
수학과,2024002,이수학,21
```

### 9.2 `product`

예시 스키마:

```text
table=product
columns=name,category,price,stock
```

예시 CSV:

```csv
name,category,price,stock
노트북,전자기기,1500000,5
키보드,전자기기,120000,30
```

### 9.3 alias lock 정규화용 테이블

락 키 정규화 검증을 위해 alias 형태의 스키마도 준비한다.

예시:

- 파일명: `student_alias.meta`
- meta 내용:

```text
table=student
columns=department,student_number,name,age
```

- 데이터 파일: `student_alias.csv`

fixture 규칙:

- 이 케이스는 별도 테스트 fixture에서 수행한다.
- 같은 fixture 안에 `student.meta`를 함께 두지 않는다.

이유:

- 현재 스키마 탐색은 exact filename `<table>.meta`를 먼저 찾는다.
- 따라서 `student.meta`와 `student_alias.meta`를 동시에 두면 alias 정규화 테스트 의도가 흐려질 수 있다.

검증 의도:

- SQL에서 `student`와 `student_alias`를 모두 사용할 수 있어야 한다.
- 두 이름은 같은 물리 테이블을 가리키므로 lock key는 동일한 `storage_name=student_alias`로 정규화되어야 한다.

## 8. 단위 테스트 계획

### 8.1 엔진 회귀

- B+ tree 삽입/탐색
- split 이후 검색 가능성 유지
- parser의 `WHERE` 파싱
- parser 오류 메시지
- UTF-8 identifier 처리
- `INSERT` 자동 id
- 명시적 `id` 컬럼 금지
- `WHERE id = ...` 인덱스 조회
- index rebuild after reset
- index invalidate 후 rebuild
- CSV escape 처리

### 8.2 서버 보조 모듈

- task queue enqueue/dequeue
- queue full 판정
- router path dispatch
- method validation
- JSON body parser
- engine lock manager
- shutdown flag 처리

## 9. API 기능 테스트 계획

### 9.1 `GET /health`

- 정상 `GET /health`는 `200`
- 응답 본문은 JSON
- `ok=true`
- `status=ok`
- `worker_count` 존재
- `queue_depth` 존재
- `POST /health`는 `405`
- body가 있으면 `400`
- `Content-Length: 0`은 허용
- body가 없으면 `Content-Type` 헤더가 있어도 허용

### 9.2 `GET /`

- 정상 `GET /`는 `200`
- 응답 본문은 HTML
- SQL 입력 textarea와 실행 버튼이 포함됨
- 페이지 내부에서 `POST /query` 호출 경로가 드러남
- `help`, `.help`, `--help`는 브라우저 로컬 help로 처리됨
- body가 있으면 `400`
- `POST /`는 `405`

### 9.3 `POST /query` 정상 케이스

- `SELECT * FROM student;`
- `SELECT id, name FROM student WHERE id = 1;`
- `SELECT name FROM student WHERE department = '컴퓨터공학과';`
- `INSERT INTO student (...) VALUES (...);`

검증 항목:

- 상태 코드 `200`
- `ok=true`
- `statement_type`
- `affected_rows`
- `summary`
- `output`
- `INSERT` 성공 시 `output=""`
- `SELECT` 성공 시 `output` 문자열은 LF(`\n`) 기준 줄바꿈을 유지하고 CRLF(`\r\n`)로 바뀌지 않는다.
- 브라우저/Windows 계열 환경에서도 `response.json()` 이후 `pre.textContent`에 넣었을 때 LF 줄바꿈으로 정상 표시되어야 한다.
- 결과 행 수가 많은 `SELECT`도 `500 INTERNAL_ERROR` 없이 성공 응답으로 반환돼야 한다.

### 9.4 `POST /query` 비정상 케이스

- 잘못된 JSON
- `Content-Type` 누락 또는 비지원
- `Content-Type: application/json; charset=utf-8` 허용
- `Content-Type: application/json;charset=UTF-8` 허용
- `Content-Type`에 비지원 charset 또는 추가 파라미터 포함
- `Content-Length` 누락
- `Content-Length`가 숫자가 아님
- `Content-Length`와 실제 body 길이 불일치
- body 크기 제한 초과
- SQL 길이 제한 초과
- request line 또는 header 제한 초과
- folded header
- `Transfer-Encoding: chunked`
- `sql` 필드 누락
- 공백만 있는 SQL
- parser 오류 SQL
- 미지원 SQL
- `WHERE id = abc`
- 존재하지 않는 테이블
- 존재하지 않는 컬럼

검증 항목:

- 상태 코드가 명세와 일치하는지
- `ok=false`
- `error.code`
- `error.message`

### 9.5 경로 및 메서드 케이스

- 없는 경로는 `404`
- `GET /query`는 `405`
- `POST /`는 `405`
- `POST /health`는 `405`

### 9.6 오류 코드 매핑 검증

아래 매핑은 명세서와 1:1로 일치해야 한다.

- `INVALID_JSON` -> `400`
- `INVALID_CONTENT_TYPE` -> `400`
- `CONTENT_LENGTH_REQUIRED` -> `411`
- `INVALID_CONTENT_LENGTH` -> `400`
- `PAYLOAD_TOO_LARGE` -> `413`
- `HEADER_TOO_LARGE` -> `431`
- `CHUNKED_NOT_SUPPORTED` -> `501`
- `MISSING_SQL_FIELD` -> `400`
- `SQL_LEX_ERROR` -> `400`
- `SQL_PARSE_ERROR` -> `400`
- `UNSUPPORTED_SQL` -> `400`
- `INVALID_SQL_ARGUMENT` -> `400`
- `SCHEMA_LOAD_ERROR` -> `500`
- `STORAGE_IO_ERROR` -> `500`
- `INDEX_REBUILD_ERROR` -> `500`
- `ENGINE_EXECUTION_ERROR` -> `500`
- `QUEUE_FULL` -> `503`

### 9.7 서버 시작 옵션 검증

- `--port 0` -> 시작 실패
- `--port 65536` -> 시작 실패
- `--worker-count 0` -> 시작 실패
- `--queue-capacity 0` -> 시작 실패
- 존재하지 않는 `--schema-dir` -> 시작 실패
- 존재하지 않는 `--data-dir` -> 시작 실패

검증 항목:

- 서버 프로세스가 listen을 시작하지 않는지
- 표준 오류 출력에 오류 메시지가 남는지
- `exit code 1`로 종료하는지

### 9.8 header size 경계값 검증

- request line + header가 `8192` 바이트인 요청 허용 여부
- request line + header가 `8193` 바이트인 요청 거부 여부
- `CRLF` 포함 계산이 명세와 일치하는지

## 10. 동시성 테스트 계획

### 10.1 같은 테이블 동시 `INSERT`

시나리오:

- `student` 테이블에 동시에 여러 `INSERT` 요청을 보낸다.

검증:

- 모든 성공 응답이 유실 없이 반환되는지
- 재조회 시 삽입 행 수가 기대치와 일치하는지
- 내부 `id`가 중복되지 않는지
- CSV와 인덱스 조회 결과가 일치하는지

### 10.2 같은 테이블 read/write 혼합

시나리오:

- `student` 테이블에 긴 `INSERT` 또는 여러 `INSERT`를 보내는 동안 `SELECT` 요청을 함께 보낸다.

검증:

- 서버가 crash하지 않는지
- 결과가 깨지지 않는지
- 직렬화 정책 때문에 같은 테이블 요청이 순서대로 처리되는지

### 10.3 다른 테이블 병렬 처리

시나리오:

- `student`와 `product`에 각각 요청을 동시에 보낸다.

검증:

- 서로 다른 테이블 요청이 동시에 진행될 수 있는지
- 한쪽 테이블 작업 때문에 다른 테이블 요청이 불필요하게 대기하지 않는지

### 10.4 alias 이름과 storage 이름 락 정규화

시나리오:

- 같은 물리 테이블을 `student`와 `student_alias` 두 이름으로 동시에 접근한다.

검증:

- 두 요청이 서로 다른 락으로 병행 실행되지 않는지
- `id` 중복, rebuild race, CSV/인덱스 불일치가 발생하지 않는지

### 10.5 queue full

시나리오:

- worker 수보다 훨씬 많은 연결을 짧은 시간 안에 동시에 보낸다.

검증:

- 일부 요청이 `503`으로 반환되는지
- 서버가 hang 없이 계속 응답 가능한 상태인지
- 큐 포화 이후에도 이후 요청을 정상 처리할 수 있는지

## 11. 복구성 테스트 계획

### 11.1 index invalidate 후 rebuild

- 일부 요청 처리 후 인덱스를 무효화한다.
- 같은 `WHERE id = ...` 조회를 다시 실행한다.

검증:

- rebuild 이후에도 같은 결과가 반환되는지

### 11.2 인덱스 등록 실패 후 복구

- 테스트 hook으로 index register 실패를 유도한다.
- 다음 인덱스 조회를 실행한다.

검증:

- 실패 요청은 오류로 끝나는지
- 이후 조회는 rebuild로 복구되는지

### 11.3 서버 재시작 후 조회

- 서버를 종료한다.
- 같은 `schema/`, `data/`를 유지한 채 서버를 다시 시작한다.
- `WHERE id = ...` 조회를 수행한다.

검증:

- 기존 데이터가 동일하게 조회되는지
- lazy rebuild가 정상 동작하는지

### 11.4 graceful shutdown

시나리오:

- 처리 중인 요청이 존재하는 상태에서 서버 종료를 시작한다.

종료 트리거:

- 운영에서는 프로세스 종료 신호
- 테스트에서는 stop hook 또는 종료 신호 전달

검증:

- shutdown 시작 전 큐에 들어간 작업은 유실 없이 처리되는지
- accept loop가 중단되는지
- worker가 deadlock 없이 종료되는지
- 프로세스가 정상 exit code로 종료되는지
- shutdown 시작 이후 새 연결은 HTTP 응답이 없거나 즉시 종료되어도 허용되는지

## 12. 완료 기준

아래 조건을 모두 만족하면 테스트 계획 기준 구현 검증 완료로 본다.

- 7주차 엔진 회귀 테스트가 통과한다.
- `GET /health` 기능 테스트가 통과한다.
- `GET /` 기능 테스트가 통과한다.
- `POST /query`의 정상/비정상 기능 테스트가 통과한다.
- 서버 시작 옵션 검증 테스트가 통과한다.
- 같은 테이블 동시성 정합성 테스트가 통과한다.
- 다른 테이블 병렬 처리 테스트가 통과한다.
- alias lock 정규화 테스트가 통과한다.
- queue full 테스트가 통과한다.
- 서버 재시작 후 rebuild 테스트가 통과한다.
- graceful shutdown 테스트가 통과한다.

## 13. README 및 발표 반영 항목

README와 발표에는 최소한 아래 내용을 포함한다.

- 실행 방법
- 테스트 실행 방법
- 대표 성공 테스트 결과
- 대표 오류 처리 결과
- 동시성 테스트 결과
- 다른 테이블 병렬 처리 시연 결과
- alias 이름과 storage 이름이 같은 락으로 직렬화되는 검증 결과
- queue full 또는 부하 상황 처리 결과
- 재시작 후 rebuild 확인 결과
- graceful shutdown 확인 결과

## 14. 우선순위

### P0

- 엔진 회귀
- `GET /health`
- `GET /`
- `POST /query` 정상/비정상
- 같은 테이블 동시 `INSERT`

### P1

- 다른 테이블 병렬 처리
- alias lock 정규화
- index rebuild
- 서버 재시작 후 조회

### P2

- queue full
- graceful shutdown
- 추가 엣지 케이스
