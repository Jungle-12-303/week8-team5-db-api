# 8주차 README 상세 설명

## 1. 문서 목적

이 문서는 발표용으로 축약한 `README.md` 에서 덜어낸 세부 설명을 모아 둔 보조 문서다.

목표는 다음과 같다.

- README에는 핵심만 남긴다.
- 구조, 흐름, 범위, 실행, 테스트의 세부 설명은 이 문서로 넘긴다.
- 발표 준비와 코드 리뷰 준비에 모두 참고할 수 있게 한다.

## 2. 이번 주차를 한 문장으로 요약하면

이번 8주차는 기존 7주차 SQL 처리기와 B+ 트리 인덱스를 유지한 채, 그 바깥에 HTTP API 서버와 병렬 요청 처리 구조를 추가한 주차다.

즉 새 DB 엔진을 다시 만든 것이 아니라,
기존 엔진 위에 외부 API, 스레드 풀, 작업 큐, 테이블 단위 락을 덧씌운 구현이라고 보면 된다.

## 3. 구현한 것과 구현하지 않은 것

### 3.1 구현한 것

- `GET /`, `GET /health`, `POST /query`
- 브라우저용 SQL 입력 화면
- `1 connection = 1 request` 기반 최소 HTTP 서버
- `Content-Length` 기반 JSON body 처리
- `accept loop + bounded queue + worker thread pool`
- queue full 시 `503 Service Unavailable`
- 같은 물리 테이블 직렬화
- 다른 물리 테이블 병렬 처리
- 기존 SQL 처리기와 B+ 트리 인덱스 재사용
- `WHERE id = ...` 조회 시 기존 인덱스 경로 유지
- 자동 테스트 기반 정상/오류/동시성 검증

### 3.2 구현하지 않은 것

이번 구현은 과제 범위에 맞춘 최소 구현이다. 아래 항목은 의도적으로 범위에서 제외했다.

- HTTP keep-alive  
  현재 서버는 `1 connection = 1 request` 모델이다. 요청 하나를 처리한 뒤 응답을 보내고 연결을 닫기 때문에, 같은 소켓을 재사용하는 keep-alive 는 지원하지 않는다.

- HTTP pipelining  
  하나의 TCP 연결에서 이전 응답을 기다리지 않고 여러 HTTP 요청을 연속으로 보내는 방식이다. 현재 구조는 한 연결에서 요청 1건만 처리하므로 이 기능을 구현하지 않았다.

- `Transfer-Encoding: chunked`  
  body 길이를 미리 `Content-Length` 로 주지 않고 여러 조각으로 나눠 보내는 HTTP 전송 방식이다. 현재 구현은 `Content-Length` 기반 고정 길이 body만 읽기 때문에 `chunked` 요청은 거부한다.

- transaction  
  여러 SQL 실행을 하나의 원자적 작업 단위로 묶고, 실패 시 롤백하는 기능이다. 이번 과제는 단일 SQL 요청 처리와 병렬 구조 구현이 핵심이므로 transaction 은 범위에서 제외했다.

- authentication  
  사용자 신원 확인과 권한 검사 기능이다. 이번 구현은 학습용 최소 API 서버이므로 로그인, 토큰, 권한 분리 같은 기능은 넣지 않았다.

- 구조화된 result set JSON  
  현재 `SELECT` 결과는 row 배열 형태의 구조화된 JSON이 아니라, 기존 엔진이 만든 표 문자열을 그대로 `output` 필드에 담아 반환한다. 이는 기존 엔진 재사용을 단순하게 유지하기 위한 선택이다.

- `UPDATE`, `DELETE`, `JOIN`, `ORDER BY`, `GROUP BY`  
  현재 parser와 executor 는 `INSERT` 와 `SELECT` 중심 최소 범위만 지원한다. 위 기능들은 문법 확장과 실행기 확장이 함께 필요한 항목이므로 이번 주차 범위 밖으로 두었다.

- `AND`, `OR` 를 포함한 복합 `WHERE`  
  현재 parser 는 `WHERE column = value` 형태의 단일 조건만 허용한다. 복합 조건을 지원하려면 parser AST 구조와 executor 조건 평가 로직을 함께 확장해야 한다.

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

- `client`: 브라우저, curl, 테스트 코드처럼 외부에서 HTTP 요청을 보내는 쪽이다.
- `server`: 소켓, accept loop, 작업 큐, worker pool을 관리한다.
- `http`: request line, header, body를 파싱하고 HTTP 응답을 만든다.
- `api`: 엔드포인트별 요청 검증과 응답 변환을 담당한다.
- `service`: API와 엔진 사이를 잇는 얇은 완충 계층이다.
- `engine adapter`: 기존 SQL 엔진 연결, 락 획득, 출력 캡처, 오류 변환을 담당한다.
- `sql / execution / storage / index`: 7주차에서 구현한 기존 엔진 재사용 영역이다.

## 5. 요청 1건 처리 흐름

`POST /query` 요청 1건은 대략 아래 순서로 흐른다.

1. accept thread가 TCP 연결을 받는다.
2. 연결이 bounded queue에 적재된다.
3. worker thread가 queue에서 연결 하나를 꺼낸다.
4. `http_request.c` 가 HTTP 요청을 파싱한다.
5. `router.c` 가 `POST /query` 를 `query_handler` 로 보낸다.
6. `query_handler.c` 가 JSON body에서 `sql` 필드를 추출한다.
7. `db_service.c` 가 엔진 어댑터로 실행을 위임한다.
8. `sql_engine_adapter.c` 가 schema 로딩, 락 획득, lexer/parser/executor 호출을 수행한다.
9. 실행 결과를 JSON 응답으로 바꿔 소켓에 전송한다.
10. 요청/응답 자원과 소켓을 정리한다.

## 6. 병렬 처리와 락 전략

### 6.1 병렬 처리 구조

- accept thread가 새 연결을 받는다.
- 새 연결은 bounded queue에 들어간다.
- worker thread가 큐에서 연결을 꺼내 요청 1건을 처리한다.
- 큐가 가득 차면 accept thread가 직접 `503` 을 반환한다.

이 구조의 핵심은 "요청을 즉시 worker에게 넘기는 것이 아니라, queue를 사이에 둬서 흡수한다"는 점이다.

### 6.2 같은 테이블과 다른 테이블

- 같은 물리 테이블 요청은 `schema.storage_name` 기준으로 같은 락을 잡는다.
- 다른 물리 테이블 요청은 서로 다른 락을 사용하므로 병렬 처리 가능하다.

즉 worker thread는 병렬로 돌더라도,
실제 SQL 실행 직전에는 테이블 단위 직렬화가 한 번 더 걸린다.

### 6.3 현재 구조의 한계

- 같은 테이블 요청이 몰리면 여러 worker가 락 대기에 묶일 수 있다.
- 현재 스케줄링은 테이블 인지형이 아니다.
- 읽기/쓰기 락 분리는 아직 하지 않았다.

## 7. 기존 엔진 재사용 방식

이번 구현은 기존 CLI 진입점을 그대로 재사용한 것이 아니라,
`sql_engine_adapter` 를 통해 기존 엔진을 HTTP/API 환경에 맞게 감쌌다.

어댑터의 흐름은 아래와 같다.

1. SQL 문자열이 비어 있지 않고 길이 제한을 넘지 않는지 검사한다.
2. `lexer` 와 `parser` 를 호출해 AST를 만든다.
3. AST에서 대상 테이블 이름을 추출한다.
4. `load_schema()` 를 호출해 `schema.storage_name` 을 확정한다.
5. `schema.storage_name` 기준으로 테이블 락을 획득한다.
6. 기존 `executor` 를 호출해 SQL을 실행한다.
7. `SELECT` 결과는 기존 표 출력 문자열을 캡처한다.
8. 엔진 결과를 API 응답 구조로 변환한다.

이 방식 덕분에 7주차 엔진은 유지하고,
8주차에서는 네트워크, HTTP, 병렬 처리 구조만 추가할 수 있었다.

## 8. API 요약

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

### 8.3 대표적인 비지원 범위

- `UPDATE`
- `DELETE`
- `JOIN`
- `ORDER BY`
- `GROUP BY`
- `AND`, `OR`
- multi-statement

세부 요청/응답 형식과 오류 코드는 [API 명세서](../docs/week8-api-spec.md)를 기준으로 한다.

## 9. 빌드, 실행, 테스트

### 9.1 빌드

```bash
make all
```

생성 바이너리:

- `build/bin/sqlparser`
- `build/bin/sqlapi_server`
- `build/bin/test_runner`

### 9.2 서버 실행

```bash
./build/bin/sqlapi_server \
  --host 127.0.0.1 \
  --port 8080 \
  --worker-count 4 \
  --queue-capacity 64 \
  --schema-dir schema \
  --data-dir data
```

시작 시 주요 검증 규칙:

- `--port` 는 `1..65535`
- `--worker-count >= 1`
- `--queue-capacity >= 1`
- `--schema-dir`, `--data-dir` 는 존재하는 디렉터리여야 한다

### 9.3 빠른 확인

```bash
curl -i http://127.0.0.1:8080/health
```

```bash
curl -i -X POST http://127.0.0.1:8080/query \
  -H "Content-Type: application/json" \
  -d '{"sql":"SELECT id, name FROM student WHERE id = 1;"}'
```

### 9.4 테스트

```bash
make test
```

현재 자동 테스트 요약:

- `Tests run: 387`
- `Tests failed: 0`

세부 테스트 범위는 [테스트 계획서](../docs/week8-test-plan.md)를 참고한다.

## 10. 핵심 파일 안내

- `src/app/sqlapi_server_main.c`: API 서버 실행 파일 진입점
- `src/server/server.c`: listen socket, accept loop, queue, worker pool을 조립하는 중심 런타임
- `src/server/task_queue.c`: bounded queue 구현
- `src/server/worker_pool.c`: worker thread pool 생명주기 관리
- `src/http/http_request.c`: HTTP 요청 파싱과 한도 검증
- `src/http/http_response.c`: 상태 코드 매핑과 JSON 응답 생성
- `src/api/query_handler.c`: `POST /query` 처리
- `src/service/db_service.c`: API와 엔진 사이의 얇은 서비스 계층
- `src/engine/sql_engine_adapter.c`: 기존 SQL 엔진 연결부
- `src/engine/engine_lock_manager.c`: 테이블 단위 직렬화 락 구현
- `tests/test_api_server.c`: API 서버 통합 테스트

## 11. 함께 보면 좋은 문서

- [8주차 API 서버 코드 리뷰 가이드](week8-api-server-review-guide.md)
- [8주차 코드 읽기 체크리스트](week8-code-reading-checklist.md)
- [요구사항 정의서](../docs/week8-requirements.md)
- [아키텍처 문서](../docs/week8-architecture.md)
- [API 명세서](../docs/week8-api-spec.md)
- [테스트 계획서](../docs/week8-test-plan.md)
