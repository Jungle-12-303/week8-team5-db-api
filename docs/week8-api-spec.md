# SQL Processor 8주차 API 명세서

## 1. 문서 목적

이 문서는 8주차 `미니 DBMS - API 서버`의 외부 API 계약을 정의한다.

본 문서는 `docs/week8-requirements.md`, `docs/week8-architecture.md`를 바탕으로, 외부 클라이언트가 어떤 요청을 보내고 어떤 응답을 받는지 명시한다.

## 2. 설계 범위

이번 API 명세는 8주차 최소 구현 범위를 대상으로 한다.

- 프로토콜: `HTTP/1.1`
- 전송: `TCP`
- 본문 형식: `application/json; charset=utf-8`
- 엔드포인트: `GET /`, `GET /health`, `POST /query`

기본 서버 설정은 다음과 같다.

- listen host: `127.0.0.1`
- listen port: `8080`
- worker count: `4`
- queue capacity: `64`
- schema dir: `schema`
- data dir: `data`
- 최대 request body 크기: `16 KiB`
- 최대 SQL 문자열 길이: `8 KiB`
- 최대 request line + header 크기: `8 KiB`

본 명세는 일반적인 서비스형 REST API 전체를 목표로 하지 않는다.

## 3. 공통 규칙

### 3.1 문자 인코딩

- 요청과 응답 본문은 UTF-8이어야 한다.
- SQL 문자열 안의 한글 테이블명, 컬럼명, 값은 UTF-8 기준으로 처리한다.

### 3.2 Content-Type

- `GET /`는 `text/html; charset=utf-8`을 반환한다.
- `GET /`는 요청 본문이 없어야 한다.
- `POST /query` 요청은 `Content-Type: application/json`이어야 한다.
- `GET /health`, `POST /query`의 성공 응답과 오류 응답은 `application/json; charset=utf-8`로 반환한다.
- `GET /health`는 요청 본문이 없어야 한다.
- `GET /health`에서 `Content-Length: 0`은 허용한다.
- `GET /health`에서 body가 없으면 `Content-Type` 헤더는 있어도 무시한다.

허용 규칙:

- `application/json`
- `application/json; charset=utf-8`
- 대소문자는 구분하지 않는다.
- `charset=utf-8` 앞뒤 공백은 허용한다.

비허용 규칙:

- `text/plain`
- `application/json` 이외의 media type
- `application/json`에 `charset=utf-8` 외의 charset 파라미터가 붙은 경우
- 의미를 알 수 없는 추가 파라미터가 붙은 경우

### 3.3 연결 정책

- 기본 구현은 요청 하나를 처리한 뒤 연결을 종료해도 된다.
- 이번 최소 구현은 `1 connection = 1 request`를 사용한다.
- keep-alive는 지원하지 않는다.
- HTTP pipelining은 지원하지 않는다.

### 3.4 데이터셋 경로 설정

서버는 다음 런타임 옵션을 통해 데이터셋 경로를 결정한다.

- `--schema-dir`
- `--data-dir`

기본값은 각각 `schema`, `data`이며, 상대 경로는 서버 프로세스의 현재 working directory 기준으로 해석한다.

### 3.5 서버 시작 옵션 검증

서버는 시작 시 아래 조건을 검증해야 한다.

- `--port`는 `1..65535`
- `--worker-count >= 1`
- `--queue-capacity >= 1`
- `--schema-dir`는 존재하는 디렉터리
- `--data-dir`는 존재하는 디렉터리

위 검증에 실패하면:

- 서버는 listen을 시작하지 않는다.
- 표준 오류 출력에 오류 메시지를 남긴다.
- `exit code 1`로 종료한다.

### 3.6 단일 SQL 문장

- `POST /query`는 한 요청당 SQL 문장 하나만 허용한다.
- multi-statement 실행은 지원하지 않는다.

### 3.7 SQL 지원 범위

허용:

- `INSERT INTO ... VALUES (...)`
- `SELECT * FROM ...`
- `SELECT column1, column2 FROM ...`
- `SELECT ... WHERE column = value`
- `SELECT ... WHERE id = <integer>`

비허용:

- `UPDATE`
- `DELETE`
- `JOIN`
- `ORDER BY`
- `GROUP BY`
- `AND`, `OR`를 포함한 복합 `WHERE`
- 여러 SQL 문장을 한 번에 전송하는 요청

### 3.8 `id` 의미

- API에서 SQL의 `id`는 사용자 컬럼이 아니다.
- `id`는 내부 예약 키 `__internal_id`를 뜻한다.
- 사용자 스키마에 `id` 컬럼이 존재해서는 안 된다.

### 3.9 canonical table key

동시성 제어의 기준 테이블 이름은 raw SQL table name이 아니라 `load_schema()` 이후 확정되는 `schema.storage_name`이다.

따라서:

- SQL에서 alias 이름으로 같은 물리 테이블을 가리켜도
- lock key와 내부 보호 기준은 동일한 `storage_name`으로 정규화된다

## 4. 상태 코드

- `200 OK`: 요청이 정상적으로 처리됨
- `400 Bad Request`: 잘못된 JSON, 잘못된 SQL, 미지원 SQL, 잘못된 파라미터
- `411 Length Required`: `POST /query`에서 필요한 `Content-Length`가 없음
- `413 Payload Too Large`: request body 또는 SQL 길이가 제한을 초과함
- `431 Request Header Fields Too Large`: request line 또는 header가 제한을 초과함
- `404 Not Found`: 존재하지 않는 경로
- `405 Method Not Allowed`: 존재하는 경로에 대해 지원하지 않는 메서드 사용
- `500 Internal Server Error`: 내부 엔진 오류, 파일 I/O 오류, 인덱스 rebuild 실패
- `501 Not Implemented`: 지원하지 않는 HTTP 전송 방식 사용
- `503 Service Unavailable`: 작업 큐 포화 등으로 요청을 수용할 수 없음

## 5. 오류 코드

오류 응답의 `error.code`는 아래 집합을 사용한다.

- `INVALID_JSON`
- `MISSING_SQL_FIELD`
- `INVALID_CONTENT_TYPE`
- `CONTENT_LENGTH_REQUIRED`
- `INVALID_CONTENT_LENGTH`
- `PAYLOAD_TOO_LARGE`
- `HEADER_TOO_LARGE`
- `CHUNKED_NOT_SUPPORTED`
- `METHOD_NOT_ALLOWED`
- `NOT_FOUND`
- `SQL_LEX_ERROR`
- `SQL_PARSE_ERROR`
- `UNSUPPORTED_SQL`
- `INVALID_SQL_ARGUMENT`
- `ENGINE_EXECUTION_ERROR`
- `SCHEMA_LOAD_ERROR`
- `STORAGE_IO_ERROR`
- `INDEX_REBUILD_ERROR`
- `QUEUE_FULL`
- `INTERNAL_ERROR`

## 6. 오류 코드 매핑 규칙

대표적인 오류 매핑 규칙은 다음과 같다.

| 상황 | HTTP 상태 코드 | error.code |
| --- | --- | --- |
| JSON 파싱 실패 | 400 | `INVALID_JSON` |
| `Content-Type` 누락 또는 비지원 | 400 | `INVALID_CONTENT_TYPE` |
| `GET /health`에 body가 존재 | 400 | `INVALID_JSON` |
| `GET /`에 body가 존재 | 400 | `INVALID_JSON` |
| `Content-Length` 누락 | 411 | `CONTENT_LENGTH_REQUIRED` |
| `Content-Length` 값이 숫자가 아니거나 body와 불일치 | 400 | `INVALID_CONTENT_LENGTH` |
| body 크기 또는 SQL 길이 제한 초과 | 413 | `PAYLOAD_TOO_LARGE` |
| request line 또는 header 크기 제한 초과 | 431 | `HEADER_TOO_LARGE` |
| `Transfer-Encoding: chunked` 사용 | 501 | `CHUNKED_NOT_SUPPORTED` |
| `sql` 필드 누락 | 400 | `MISSING_SQL_FIELD` |
| lexer 실패 | 400 | `SQL_LEX_ERROR` |
| parser 실패 | 400 | `SQL_PARSE_ERROR` |
| 현재 엔진이 지원하지 않는 SQL | 400 | `UNSUPPORTED_SQL` |
| `WHERE id = abc` 같은 잘못된 인자 | 400 | `INVALID_SQL_ARGUMENT` |
| 스키마 meta 해석 실패, 테이블명 불일치, 필수 필드 누락 | 500 | `SCHEMA_LOAD_ERROR` |
| schema meta 파일 또는 CSV 파일 열기/읽기 실패 | 500 | `STORAGE_IO_ERROR` |
| 인덱스 rebuild 실패 | 500 | `INDEX_REBUILD_ERROR` |
| 그 외 executor 실패 | 500 | `ENGINE_EXECUTION_ERROR` |
| 작업 큐 포화 | 503 | `QUEUE_FULL` |
| 분류되지 않은 내부 오류 | 500 | `INTERNAL_ERROR` |

## 7. `GET /health`

### 7.1 목적

- 서버 프로세스가 요청을 받을 수 있는지 확인한다.
- 발표, 기능 테스트, 간단한 헬스체크 용도로 사용한다.

### 7.2 요청

```http
GET /health HTTP/1.1
Host: 127.0.0.1:8080
```

요청 본문은 없다.

규칙:

- `GET /health`에 request body가 있으면 `400 Bad Request`로 처리한다.

### 7.3 성공 응답

```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8

{
  "ok": true,
  "status": "ok",
  "worker_count": 4,
  "queue_depth": 0
}
```

### 7.4 응답 필드

- `ok`: 성공 여부
- `status`: 서버 상태 문자열. 최소 구현에서는 항상 `ok`
- `worker_count`: 현재 worker thread 개수
- `queue_depth`: 현재 작업 큐 대기 수

## 8. `GET /`

### 8.1 목적

- 브라우저에서 바로 접근 가능한 진입 페이지를 제공한다.
- 간단한 SQL 입력 UI를 통해 기존 `POST /query` API를 호출할 수 있게 한다.
- 발표와 데모에서 서버와 API 사용법을 빠르게 확인할 수 있게 한다.

### 8.2 요청

```http
GET / HTTP/1.1
Host: 127.0.0.1:8080
```

규칙:

- `GET /`는 request body를 허용하지 않는다.
- 응답 본문은 단일 HTML 페이지다.

### 8.3 성공 응답

```http
HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
```

응답 본문은 아래 요소를 포함한다.

- 서비스 이름과 지원 엔드포인트 안내
- SQL 입력 textarea
- 실행 버튼
- 결과 출력 영역
- 에러 코드와 메시지 표시 영역
- 브라우저 내부 help 명령 안내

### 8.4 동작 방식

- 페이지 내부 JavaScript는 사용자가 입력한 SQL을 `POST /query`로 전송한다.
- 따라서 `/`는 별도의 SQL 실행 계약을 만들지 않고, 기존 JSON API의 브라우저용 클라이언트 역할만 한다.
- 단, `help`, `.help`, `--help`는 브라우저 내부 편의 명령으로 처리하며 `POST /query`로 전송하지 않는다.
- 위 help 명령은 API 서버의 SQL 계약을 확장하는 것이 아니라, 루트 HTML 페이지에 한정된 클라이언트 기능이다.

## 9. `POST /query`

### 9.1 목적

- 외부 클라이언트가 SQL 문장을 실행하는 기본 엔드포인트다.
- 8주차 최소 구현에서는 모든 DB 조작 요청을 이 엔드포인트로 처리한다.

### 9.2 요청 헤더

```http
POST /query HTTP/1.1
Host: 127.0.0.1:8080
Content-Type: application/json; charset=utf-8
Content-Length: 47
```

### 9.3 요청 본문

```json
{
  "sql": "SELECT id, name FROM users WHERE id = 1;"
}
```

### 9.4 요청 필드 규칙

- `sql` 필드는 필수다.
- `sql`은 문자열이어야 한다.
- 공백만 있는 SQL은 허용하지 않는다.
- SQL은 현재 엔진이 지원하는 단일 문장이어야 한다.
- request body 전체 크기는 `16 KiB`를 넘을 수 없다.
- `sql` 문자열 길이는 UTF-8 바이트 기준 `8 KiB`를 넘을 수 없다.
- request line과 header는 합산 `8 KiB`를 넘을 수 없다.
- `POST /query`는 `Content-Length`가 반드시 있어야 한다.
- `Content-Length`는 10진수 정수여야 하며 실제 수신 body 길이와 일치해야 한다.
- `Transfer-Encoding: chunked`는 지원하지 않는다.

## 10. `POST /query` 성공 응답

### 10.1 `SELECT` 성공 예시

```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8

{
  "ok": true,
  "statement_type": "select",
  "affected_rows": 1,
  "summary": "SELECT 1",
  "output": "+----+------+\n| id | name |\n+----+------+\n| 1  | Alice |\n+----+------+\n",
  "elapsed_ms": 0.42
}
```

### 10.2 `INSERT` 성공 예시

```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8

{
  "ok": true,
  "statement_type": "insert",
  "affected_rows": 1,
  "summary": "INSERT 1",
  "output": "",
  "elapsed_ms": 0.31
}
```

### 10.3 성공 응답 필드

- `ok`: 성공 여부. 성공 시 `true`
- `statement_type`: `select` 또는 `insert`
- `affected_rows`: 영향받은 행 수
- `summary`: 엔진 요약 메시지. 예: `SELECT 1`, `INSERT 1`
- `output`: executor가 생성한 출력 문자열
- `elapsed_ms`: 요청 처리 시간 밀리초 단위 값

### 10.4 `output` 필드 의미

- `SELECT`에서는 표 형식 문자열을 담는다.
- 최소 구현에서 `INSERT` 성공 시 `output`은 항상 빈 문자열 `""`다.
- 본 API는 최소 구현에서 row 배열 JSON을 직접 제공하지 않는다.
- `output` 내부 줄바꿈은 LF(`\n`) 기준으로 유지한다.
- HTTP 헤더 줄바꿈(`\r\n`)과 `output` 문자열 줄바꿈은 별개이며, JSON 역직렬화 후 브라우저 `pre` 출력에서는 LF 기준 줄바꿈이 그대로 보존되어야 한다.
- 결과 행 수가 많은 `SELECT`도 JSON 응답 본문 크기 안에서 정상 반환돼야 하며, 응답 조립 과정에서 고정 길이 버퍼 때문에 실패해서는 안 된다.

## 11. `POST /query` 오류 응답

### 11.1 공통 오류 형식

```http
HTTP/1.1 400 Bad Request
Content-Type: application/json; charset=utf-8

{
  "ok": false,
  "error": {
    "code": "SQL_PARSE_ERROR",
    "message": "expected identifier, got EOF at position 6"
  }
}
```

### 11.2 오류 응답 필드

- `ok`: 실패 시 `false`
- `error.code`: 기계 판독용 오류 코드
- `error.message`: 사람이 읽을 수 있는 상세 설명
- 브라우저 루트 페이지(`GET /`)는 이 값을 그대로 표시하되, 필요하면 `error.code`와 함께 추가 안내 문구를 덧붙일 수 있다.

### 11.3 대표 오류 사례

잘못된 JSON:

```http
HTTP/1.1 400 Bad Request
Content-Type: application/json; charset=utf-8

{
  "ok": false,
  "error": {
    "code": "INVALID_JSON",
    "message": "request body is not valid JSON"
  }
}
```

`sql` 필드 누락:

```http
HTTP/1.1 400 Bad Request
Content-Type: application/json; charset=utf-8

{
  "ok": false,
  "error": {
    "code": "MISSING_SQL_FIELD",
    "message": "field 'sql' is required"
  }
}
```

미지원 SQL:

```http
HTTP/1.1 400 Bad Request
Content-Type: application/json; charset=utf-8

{
  "ok": false,
  "error": {
    "code": "UNSUPPORTED_SQL",
    "message": "SQL must start with SELECT or INSERT. Check the first keyword for a typo."
  }
}
```

큐 포화:

```http
HTTP/1.1 503 Service Unavailable
Content-Type: application/json; charset=utf-8

{
  "ok": false,
  "error": {
    "code": "QUEUE_FULL",
    "message": "server is busy"
  }
}
```

body 초과:

```http
HTTP/1.1 413 Payload Too Large
Content-Type: application/json; charset=utf-8

{
  "ok": false,
  "error": {
    "code": "PAYLOAD_TOO_LARGE",
    "message": "request body exceeds 16384 bytes"
  }
}
```

chunked 요청:

```http
HTTP/1.1 501 Not Implemented
Content-Type: application/json; charset=utf-8

{
  "ok": false,
  "error": {
    "code": "CHUNKED_NOT_SUPPORTED",
    "message": "Transfer-Encoding: chunked is not supported"
  }
}
```

## 12. 메서드 및 경로 규칙

### 12.1 `GET /`

- 허용 메서드: `GET`
- 다른 메서드 사용 시 `405 Method Not Allowed`

### 12.2 `GET /health`

- 허용 메서드: `GET`
- 다른 메서드 사용 시 `405 Method Not Allowed`

### 12.3 `POST /query`

- 허용 메서드: `POST`
- 다른 메서드 사용 시 `405 Method Not Allowed`

### 12.4 그 외 경로

- `404 Not Found`

## 13. 동시성 및 정합성 보장

본 API는 아래 보장을 제공한다.

- 서로 다른 테이블에 대한 요청은 병렬 처리 가능하다.
- 같은 테이블에 대한 요청은 직렬화된다.
- 같은 물리 테이블을 가리키는 alias 이름과 storage 이름은 같은 lock key로 직렬화된다.
- 같은 테이블에 대한 동시 `INSERT`에서 내부 `id` 생성과 인덱스 반영은 경쟁 조건 없이 처리되어야 한다.

본 API는 아래 보장을 제공하지 않는다.

- 트랜잭션
- 복수 요청 간 원자적 묶음
- 세션 격리 수준
- read/write lock 분리

## 14. 요청 제한

- 요청 본문은 JSON 객체 하나만 허용한다.
- request body 최대 크기는 `16 KiB`다.
- `sql` 문자열 최대 길이는 UTF-8 바이트 기준 `8 KiB`다.
- request line과 header는 합산 `8 KiB`를 넘을 수 없다.
- `POST /query`는 `Content-Length` 필수다.
- `Content-Length` 값이 잘못됐거나 body 길이와 맞지 않으면 요청은 실패한다.
- streaming body는 지원하지 않는다.
- `Transfer-Encoding: chunked`는 지원하지 않는다.

header 크기 계산 규칙:

- request line 한 줄 전체를 포함한다.
- request line 뒤의 `\r\n`을 포함한다.
- 각 header line 전체를 포함한다.
- 각 header line 뒤의 `\r\n`을 포함한다.
- header block 종료용 마지막 빈 줄의 `\r\n`을 포함한다.
- 본문 바이트는 포함하지 않는다.

추가 규칙:

- obsolete line folding은 지원하지 않는다.
- 줄바꿈으로 이어지는 folded header는 잘못된 요청으로 처리한다.

## 15. 서버 시작 실패 정책

- 잘못된 `--port`, `--worker-count`, `--queue-capacity`는 시작 실패다.
- 존재하지 않는 `--schema-dir`, `--data-dir`는 시작 실패다.
- 시작 실패 시 서버는 표준 오류 출력에 메시지를 남기고 `exit code 1`로 종료한다.

## 16. 테스트 예시

### 16.1 health check

```bash
curl -i http://127.0.0.1:8080/health
```

### 16.2 select query

```bash
curl -i -X POST http://127.0.0.1:8080/query ^
  -H "Content-Type: application/json" ^
  -d "{\"sql\":\"SELECT id, name FROM users WHERE id = 1;\"}"
```

### 16.3 insert query

```bash
curl -i -X POST http://127.0.0.1:8080/query ^
  -H "Content-Type: application/json" ^
  -d "{\"sql\":\"INSERT INTO users (name, age) VALUES ('Alice', 20);\"}"
```

## 16. 향후 확장 여지

이번 명세는 최소 구현 기준이다.

후속 확장 가능 항목:

- 구조화된 row 배열 응답
- 쿼리 timeout
- 배치 실행 API
- 메트릭스 API
- 인증/인가
