# SQL Processor 8주차 아키텍처 문서

## 1. 문서 목적

이 문서는 8주차 과제인 `미니 DBMS - API 서버`를 현재 7주차 SQL 처리기 위에 어떤 구조로 얹을지 아키텍처 수준에서 정의한다.

본 문서는 다음 문서를 기준으로 작성한다.

- 공식 공지: `8주차 수요 코딩회 (수요일) 공지`
- 8주차 요구사항 정의서: `docs/week8-requirements.md`
- 7주차 아키텍처 문서: `week7-reference-docs/architecture.md`

본 문서의 역할은 다음과 같다.

- API 서버와 기존 SQL 엔진의 연결 위치를 확정한다.
- 스레드 풀과 작업 큐 구조를 정의한다.
- 병렬 SQL 처리 시 동시성 보호 전략을 확정한다.
- 구현 중 계층 책임이 섞이지 않도록 모듈 경계를 정한다.

## 2. 핵심 설계 결정

이번 8주차 구조의 핵심 결정은 다음과 같다.

- API 프로토콜은 `HTTP/1.1 over TCP`로 선택한다.
- 외부 진입점은 `GET /`, `GET /health`, `POST /query`로 구성한다.
- 기존 7주차 SQL 처리기와 B+ 트리 인덱스를 내부 DB 엔진으로 재사용한다.
- API 서버는 기존 `sql -> execution -> storage/index` 계층 바깥에 추가한다.
- 서버는 `accept loop + task queue + worker thread pool` 구조를 사용한다.
- 서버 실행 모델은 `1 connection = 1 request`로 고정한다.
- SQL 요청은 worker thread에서 처리한다.
- 서버 바이너리 이름은 `sqlapi_server`로 정의한다.
- 서버는 `--host`, `--port`, `--worker-count`, `--queue-capacity`, `--schema-dir`, `--data-dir` 옵션을 지원한다.
- 같은 테이블에 대한 요청은 `table-level exclusive lock`으로 직렬화한다.
- 다른 테이블 요청은 병렬 처리 가능하게 한다.
- 전역 `TableIndexRegistry`는 별도의 `registry_mutex`로 보호한다.
- API 응답은 JSON으로 반환하되, `SELECT` 결과 표는 우선 문자열 형태로 캡처해 전달한다.
- 작업 큐가 가득 찼을 때는 accept thread가 즉시 `503 Service Unavailable` 응답을 반환하고 연결을 닫는다.
- 서버 종료는 기본적으로 graceful shutdown 정책을 사용한다.

## 3. 전체 구조

### 3.1 계층 구조

```text
browser / api client
  -> http server
  -> router / handler
  -> service
  -> engine adapter
  -> sql
  -> execution
  -> storage / index
```

### 3.2 모듈 의존 방향

```text
server -> http
http -> api
api -> service
service -> engine
engine -> sql
engine -> execution
execution -> storage
execution -> index
storage -> common
index -> common
```

### 3.3 설계 의도

- HTTP 요청 파싱 코드는 SQL 파서/실행기와 분리한다.
- API 계층은 라우팅과 요청/응답 형식만 담당한다.
- 서비스 계층은 API 요구를 내부 실행 요청으로 변환한다.
- 엔진 어댑터는 락 획득, SQL 파싱, SQL 실행, 출력 캡처, 오류 변환을 담당한다.
- 기존 storage/index 계층은 API 서버 세부사항을 알지 못해야 한다.

### 3.4 서버 실행 모델

이번 최소 구현의 서버 실행 모델은 다음과 같다.

- 클라이언트 연결 하나는 요청 하나만 전송한다고 가정한다.
- 서버는 요청 하나를 처리한 뒤 응답을 보내고 연결을 닫아도 된다.
- HTTP keep-alive, pipelining, connection reuse는 이번 범위에서 지원하지 않는다.
- `GET /`는 브라우저 진입용 HTML 페이지를 반환한다.
- `POST /query`는 `Content-Length` 기반 고정 길이 body만 지원한다.
- `Transfer-Encoding: chunked`는 지원하지 않는다.
- `GET /health`는 request body를 허용하지 않으며, body가 있으면 잘못된 요청으로 처리한다.
- `GET /health`에서 `Content-Length: 0`은 허용한다.
- `GET /health`에서 body가 없으면 `Content-Type` 헤더는 있어도 무시한다.

이 선택의 이유는 다음과 같다.

- worker와 작업 큐의 책임을 단순하게 유지할 수 있다.
- 요청 단위 정리와 소켓 close 경로를 명확히 만들 수 있다.
- 멀티스레드 환경에서 소켓 생명주기 버그를 줄일 수 있다.

## 4. 권장 디렉터리 구조

아래 구조는 8주차 구현을 위한 권장안이다.

```text
src/
  app/
    main.c
  server/
    server_main.c
    server.c
    worker_pool.c
    task_queue.c
  http/
    http_request.c
    http_response.c
    router.c
  api/
    root_handler.c
    health_handler.c
    query_handler.c
  service/
    db_service.c
  engine/
    sql_engine_adapter.c
    engine_lock_manager.c
  execution/
  sql/
  storage/
  index/
```

위 구조의 목적은 기존 7주차 엔진을 직접 뜯어고치기보다, 바깥에 API 서버 계층과 어댑터 계층을 추가하는 것이다.

### 4.1 서버 실행 파일과 옵션

권장 서버 실행 파일은 다음과 같다.

```text
build/bin/sqlapi_server
```

권장 실행 예시는 다음과 같다.

```bash
./build/bin/sqlapi_server \
  --host 127.0.0.1 \
  --port 8080 \
  --worker-count 4 \
  --queue-capacity 64 \
  --schema-dir schema \
  --data-dir data
```

옵션 의미:

- `--host`: listen 주소. 기본값 `127.0.0.1`
- `--port`: listen 포트. 기본값 `8080`
- `--worker-count`: worker thread 수. 기본값 `4`
- `--queue-capacity`: bounded queue 최대 작업 수. 기본값 `64`
- `--schema-dir`: schema meta 디렉터리. 기본값 `schema`
- `--data-dir`: CSV 데이터 디렉터리. 기본값 `data`

경로 규칙:

- 상대 경로는 서버 프로세스의 현재 working directory 기준으로 해석한다.
- 저장소 루트에서 서버를 실행하는 것을 기본 전제로 한다.
- 테스트에서는 임시 디렉터리를 `--schema-dir`, `--data-dir`로 주입해 격리한다.

옵션 검증 규칙:

- `--port`는 `1` 이상 `65535` 이하여야 한다.
- `--worker-count`는 `1` 이상이어야 한다.
- `--queue-capacity`는 `1` 이상이어야 한다.
- `--schema-dir`는 존재하는 디렉터리여야 한다.
- `--data-dir`는 존재하는 디렉터리여야 한다.

시작 실패 정책:

- 위 조건을 만족하지 않으면 서버는 시작하지 않는다.
- 서버는 표준 오류 출력에 사람이 읽을 수 있는 오류 메시지를 남긴다.
- 프로세스는 `exit code 1`로 종료한다.

## 5. 계층 책임

### 5.1 server 계층

책임:

- 소켓 생성, bind, listen, accept
- 서버 시작과 종료 제어
- worker thread pool 시작과 정리
- 작업 큐에 연결 단위 작업 전달
- 큐 포화 시 `503` 응답 직접 반환

비책임:

- HTTP 본문 파싱
- SQL 문법 해석
- 테이블 락 세부 처리

### 5.2 http 계층

책임:

- HTTP request line, header, body 파싱
- `Content-Type`, `Content-Length`, header size, request body size 검증
- request line + header 전체 크기 계산
- JSON 요청 본문 추출
- HTTP status line, header, body 생성
- 라우팅

비책임:

- SQL 실행
- 인덱스 접근
- 비즈니스 규칙 판단

### 5.3 api 계층

책임:

- `GET /`, `GET /health`, `POST /query` 엔드포인트 처리
- 요청 필드 유효성 검사
- 서비스 호출
- 서비스 결과를 HTTP 응답으로 변환
- `GET /health`의 body 금지 규칙 적용
- `GET /` HTML 페이지 반환

비책임:

- SQL parser 직접 호출
- storage/index 직접 접근

### 5.4 service 계층

책임:

- API 입력을 내부 실행 요청으로 변환
- 공통 정책 적용
- 엔진 어댑터 호출

비책임:

- HTTP 파싱
- B+ 트리 직접 조작

### 5.5 engine adapter 계층

책임:

- SQL 문자열을 lexer/parser/executor에 연결
- AST에서 대상 테이블 이름 추출
- 스키마를 로드해 canonical table key를 계산
- 실행 전 락 획득, 실행 후 락 해제
- executor 출력 캡처
- 엔진 오류를 API 오류 코드로 변환

비책임:

- HTTP 상태코드 최종 결정
- 소켓 직접 송수신

### 5.6 기존 sql 계층

책임:

- SQL 문자열을 토큰으로 분리
- AST 생성
- 지원 문법 범위 검증

### 5.7 기존 execution 계층

책임:

- AST를 실제 동작으로 연결
- `INSERT`, `SELECT`, `WHERE id = ...` 분기
- 내부 `id` 자동 생성

### 5.8 기존 storage 계층

책임:

- 스키마 로딩
- CSV 헤더 검증
- CSV append
- CSV 스캔 및 단일 행 읽기

### 5.9 기존 index 계층

책임:

- B+ 트리 삽입, 탐색
- 테이블별 인덱스 레지스트리 관리
- CSV 기반 lazy rebuild
- 다음 자동 `id` 추적

## 6. 요청 처리 흐름

### 6.1 `GET /`

1. browser가 `GET /` 요청을 보낸다.
2. accept loop가 연결을 수락한다.
3. 작업 큐에 연결을 넣는다.
4. worker가 요청을 읽고 router로 전달한다.
5. `root_handler`가 브라우저 진입용 HTML을 생성한다.
6. HTTP 200 HTML 응답을 반환한다.

### 6.2 `GET /health`

1. client가 `GET /health` 요청을 보낸다.
2. accept loop가 연결을 수락한다.
3. 작업 큐에 연결을 넣는다.
4. worker가 요청을 읽고 router로 전달한다.
5. `health_handler`가 서버 상태를 확인한다.
6. HTTP 200 JSON 응답을 반환한다.

### 6.3 `POST /query`

1. client가 `POST /query` 요청을 보낸다.
2. accept loop가 연결을 수락한다.
3. 작업 큐에 연결을 넣는다.
4. worker가 요청을 읽고 JSON body에서 `sql` 문자열을 추출한다.
5. `query_handler`가 `db_service`를 호출한다.
6. `db_service`가 `sql_engine_adapter`에 실행을 위임한다.
7. adapter가 SQL을 lex/parse한다.
8. adapter가 AST에서 raw table name을 확인한다.
9. adapter가 `load_schema(schema_dir, data_dir, raw_table_name)`로 스키마를 미리 로드한다.
10. adapter가 `schema.storage_name`을 canonical table key로 확정한다.
11. adapter가 canonical table key 기준 table lock을 획득한다.
12. adapter가 executor를 호출한다.
13. executor가 storage/index 계층과 상호작용해 결과를 생성한다.
14. adapter가 executor 출력과 요약 메시지를 로컬 버퍼에 복사한다.
15. adapter가 락을 해제한다.
16. service/api 계층이 JSON 응답을 구성한다.
17. worker가 응답을 쓰고 연결을 정리한다.

### 6.4 작업 큐 단위

작업 큐가 담는 단위는 `connection task`다.

즉 큐에 들어가는 항목은 다음 정보의 묶음이다.

- client socket
- accept 시각 또는 요청 식별용 메타정보

worker는 큐에서 소켓 하나를 꺼내:

1. HTTP 요청 한 개를 읽고
2. 응답 한 개를 쓰고
3. 소켓을 닫는다

이 모델에서는 한 연결에서 여러 요청을 순차 처리하지 않는다.

### 6.4 canonical table key

table lock의 키는 SQL 원문에 들어온 raw table name이 아니라, `load_schema()` 이후 확정되는 `schema.storage_name`이다.

정규화 규칙:

1. SQL 파싱으로 raw table name을 얻는다.
2. `load_schema(schema_dir, data_dir, raw_table_name)`를 호출한다.
3. 스키마 로드가 성공하면 `schema.storage_name`을 canonical key로 사용한다.
4. table lock, 로그, 내부 동시성 보호 대상 이름은 모두 canonical key를 사용한다.

이 규칙이 필요한 이유:

- 7주차 엔진은 같은 물리 테이블을 `table_name` 또는 `storage_name` 둘 다로 찾을 수 있다.
- raw table name 기준으로 락을 잡으면 alias 이름과 실제 storage 파일명이 다른 경우 서로 다른 락을 잡게 된다.
- canonical key를 `storage_name`으로 통일해야 같은 물리 테이블 요청이 정확히 직렬화된다.

## 7. SQL 노출 범위

API 서버는 현재 엔진이 실제로 안정적으로 처리하는 SQL만 노출한다.

허용 범위:

- `INSERT INTO ... VALUES (...)`
- `SELECT * FROM ...`
- `SELECT column1, column2 FROM ...`
- `SELECT ... WHERE column = value`
- `SELECT ... WHERE id = <integer>`

비허용 범위:

- `UPDATE`
- `DELETE`
- `JOIN`
- `ORDER BY`
- `GROUP BY`
- `AND`, `OR`를 포함한 복합 조건
- 여러 문장을 한 요청에 함께 보내는 multi-statement 실행

추가 규칙:

- `id`는 사용자 컬럼이 아니라 내부 예약 키를 의미한다.
- 사용자 스키마에 `id` 컬럼을 둘 수 없다.
- `SELECT id`는 내부 `__internal_id`를 노출하는 조회로 해석한다.

## 8. 엔진 연동 방식

### 8.1 직접 CLI 재사용 금지

API 서버는 `app/main.c`의 CLI 진입점을 재사용하지 않는다.

이유:

- CLI는 표준입출력 기반이다.
- API 서버는 요청 단위로 구조화된 결과를 받아야 한다.
- CLI 출력 포맷과 HTTP 응답 포맷을 분리해야 한다.

### 8.2 어댑터 기반 연동

서버는 별도 `sql_engine_adapter`를 통해 기존 엔진과 연결한다.

어댑터의 내부 흐름은 다음과 같다.

1. SQL 문자열 blank 여부 확인
2. lexer 호출
3. parser 호출
4. raw table name 추출
5. `load_schema()`로 canonical table key 계산
6. 락 획득
7. `execute_statement()` 호출
8. 출력 캡처
9. 결과 구조체 생성
10. 락 해제

health check처럼 테이블을 사용하지 않는 요청은 이 경로를 타지 않는다.

### 8.3 결과 캡처 방식

현재 executor는 `FILE *out`에 표 형식 결과를 쓴다.

따라서 8주차 최소 구현에서는:

- `SELECT` 결과를 JSON row 배열로 재구성하지 않는다.
- executor가 출력한 표 문자열을 그대로 캡처해 `output` 필드에 넣는다.
- `summary`, `affected_rows`, `elapsed_ms`를 별도 필드로 제공한다.
- 최소 구현에서 `INSERT` 성공 응답의 `output`은 항상 빈 문자열 `""`로 고정한다.

이 선택의 장점은 다음과 같다.

- 기존 executor 수정 범위를 줄일 수 있다.
- 발표와 기능 시연에 필요한 결과 가시성이 충분하다.
- 7주차 테스트와 동작 의미를 최대한 유지한다.

## 9. 동시성 설계

### 9.1 기본 원칙

- 같은 테이블 요청은 정합성 우선으로 직렬화한다.
- 다른 테이블 요청은 병렬 처리 가능하게 한다.
- 전역 공유 구조는 별도 전역 락으로 보호한다.

### 9.2 왜 table-level exclusive lock 인가

현재 7주차 엔진은 다음 이유로 순수 read-only 구조가 아니다.

- `WHERE id = ...` 첫 조회 시 lazy rebuild가 일어날 수 있다.
- index registry는 메모리 전역 상태다.
- `INSERT`는 `next_id`, CSV append, index register를 함께 갱신한다.

따라서 같은 테이블에 대해 read/write를 세분화하지 않고, 우선 `exclusive lock` 하나로 보호하는 것이 가장 안전하다.

### 9.3 락 종류

#### 테이블 락

- 키: `schema.storage_name`
- 범위: 같은 테이블에 대한 모든 `SELECT`, `INSERT`
- 목적: 같은 테이블에 대한 race 방지
- raw SQL table name은 락 키로 직접 쓰지 않는다

#### registry mutex

- 범위: 전역 `TableIndexRegistry`
- 목적: 엔트리 생성, 조회, 재설정, 무효화 같은 전역 메타구조 보호
- 원칙: `registry.items`, `registry.count`, `registry.capacity`, 특정 테이블 엔트리의 생성/획득/loaded 상태 전이는 모두 mutex 경계 안에서 수행한다.

### 9.4 락 획득 순서

deadlock 방지를 위해 락 획득 순서를 아래처럼 고정한다.

1. SQL parse
2. raw table name 식별
3. `load_schema()`로 canonical table key 계산
4. table lock 획득
5. engine 내부에서 필요한 짧은 구간에 registry mutex 사용
6. engine 작업 종료
7. table lock 해제

`registry mutex`를 먼저 오래 잡은 상태에서 table lock을 기다리는 구조는 금지한다.

### 9.5 registry mutex의 역할 한계

`registry_mutex`는 전역 레지스트리 구조를 보호하지만, 같은 테이블에 대한 전체 정합성을 대신 보장하지는 않는다.

즉:

- `users`용 인덱스와 `orders`용 인덱스는 각각 별도로 존재한다.
- `registry_mutex`는 이 인덱스들을 담는 전역 registry 구조를 보호한다.
- 같은 테이블의 `next_id`, lazy rebuild, CSV append, index register 흐름은 여전히 table lock으로 직렬화해야 한다.

### 9.6 전역 락의 위치

`global engine mutex`는 최종 기본 설계가 아니다.

다만 아래 상황에서는 임시 fallback으로 허용할 수 있다.

- 디버깅 초기에 race 범위를 줄일 때
- table lock 구현 전 임시 안전장치가 필요할 때

최종 구조는 `table-level exclusive lock + registry mutex`를 기준으로 한다.

## 10. 스레드 풀과 작업 큐

### 10.1 스레드 풀

- worker 수는 고정 크기다.
- 기본값은 `4`로 두고 CLI 옵션으로 변경 가능하게 한다.
- 요청마다 새 스레드를 만드는 구조는 사용하지 않는다.
- 기본 listen 주소는 `127.0.0.1:8080`으로 둔다.
- 서버는 `--schema-dir`, `--data-dir`로 대상 데이터셋을 바꿀 수 있어야 한다.

### 10.2 작업 큐

- accept thread는 연결 단위 작업을 큐에 넣는다.
- worker thread는 큐에서 작업을 꺼내 처리한다.
- 큐는 bounded queue로 두고 기본 용량은 `64`로 한다.

### 10.3 큐 포화 정책

- 큐가 가득 차면 accept thread가 간단한 `503 Service Unavailable` HTTP 응답을 직접 작성한다.
- 해당 연결은 `503` 응답 전송 직후 닫는다.
- 요청을 조용히 버리지는 않는다.

이 경로에서는 worker를 거치지 않는다.

### 10.5 header 크기 계산 규칙

request line + header 크기 한도는 `8 KiB`이며, 계산 규칙은 다음과 같다.

- request line 한 줄 전체를 바이트 수에 포함한다.
- request line 끝의 `\r\n`도 포함한다.
- 각 header line 전체를 바이트 수에 포함한다.
- 각 header line 끝의 `\r\n`도 포함한다.
- header block 종료를 나타내는 마지막 빈 줄의 `\r\n`도 포함한다.
- 본문 바이트는 이 한도 계산에 포함하지 않는다.

추가 규칙:

- obsolete line folding은 지원하지 않는다.
- 줄바꿈으로 이어지는 folded header는 잘못된 header로 간주한다.

### 10.4 종료 정책

- 종료 트리거는 기본적으로 프로세스 종료 신호 또는 테스트용 stop hook이다.
- shutdown 시작 시 전역 `shutting_down` 플래그를 설정한다.
- listen socket을 닫아 accept loop를 깨운다.
- accept loop는 더 이상 새 작업을 enqueue하지 않는다.
- 이미 큐에 들어간 작업은 graceful shutdown에서 drain 후 종료한다.
- worker는 종료 플래그가 켜졌더라도 큐가 빌 때까지 남은 작업을 처리한다.
- 큐가 비고 모든 worker가 종료되면 프로세스를 종료한다.

새 연결 처리 규칙:

- shutdown 시작 이후 새 연결은 TCP 수준에서 거부되거나 즉시 닫힐 수 있다.
- shutdown 중 새 연결에 대해 반드시 HTTP 응답을 보장하지는 않는다.
- 따라서 shutdown 이후 새 연결 테스트는 "정상 API 응답을 받지 못해도 허용"으로 판정한다.

성공 기준:

- shutdown 시작 전 큐에 들어간 요청은 유실 없이 처리된다.
- 종료 중 deadlock 없이 worker join이 끝난다.

## 11. 실패 처리 원칙

### 11.1 SQL 오류

- lexer/parser 오류는 사용자 입력 오류로 처리한다.
- 미지원 SQL은 사용자 입력 오류로 처리한다.
- 잘못된 `WHERE id = value`의 value는 사용자 입력 오류로 처리한다.

### 11.2 엔진 오류

- 스키마 로딩 실패
- CSV 읽기 실패
- CSV/인덱스 불일치
- 인덱스 rebuild 실패

위 항목은 내부 실행 오류로 처리한다.

### 11.3 HTTP 및 서버 오류

- 잘못된 메서드는 `405`
- 없는 경로는 `404`
- 잘못된 JSON, 잘못된 Content-Type, 빈 `sql` 필드는 `400`
- `Content-Length`가 필요한 요청에서 길이 정보가 없으면 `411`
- request body 또는 SQL 길이 제한 초과는 `413`
- request line 또는 header가 한도 초과면 `431`
- `Transfer-Encoding: chunked`는 `501`
- 큐 포화는 `503`

### 11.4 인덱스 등록 실패

7주차 설계를 그대로 따른다.

- CSV append 후 인덱스 등록 실패 시 현재 요청은 실패로 반환한다.
- 해당 테이블 인덱스는 무효화한다.
- 다음 인덱스 접근 시 CSV 기준 rebuild로 복구할 수 있게 한다.

## 12. 테스트 관점의 아키텍처 요구

### 12.1 최소 테이블 구성

테스트 데이터는 최소 2개 테이블을 준비하는 것이 권장된다.

이유:

- 같은 테이블 직렬화 검증
- 다른 테이블 병렬 처리 검증

### 12.2 필수 검증 시나리오

- `GET /health` 정상 응답
- `GET /` 정상 HTML 응답
- `POST /query` 정상 `SELECT`
- `POST /query` 정상 `INSERT`
- `POST /query` 응답의 `output`이 LF 줄바꿈을 유지하고 JSON 직렬화/역직렬화 후에도 깨지지 않는지 확인
- alias table name과 storage file name이 달라도 같은 물리 테이블 락을 공유하는지 확인
- 같은 테이블 동시 `INSERT`에서 `id` 중복이 없는지 확인
- 같은 테이블 동시 read/write에서 정합성이 깨지지 않는지 확인
- 다른 테이블 요청이 병렬로 진행되는지 확인
- 서버 재시작 후 인덱스 rebuild 가능 여부 확인
- 큐 포화 시 즉시 `503`이 반환되는지 확인
- body/header 한도 초과 요청이 명세 상태 코드로 거부되는지 확인
- graceful shutdown에서 drain이 완료되는지 확인

## 13. 한계와 후속 확장

이번 구조의 의도적 한계는 다음과 같다.

- 브라우저 진입점은 `GET /` 하나지만, 실제 DB 조작 계약은 여전히 `POST /query` 중심이다.
- 응답 데이터는 구조화된 row 배열이 아니라 문자열 표 출력 중심이다.
- 같은 테이블 요청은 모두 직렬화하므로 높은 write 경쟁 상황에서는 병목이 생길 수 있다.
- read/write lock 분리, 세분화된 인덱스 락, 세션 격리 수준은 이번 범위에 포함하지 않는다.

후속 확장 후보는 다음과 같다.

- 구조화된 result set JSON
- read/write lock 분리
- 쿼리 timeout
- request id / trace id
- richer metrics endpoint

## 14. 구현 완료 기준

아래 조건을 만족하면 본 아키텍처 문서 기준 최소 구현 완료로 본다.

- API 서버가 `HTTP/1.1` 기반으로 동작한다.
- `GET /`, `GET /health`, `POST /query`가 동작한다.
- worker thread pool과 bounded task queue가 동작한다.
- 기존 7주차 엔진을 재사용한다.
- 같은 테이블 요청은 직렬화된다.
- 다른 테이블 요청은 병렬 처리 가능하다.
- 전역 index registry는 별도 mutex로 보호된다.
- 큐 포화 시 `503`을 반환한다.
- README에서 이 구조를 설명할 수 있다.
