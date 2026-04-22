# 정글 미니 DBMS API 서버

## 1. 프로젝트 개요

이 저장소는 8주차 수요 코딩회 과제인 `미니 DBMS - API 서버` 구현 결과물입니다.

7주차에서 만든 SQL 처리기와 B+ 트리를 그대로 두고, 그 앞에 HTTP API 서버, 스레드 풀, 작업 큐, 테이블 단위 락을 추가해 외부 클라이언트가 병렬로 DB 기능을 사용할 수 있게 만들었습니다.

이번 구현은 8주차 과제 범위에 맞춘 최소 구현입니다.

시스템에 대한 세부 설계는 아래 문서를 기준으로 작성했습니다.

- [요구사항 정의서](docs/week8-requirements.md)
- [아키텍처 문서](docs/week8-architecture.md)
- [API 명세서](docs/week8-api-spec.md)
- [테스트 계획서](docs/week8-test-plan.md)

문서 우선순위: `architecture.md > api-spec.md > requirements.md > README.md`

## 2. 핵심 포인트

- 7주차 SQL 처리기와 B+ 트리를 버리지 않고, API 서버 바깥 계층만 추가해 재사용했습니다.
- `accept loop + bounded queue + worker thread pool` 구조로 요청을 병렬 처리합니다.
- 같은 물리 테이블은 `storage_name` 기준 락으로 직렬화하고, 다른 테이블은 병렬 처리합니다.
- `GET /`, `GET /health`, `POST /query` 세 엔드포인트로 브라우저와 외부 클라이언트 사용을 지원합니다.
- 자동 테스트로 HTTP 오류, SQL 오류, queue full, 동시성, 재시작 복구까지 검증했습니다.

## 3. 요구사항 대응 요약

| 공지 요구사항 | 구현/설계 대응 | 근거 |
| --- | --- | --- |
| 구현한 API를 통해 외부 클라이언트에서 DBMS 기능을 사용할 수 있어야 합니다. | `GET /`, `GET /health`, `POST /query`를 제공하고, 브라우저 진입 페이지도 함께 구성했습니다. | `docs/week8-api-spec.md`, `src/api/`, `src/http/` |
| 스레드 풀(Thread Pool)을 구성하고, 요청이 들어올 때마다 스레드를 할당하여 SQL 요청을 병렬로 처리해야 합니다. | `accept loop + bounded queue + worker thread pool` 구조로 요청을 병렬 처리합니다. | `docs/week8-architecture.md`, `src/server/server.c`, `src/server/task_queue.c`, `src/server/worker_pool.c` |
| 이전 차수 SQL 처리기 계층을 재사용하여 내부 DB 엔진을 구성합니다. | 기존 `lexer`, `parser`, `executor`, `storage`, `index` 계층을 그대로 재사용했습니다. | `docs/week8-requirements.md`, `docs/week8-architecture.md`, `src/sql/`, `src/execution/`, `src/storage/`, `src/index/`, `src/engine/sql_engine_adapter.c` |
| 이전 차수 B+ 트리 인덱스를 재사용하여 조회 경로를 유지합니다. | `WHERE id = ...` 조회는 기존 B+ 트리 인덱스 경로를 그대로 사용합니다. | `docs/week8-requirements.md`, `docs/week8-architecture.md`, `src/index/table_index.c`, `src/execution/executor.c` |
| 멀티 스레드 동시성 이슈를 고려해야 합니다. | 같은 테이블은 직렬화하고, 다른 테이블은 병렬 처리하도록 락 전략을 적용했습니다. | `docs/week8-architecture.md`, `src/engine/engine_lock_manager.c`, `src/index/table_index.c` |
| API 서버 아키텍쳐와 내부 DB 엔진과 외부 API 서버 사이 연결을 설계해야 합니다. | `client -> server -> http -> api -> service -> engine adapter -> sql/execution/storage/index` 계층 구조를 유지했습니다. | `docs/week8-architecture.md`, `src/api/`, `src/service/`, `src/engine/` |
| 단위 테스트, API 기능 테스트, 엣지 케이스를 최대한 고려해야 합니다. | API 통합 테스트와 엔진 회귀 테스트로 정상/비정상/동시성 경로를 검증했습니다. | `docs/week8-test-plan.md`, `tests/test_runner.c`, `tests/test_api_server.c` |
| 발표자료는 따로 만들지 않고, README.md를 기준으로 설명합니다. | README에 구조, 사용 방법, 검증 결과, 한계를 모아 발표 문서 역할을 하도록 구성했습니다. | `README.md`, `docs/images/` |

## 4. 시스템 구조

### 4.1 계층 구조

```text
client
  -> server
  -> http
  -> api
  -> service
  -> engine adapter
  -> sql / execution
  -> storage / index
```

### 4.2 계층별 역할

- `client`: 브라우저, curl, 테스트 코드처럼 외부에서 HTTP 요청을 보내는 호출자입니다.
- `server`: 소켓을 열고 연결을 받아 queue와 worker로 분배하는 런타임 계층입니다.
- `http`: request line, header, body를 파싱하고 HTTP 응답을 만드는 계층입니다.
- `api`: `GET /`, `GET /health`, `POST /query`를 처리하고 서비스 계층으로 넘깁니다.
- `service`: 현재는 얇은 위임 계층이지만, API 핸들러가 엔진 세부사항에 직접 달라붙지 않게 하는 완충 지점입니다.
- `engine adapter`: 기존 SQL 엔진과 연결하면서 락, 실행, 출력 캡처, 오류 변환을 담당합니다.
- `sql/execution/storage/index`: 7주차에서 구현한 SQL 엔진과 B+ 트리 인덱스 재사용 영역입니다.

### 4.3 핵심 설계 선택

- 서버 실행 모델은 `1 connection = 1 request`입니다.
- `GET /`는 브라우저용 HTML 진입 페이지를 반환합니다.
- `POST /query`는 `Content-Length` 기반 고정 길이 body만 지원합니다.
- keep-alive, pipelining, `Transfer-Encoding: chunked`는 지원하지 않습니다.
- 조회 결과는 구조화된 row 배열 대신 기존 엔진의 표 출력 문자열을 그대로 JSON `output`에 담습니다.

## 5. 데이터 흐름

### 5.1 정상 요청 흐름

![정상 요청 흐름 다이어그램](docs/images/api-server-request-flow.png)

요청 1건은 `accept -> task queue -> worker -> http -> api -> service -> engine adapter -> executor` 순서로 처리됩니다.

`POST /query` 기준으로 보면:

1. accept thread가 연결을 받습니다.
2. worker thread가 HTTP 요청을 파싱합니다.
3. `query_handler`가 JSON에서 `sql` 문자열을 꺼냅니다.
4. `db_service`가 엔진 어댑터로 실행을 위임합니다.
5. 어댑터가 schema 로딩, 락 획득, SQL 실행, 출력 캡처를 수행합니다.
6. 결과를 JSON으로 바꿔 응답합니다.

HTTP 형식 오류나 queue full 같은 예외는 가능한 한 앞단에서 바로 응답합니다.

### 5.2 동시성 제어가 걸리는 지점

![런타임 동시성 구조 다이어그램](docs/images/api-server-runtime-flow.png)

동시성 제어는 `engine adapter`에서 `load_schema()` 이후 확정되는 `schema.storage_name` 기준으로 걸립니다.
같은 물리 테이블 요청은 같은 락으로 직렬화되고, 다른 테이블 요청은 병렬 처리됩니다.

## 6. 병렬 처리와 락 전략

### 6.1 서버 실행 모델

- `1 connection = 1 request`
- 요청 하나를 처리한 뒤 응답을 보내고 연결 종료
- `POST /query`는 `Content-Length` 기반 body만 허용
- `GET /health`는 body를 허용하지 않음

### 6.2 병렬 처리 구조

- accept thread가 새 연결을 받아 bounded queue에 적재합니다.
- worker thread가 큐에서 연결을 꺼내 HTTP 요청 1개를 처리합니다.
- 큐가 가득 차면 accept thread가 즉시 `503 Service Unavailable`을 반환합니다.
* 검증 테스트: tests/test_api_server.c의 run_api_server_queue_full_test()

![Accept loop, bounded queue, worker thread pool 구조](docs/images/accept-queue-worker-flow.png)

### 6.3 정합성 보호

- 같은 테이블 요청은 `table-level exclusive lock`으로 직렬화합니다.
- 서로 다른 테이블 요청은 병렬 처리 가능합니다.
- 락 키는 SQL 원문 이름이 아니라 `schema.storage_name`입니다.
- 전역 `TableIndexRegistry`는 별도의 `registry_mutex`로 보호합니다.

### 6.4 발표 시연에서 보여줄 수 있는 점

- 같은 테이블의 긴 요청이 진행 중이면 뒤에 온 같은 테이블 요청은 기다립니다.
- 다른 테이블 요청은 같은 시점에도 응답할 수 있습니다.
- 즉 worker thread는 병렬로 돌지만, 같은 물리 테이블만 락으로 직렬화됩니다.

## 7. 기존 엔진 재사용 방식

이번 구현은 CLI 진입점을 재사용하지 않고, `sql_engine_adapter`를 통해 기존 엔진과 연결합니다.

어댑터는 아래 순서로 동작합니다.

1. SQL 문자열이 비어 있지 않고 길이 제한을 넘지 않는지 확인합니다.
2. `lexer`와 `parser`를 호출해 SQL을 AST로 해석합니다.
3. AST에서 대상 테이블 이름을 추출합니다.
4. `load_schema()`를 호출해 `schema.storage_name`을 확정합니다.
5. `schema.storage_name` 기준으로 락을 획득합니다.
6. 기존 `executor`를 호출해 SQL을 실행합니다.
7. `SELECT` 결과는 기존 표 출력 문자열을 캡처합니다.
8. 엔진 결과를 API 응답 구조로 바꿔 반환합니다.

이 방식으로 기존 엔진은 유지하고, API 서버 쪽에서는 네트워크, HTTP, 동시성 제어만 추가했습니다.

## 8. API 사용 방법

### 8.1 엔드포인트

- `GET /`: 브라우저용 SQL 입력 화면
- `GET /health`: 서버 상태 확인
- `POST /query`: SQL 실행

### 8.2 지원 SQL

- `INSERT INTO ... VALUES (...)`
- `SELECT * FROM ...`
- `SELECT column1, column2 FROM ...`
- `SELECT ... WHERE column = value`
- `SELECT ... WHERE id = <integer>`

비지원 SQL과 세부 제약은 [API 명세서](docs/week8-api-spec.md)를 기준으로 합니다.

### 8.3 응답 예시

`GET /health`

```json
{
  "ok": true,
  "status": "ok",
  "worker_count": 4,
  "queue_depth": 0
}
```

`POST /query` `SELECT` 성공

```json
{
  "ok": true,
  "statement_type": "select",
  "affected_rows": 1,
  "summary": "SELECT 1",
  "output": "+----+------+\n| id | name |\n+----+------+\n| 1  | Alice |\n+----+------+\n",
  "elapsed_ms": 0.42
}
```

`POST /query` `INSERT` 성공

```json
{
  "ok": true,
  "statement_type": "insert",
  "affected_rows": 1,
  "summary": "INSERT 1",
  "output": "",
  "elapsed_ms": 0.31
}
```

## 9. 대표 예외 처리

대표적인 예외 분기만 요약하면 아래와 같습니다.

| 입력/상황 | HTTP 상태 | `error.code` |
| --- | --- | --- |
| 잘못된 JSON | 400 | `INVALID_JSON` |
| `Content-Type` 누락 또는 비지원 | 400 | `INVALID_CONTENT_TYPE` |
| `Content-Length` 누락 | 411 | `CONTENT_LENGTH_REQUIRED` |
| 미지원 SQL | 400 | `UNSUPPORTED_SQL` |
| 잘못된 SQL 인자 | 400 | `INVALID_SQL_ARGUMENT` |
| 작업 큐 포화 | 503 | `QUEUE_FULL` |
| 내부 실행 오류 | 500 | `INTERNAL_ERROR` |

전체 오류 코드와 세부 매핑 규칙은 [API 명세서](docs/week8-api-spec.md)를 기준으로 합니다.

## 10. 빌드 및 실행

### 10.1 빌드

```bash
make all
```

생성 바이너리:

- `build/bin/sqlparser`
- `build/bin/sqlapi_server`
- `build/bin/test_runner`

### 10.2 기본 서버 실행

```bash
./build/bin/sqlapi_server \
  --host 127.0.0.1 \
  --port 8080 \
  --worker-count 4 \
  --queue-capacity 64 \
  --schema-dir schema \
  --data-dir data
```

시작 시 검증 규칙:

- `--port`는 `1..65535`
- `--worker-count >= 1`
- `--queue-capacity >= 1`
- `--schema-dir`, `--data-dir`는 존재하는 디렉터리여야 함

종료:

- 포그라운드 실행 중에는 `Ctrl+C`
- 백그라운드 프로세스를 정리할 때는 `pkill -f sqlapi_server`

### 10.3 빠른 확인

health check:

```bash
curl -i http://127.0.0.1:8080/health
```

select:

```bash
curl -i -X POST http://127.0.0.1:8080/query \
  -H "Content-Type: application/json" \
  -d '{"sql":"SELECT id, name FROM student WHERE id = 1;"}'
```

insert:

```bash
curl -i -X POST http://127.0.0.1:8080/query \
  -H "Content-Type: application/json" \
  -d '{"sql":"INSERT INTO student (department, student_number, name, age) VALUES (\"컴퓨터공학과\", \"2024999\", \"홍길동\", 20);"}'
```

브라우저에서 확인할 때는 `http://127.0.0.1:8080/`로 접속하면 됩니다.

## 11. 테스트와 검증

> 현재 자동 테스트 기준 `Tests run: 387`, `Tests failed: 0`이며, 정상/오류/경계값/동시성/복구성까지 검증했습니다.

### 11.1 테스트 실행

```bash
make test
```

현재 자동 테스트가 검증하는 세부 항목과 설계 의도는 [테스트 계획서](docs/week8-test-plan.md)를 기준으로 합니다.

## 12. 저장소 구조

```text
docs/                     8주차 요구사항/설계/명세/테스트 문서
include/sqlparser/        공개 헤더
src/                      엔진, 서버, HTTP, API, 서비스 구현
tests/                    엔진 및 API 테스트
schema/                   기본 메타 스키마
data/                     기본 CSV 데이터
week7-reference-docs/     7주차 참고 문서
```

핵심 파일:

- `src/app/sqlapi_server_main.c`: API 서버 전용 실행 파일 진입점
- `src/server/server.c`: listen socket, accept loop, queue, worker pool을 조립하는 런타임 중심 모듈
- `src/engine/sql_engine_adapter.c`: 기존 SQL 엔진과 API 서버를 연결하는 어댑터
- `src/http/http_request.c`: HTTP 요청 파싱과 한도 검증
- `src/http/http_response.c`: JSON 응답 생성과 상태 코드 매핑
- `tests/test_api_server.c`: API 서버 통합 테스트

## 13. 한계와 후속 개선 포인트

현재 구현은 8주차 과제 범위에 맞춘 최소 구현입니다.

- HTTP/1.1만 지원합니다.
- 엔드포인트는 `GET /`, `GET /health`, `POST /query` 중심의 최소 범위만 제공합니다.
- 조회 결과는 구조화된 row 배열이 아니라 문자열 표 출력입니다.
- 같은 테이블 요청은 모두 직렬화되므로 경쟁이 높은 상황에서는 병목이 생길 수 있습니다.
- 병렬 처리 자체는 worker pool로 제공하지만, 스케줄링은 테이블 인지형이 아니어서 같은 테이블 요청이 몰리면 여러 worker가 락 대기에 묶일 수 있습니다.
- 현재 구현은 학습용 최소 API 서버이므로 연결 재사용(keep-alive), 조각 전송(chunked transfer), 트랜잭션(transaction), 사용자 인증(authentication) 같은 실서비스 기능은 범위에서 제외했습니다.


후속 확장 후보:

- 구조화된 result set JSON
- read/write lock 분리
- query timeout
- request id / trace id
- richer metrics endpoint
