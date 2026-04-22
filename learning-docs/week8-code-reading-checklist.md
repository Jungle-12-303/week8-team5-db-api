# 8주차 코드 읽기 체크리스트

## 1. 문서 목적

이 문서는 8주차 API 서버 코드를 읽을 때
"어디부터 어떤 질문을 가지고 보면 되는가"를 빠르게 확인하기 위한 체크리스트다.

긴 설명 문서라기보다,
코드 리뷰 전후에 빠르게 점검하는 용도로 사용하는 것을 목표로 한다.

## 2. 먼저 읽을 문서 체크

아래 문서를 읽었는지 먼저 확인한다.

- [ ] `docs/week8-architecture.md`
- [ ] `docs/week8-api-spec.md`
- [ ] `docs/week8-requirements.md`
- [ ] `README.md`

이 순서를 먼저 보는 이유:

- 아키텍처 문서는 계층 책임과 전체 흐름을 먼저 잡아 준다.
- API 명세서는 외부 계약과 오류 응답 기준을 알려 준다.
- 요구사항 문서는 이번 주차의 본질과 범위를 확인하게 해 준다.
- README는 발표 관점에서 구현 요약을 보여 준다.

## 3. 읽기 순서 체크

아래 순서대로 읽으면 전체 구조를 가장 빠르게 파악하기 쉽다.

### 3.1 시작점

- [ ] `src/app/sqlapi_server_main.c`

확인할 것:

- [ ] 서버 실행 옵션은 무엇인가
- [ ] 기본값은 어디서 채워지는가
- [ ] `create -> start -> wait -> destroy` 흐름이 어떻게 이어지는가

### 3.2 서버 런타임

- [ ] `src/server/server.c`
- [ ] `src/server/task_queue.c`
- [ ] `src/server/worker_pool.c`

확인할 것:

- [ ] accept loop는 어디에 있는가
- [ ] queue full 시 누가 `503`을 반환하는가
- [ ] worker thread는 어떤 함수를 반복 실행하는가
- [ ] shutdown 시 accept thread와 worker thread는 어떤 순서로 정리되는가

### 3.3 HTTP 계층

- [ ] `src/http/http_request.c`
- [ ] `src/http/http_response.c`
- [ ] `src/http/router.c`

확인할 것:

- [ ] HTTP request line과 header는 어디서 파싱되는가
- [ ] `Content-Length`, `Content-Type`, `chunked` 검증은 어디서 하는가
- [ ] path와 method에 따라 어떤 핸들러로 분기되는가

### 3.4 API 계층

- [ ] `src/api/root_handler.c`
- [ ] `src/api/health_handler.c`
- [ ] `src/api/query_handler.c`

확인할 것:

- [ ] `GET /`, `GET /health`, `POST /query`가 각각 어디서 처리되는가
- [ ] `POST /query` 에서 JSON body의 `sql` 필드를 어디서 추출하는가
- [ ] API 핸들러는 어디까지 하고, 실제 SQL 실행은 어디로 넘기는가

### 3.5 Service 와 Engine Adapter

- [ ] `src/service/db_service.c`
- [ ] `src/engine/sql_engine_adapter.c`
- [ ] `src/engine/engine_lock_manager.c`

확인할 것:

- [ ] service 계층이 왜 얇아도 필요한가
- [ ] 실제 SQL 엔진 진입은 어디서 일어나는가
- [ ] table-level lock은 어디서 잡히는가
- [ ] 같은 테이블과 다른 테이블 요청이 어떻게 갈라지는가

### 3.6 기존 엔진 재사용 영역

- [ ] `src/sql/lexer.c`
- [ ] `src/sql/parser.c`
- [ ] `src/execution/executor.c`
- [ ] `src/storage/schema.c`
- [ ] `src/storage/storage.c`
- [ ] `src/index/table_index.c`
- [ ] `src/index/bptree.c`

확인할 것:

- [ ] lexer는 SQL 원문을 어떻게 토큰으로 나누는가
- [ ] parser는 어떤 SQL만 허용하는가
- [ ] executor는 언제 인덱스를 사용하고 언제 CSV 전체를 스캔하는가
- [ ] schema와 storage는 각각 무엇을 담당하는가
- [ ] `WHERE id = ...` 경로에서 B+ 트리가 어디서 사용되는가

## 4. 요청 1건 추적 체크

아래 질문에 막힘 없이 답할 수 있는지 확인한다.

- [ ] 클라이언트 연결은 어느 스레드가 받는가
- [ ] 연결은 queue에 어떤 형태로 들어가는가
- [ ] worker는 queue에서 무엇을 꺼내는가
- [ ] HTTP 요청은 어디서 `HttpRequest` 구조체가 되는가
- [ ] `POST /query` 는 어디서 `query_handler` 로 가는가
- [ ] `sql` 문자열은 어디서 service 계층으로 넘어가는가
- [ ] 실제 SQL 실행은 어디서 시작되는가
- [ ] 결과는 어디서 JSON 응답으로 바뀌는가
- [ ] 소켓은 어디서 닫히는가

## 5. 병렬 처리 이해 체크

아래 질문에 답할 수 있으면 8주차 핵심을 이해한 것이다.

- [ ] `accept loop + bounded queue + worker thread pool` 구조를 한 문장으로 설명할 수 있는가
- [ ] queue는 왜 block 방식이 아니라 `try_push` + 즉시 거절 구조인가
- [ ] 같은 테이블 요청이 직렬화되는 실제 지점은 어디인가
- [ ] 다른 테이블 요청은 왜 병렬 처리될 수 있는가
- [ ] queue depth와 worker count는 어디에서 확인되는가

## 6. 발표 직전 최소 확인 체크

발표 전에 아래만 다시 봐도 핵심 설명은 가능해야 한다.

- [ ] `README.md`
- [ ] `src/server/server.c`
- [ ] `src/server/task_queue.c`
- [ ] `src/api/query_handler.c`
- [ ] `src/engine/sql_engine_adapter.c`
- [ ] `src/engine/engine_lock_manager.c`

이때 꼭 말할 수 있어야 하는 문장:

- [ ] "이번 주차는 기존 SQL 엔진을 유지한 채 바깥에 API 서버 계층을 추가한 것이다."
- [ ] "요청은 accept loop, bounded queue, worker thread pool 구조로 병렬 처리된다."
- [ ] "같은 테이블은 직렬화하고, 다른 테이블은 병렬 처리한다."
- [ ] "`WHERE id = ...` 는 기존 B+ 트리 인덱스를 그대로 사용한다."

## 7. 자주 헷갈리는 포인트 체크

- [ ] API 핸들러가 직접 SQL parser를 호출하는 것이 아니라는 점을 이해했는가
- [ ] service 계층은 얇지만 계층 경계 유지에 의미가 있다는 점을 이해했는가
- [ ] lock은 단순한 "논리적 금지"가 아니라 실제 `pthread mutex` 기반 직렬화라는 점을 이해했는가
- [ ] `queue_depth` 는 현재 대기 중인 작업 수이지, 전체 소켓 수가 아니라는 점을 이해했는가
- [ ] `elapsed_ms` 와 `wall_elapsed_ms` 의 차이를 설명할 수 있는가

## 8. 테스트 코드 확인 체크

- [ ] `tests/test_api_server.c`
- [ ] `tests/test_runner.c`

확인할 것:

- [ ] 정상 요청과 비정상 요청이 모두 검증되는가
- [ ] queue full 시나리오가 테스트되는가
- [ ] 동시성 시나리오가 테스트되는가
- [ ] 테스트 데이터는 실제 프로젝트 데이터와 분리된 임시 경로를 쓰는가

## 9. 최종 점검 질문

아래 질문에 답할 수 있으면 코드 리뷰 준비가 된 상태다.

- [ ] "요청 1건이 어디서 시작해서 어디서 끝나는가?"
- [ ] "동시성 제어는 어디서 걸리는가?"
- [ ] "기존 엔진 재사용은 정확히 어느 계층에서 일어나는가?"
- [ ] "왜 service 계층을 남겨 두었는가?"
- [ ] "왜 같은 테이블은 막고, 다른 테이블은 병렬 처리하는가?"
- [ ] "queue full, invalid JSON, unsupported SQL 은 각각 어디서 처리되는가?"
