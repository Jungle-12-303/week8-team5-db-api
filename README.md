# 정글 미니 DBMS API 서버

## 1. 프로젝트 개요

이 저장소는 8주차 수요코딩회 과제인 `미니 DBMS - API 서버` 구현 결과물입니다.

핵심 목표는 다음과 같습니다.

- 7주차에 구현한 SQL 처리기와 B+ 트리 인덱스를 내부 DB 엔진으로 재사용한다.
- 외부 클라이언트가 네트워크 API를 통해 DBMS 기능을 사용할 수 있게 한다.
- 스레드 풀 기반으로 동시에 들어오는 SQL 요청을 병렬 처리한다.
- 병렬 처리 상황에서도 같은 테이블에 대한 정합성을 보장한다.
- README만으로 구조, 실행 방법, 테스트 방법, 검증 결과를 설명할 수 있게 한다.

세부 설계와 계약은 아래 문서를 기준으로 구현했습니다.

- [요구사항 정의서](docs/week8-requirements.md)
- [아키텍처 문서](docs/week8-architecture.md)
- [API 명세서](docs/week8-api-spec.md)
- [테스트 계획서](docs/week8-test-plan.md)

문서 우선순위는 `architecture.md > api-spec.md > requirements.md > README.md`입니다.

## 2. 과제 요구사항 대응

- `기존 SQL 처리기 재사용`: 기존 lexer, parser, executor, storage, index 계층을 그대로 내부 엔진으로 사용합니다.
- `B+ 트리 인덱스 재사용`: `WHERE id = ...` 조회와 lazy rebuild 경로를 기존 인덱스 레이어 위에서 유지합니다.
- `API 서버 제공`: `HTTP/1.1 over TCP` 기반으로 `GET /health`, `POST /query`를 제공합니다.
- `스레드 풀`: `accept loop + bounded task queue + worker thread pool` 구조를 사용합니다.
- `병렬 SQL 처리`: 서로 다른 테이블 요청은 병렬 처리하고, 같은 테이블 요청은 직렬화합니다.
- `테스트`: 기존 엔진 회귀 테스트에 더해 API 서버 스모크 테스트를 통합했습니다.
- `엣지 케이스 고려`: 잘못된 헤더, 잘못된 JSON, body 크기 초과, `chunked` 미지원, queue full 등을 명시적으로 처리합니다.

## 3. 전체 구조

### 계층 구조

```text
client
  -> http server
  -> router / handler
  -> service
  -> engine adapter
  -> sql
  -> execution
  -> storage / index
```

### 계층별 역할

- `server`: 소켓 생성, `bind/listen/accept`, 작업 큐 적재, worker pool 제어
- `http`: 요청 라인/헤더/body 파싱, 응답 생성, 라우팅
- `api`: 엔드포인트별 요청 검증과 응답 변환
- `service`: API 입력을 내부 실행 요청으로 변환
- `engine adapter`: SQL 파싱, 대상 테이블 식별, 락 획득/해제, 엔진 호출, 출력 캡처
- `sql/execution/storage/index`: 7주차 SQL 엔진과 B+ 트리 인덱스 로직

### 기존 엔진 재사용 방식

이번 주차 구현은 CLI 진입점을 재사용하지 않고, 별도의 `sql_engine_adapter`를 통해 기존 엔진과 연결합니다.

어댑터는 다음 흐름으로 동작합니다.

1. SQL 문자열 검증
2. lexer / parser 호출
3. AST에서 raw table name 추출
4. `load_schema()`로 스키마를 로드해 `schema.storage_name` 확보
5. `schema.storage_name` 기준 테이블 락 획득
6. 기존 executor 호출
7. executor 출력 문자열 캡처
8. JSON 응답용 결과 구조체 생성
9. 락 해제

이 방식으로 기존 엔진 책임은 유지하고, API 서버만 바깥 계층으로 덧씌우는 형태를 선택했습니다.

## 4. 동시성 처리 전략

### 서버 실행 모델

- `1 connection = 1 request`
- keep-alive 미지원
- 요청 처리 후 응답을 쓰고 연결 종료
- `POST /query`는 `Content-Length` 기반 고정 길이 body만 지원

### 병렬 처리 구조

- accept thread가 연결을 받아 bounded queue에 적재합니다.
- worker thread가 큐에서 연결을 꺼내 요청 1개를 처리합니다.
- 큐가 가득 차면 worker를 기다리지 않고 즉시 `503 Service Unavailable`을 반환합니다.

### 정합성 보호

- 같은 테이블 요청은 `table-level exclusive lock`으로 직렬화합니다.
- 서로 다른 테이블 요청은 병렬 처리 가능합니다.
- 락 키는 SQL 원문 이름이 아니라 `schema.storage_name`입니다.
- 전역 `TableIndexRegistry`는 별도의 `registry_mutex`로 보호합니다.

왜 이렇게 했는지:

- 현재 엔진은 `INSERT`, lazy rebuild, `next_id`, 인덱스 등록이 모두 공유 상태를 만집니다.
- 따라서 같은 테이블에 대해 read/write를 세분화하기보다 우선 exclusive lock 하나로 직렬화하는 것이 가장 안전합니다.
- 대신 테이블 단위로 락 범위를 제한해 다른 테이블 간 병렬성은 유지했습니다.

## 5. 지원 기능 범위

### 지원 엔드포인트

- `GET /health`
- `POST /query`

### 지원 SQL

- `INSERT INTO ... VALUES (...)`
- `SELECT * FROM ...`
- `SELECT column1, column2 FROM ...`
- `SELECT ... WHERE column = value`
- `SELECT ... WHERE id = <integer>`

### 비지원 SQL

- `UPDATE`
- `DELETE`
- `JOIN`
- `ORDER BY`
- `GROUP BY`
- `AND`, `OR`를 포함한 복합 조건
- multi-statement 실행

### 요청 제한

- request line + header 최대 `8 KiB`
- request body 최대 `16 KiB`
- SQL 문자열 최대 `8 KiB`
- `Transfer-Encoding: chunked` 미지원
- `POST /query`는 `Content-Length` 필수

## 6. API 사용 방법

### `GET /health`

요청:

```http
GET /health HTTP/1.1
Host: 127.0.0.1:8080
```

성공 응답 예시:

```json
{
  "ok": true,
  "status": "ok",
  "worker_count": 4,
  "queue_depth": 0
}
```

### `POST /query`

요청:

```http
POST /query HTTP/1.1
Host: 127.0.0.1:8080
Content-Type: application/json; charset=utf-8
Content-Length: 47

{"sql":"SELECT name FROM student WHERE id = 1;"}
```

`SELECT` 성공 응답 예시:

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

`INSERT` 성공 응답 예시:

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

오류 응답 예시:

```json
{
  "ok": false,
  "error": {
    "code": "UNSUPPORTED_SQL",
    "message": "only INSERT and SELECT are supported"
  }
}
```

상세 계약은 [API 명세서](docs/week8-api-spec.md)를 기준으로 합니다.

## 7. 빌드 및 실행

### 빌드

```bash
make all
```

생성 바이너리:

- `build/bin/sqlparser`
- `build/bin/sqlapi_server`
- `build/bin/test_runner`

### CLI 엔진 실행

```bash
./build/bin/sqlparser -e "SELECT * FROM student;"
```

### API 서버 실행

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

위 조건을 만족하지 않으면 사람이 읽을 수 있는 오류 메시지를 출력하고 `exit code 1`로 종료합니다.

### 빠른 확인 예시

health check:

```bash
curl -i http://127.0.0.1:8080/health
```

select:

```bash
curl -i -X POST http://127.0.0.1:8080/query ^
  -H "Content-Type: application/json" ^
  -d "{\"sql\":\"SELECT id, name FROM student WHERE id = 1;\"}"
```

insert:

```bash
curl -i -X POST http://127.0.0.1:8080/query ^
  -H "Content-Type: application/json" ^
  -d "{\"sql\":\"INSERT INTO student (department, student_number, name, age) VALUES ('컴퓨터공학과', '2024999', '홍길동', 20);\"}"
```

## 8. 테스트 전략과 검증 결과

### 테스트 구성

- 기존 7주차 SQL 엔진 회귀 테스트 유지
- API 서버 스모크 테스트 추가
- 인덱스 rebuild / recovery 관련 테스트 유지
- CLI 동작 검증 유지

현재 테스트 러너는 최소한 아래를 확인합니다.

- SQL 엔진 기본 회귀 동작
- 인덱스 rebuild 및 recovery 경로
- `GET /health`
- 정상 `POST /query`
- `Content-Length` 없는 `POST /query`

### 테스트 실행

```bash
make test
```

### 최근 검증 결과

현재 작업 트리 기준으로 `make test`를 실행했을 때:

- `Tests run: 376`
- `Tests failed: 0`

테스트 설계와 추가 검증 시나리오는 [테스트 계획서](docs/week8-test-plan.md)에 정리했습니다.

## 9. 저장소 구조

```text
docs/                     8주차 요구사항/설계/명세/테스트 문서
include/sqlparser/        공개 헤더
src/                      엔진, 서버, HTTP, API, 서비스 구현
tests/                    엔진 및 API 테스트
schema/                   기본 메타 스키마
data/                     기본 CSV 데이터
week7-reference-docs/     7주차 참고 문서
```

## 10. 한계와 후속 개선 포인트

현재 구현은 8주차 과제 범위에 맞춘 최소 구현입니다.

- HTTP/1.1만 지원합니다.
- 엔드포인트는 `GET /health`, `POST /query` 두 개뿐입니다.
- 응답의 조회 결과는 구조화된 row 배열이 아니라 문자열 표 출력입니다.
- 같은 테이블 요청은 모두 직렬화되므로 경쟁이 높은 상황에서는 병목이 생길 수 있습니다.
- keep-alive, chunked transfer, transaction, authentication은 지원하지 않습니다.

후속 확장 후보:

- 구조화된 result set JSON
- read/write lock 분리
- query timeout
- request id / trace id
- richer metrics endpoint

## 11. 발표 시 설명 포인트

이번 구현을 설명할 때 핵심은 다음 세 가지입니다.

- 7주차 SQL 처리기와 B+ 트리를 버리지 않고, API 서버 바깥 계층만 추가해 재사용했다는 점
- 스레드 풀로 병렬성을 확보하되, 같은 테이블은 `storage_name` 기준 락으로 정합성을 지켰다는 점
- README와 테스트 결과를 통해 기능, 오류 처리, 동시성 전략을 재현 가능하게 검증했다는 점
