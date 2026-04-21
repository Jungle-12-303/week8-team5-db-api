# SQL Processor Test Cases

## 1. 문서 목적

이 문서는 현재 루트 프로젝트에 구현된 테스트 케이스를 범주별로 정리하고, 각 테스트가 어떤 요구사항을 검증하는지 빠르게 파악할 수 있도록 돕는다.

기준 파일은 `tests/test_runner.c`이며, 7주차 요구사항 정의서와 아키텍처 문서를 기준으로 테스트를 분류한다.

현재 기준 테스트 규모는 아래와 같다.

- 현재 상위 테스트 함수 수: `31개`
- 현재 assertion 수: `375개`
- 이전 보강 전 기준: 상위 테스트 함수 `12개`, assertion `110개`

## 2. 테스트 실행 방법

Linux 또는 Docker 환경에서는 아래 두 방식 중 하나를 사용한다.

```bash
bash scripts/docker-test.sh
```

```bash
make test
```

테스트 러너는 단일 바이너리로 빌드되며, 성공 시 각 케이스에 대해 `[PASS]`를 출력하고 마지막에 전체 실행 수와 실패 수를 요약한다.

## 3. 테스트 분류

### 3.1 B+ 트리 단위 테스트

대상 함수:
- `test_bptree_insert_and_search`
- `test_bptree_split_preserves_searchability`

검증 내용:
- 여러 키를 순차 삽입할 수 있다.
- 삽입된 키를 다시 검색할 수 있다.
- 키에 매핑된 오프셋 값이 보존된다.
- 중복 키 삽입은 거부된다.
- 충분한 키를 삽입해 리프/내부 노드 분할이 발생해도 검색 결과가 유지된다.

연관 요구사항:
- `B+ 트리` 구현
- 중복 키 처리 정책
- 인덱스 검색 기본 동작

### 3.2 파서 테스트

대상 함수:
- `test_parser_where`
- `test_parser_error_details`
- `test_parser_utf8_identifiers`

검증 내용:
- `SELECT ... WHERE` 문장을 파싱할 수 있다.
- `WHERE` 컬럼명과 비교값을 AST에 저장한다.
- UTF-8 테이블명을 파싱할 수 있다.

연관 요구사항:
- 기존 `SELECT ... WHERE` 유지
- UTF-8 식별자 처리
- `WHERE id = value` 표현 재사용 기반 확보

### 3.3 스키마 로딩 테스트

대상 함수:
- `test_schema_loading_with_alias_filename`
- `test_schema_reports_missing_directory`
- `test_schema_reports_alias_candidate_open_failure`

검증 내용:
- 파일명과 테이블명이 달라도 `table=` 선언을 기준으로 스키마를 찾을 수 있다.
- 별칭 테이블명과 실제 저장 파일명을 함께 유지한다.
- 별칭 스키마 탐색 중 후보 meta 파일을 열 수 없으면 실제 파일 열기 오류를 노출한다.

연관 요구사항:
- `schema/*.meta` + `data/*.csv` 기반 파일 저장
- 기존 프로젝트 호환성 유지

### 3.4 INSERT 및 자동 내부 PK 테스트

대상 함수:
- `test_insert_auto_id`
- `test_insert_overrides_user_id`

검증 내용:
- `INSERT` 시 숨은 내부 PK가 자동 생성된다.
- 첫 삽입은 내부 PK `1`부터 시작한다.
- 일부 컬럼만 입력해도 누락 컬럼은 빈 문자열로 저장된다.
- 내부 PK는 시스템이 결정하며 사용자 스키마에 명시적 `id` 컬럼이 없어도 동작하는 것이 목표 설계다.
- `INSERT` 이후 테이블 인덱스가 메모리에 로드된다.

연관 요구사항:
- 시스템 관리 `__internal_id`
- 새 레코드 `__internal_id = max(__internal_id) + 1`
- 기존 SQL 처리기와의 연동 유지

### 3.5 일반 WHERE 테스트

대상 함수:
- `test_select_execution_with_general_where`
- `test_select_explicit_internal_id_column`

검증 내용:
- `WHERE age = 20` 같은 일반 조건은 기존 선형 탐색으로 동작한다.
- 일치하는 여러 행을 올바르게 출력한다.
- 일치하지 않는 행은 결과에 포함되지 않는다.
- 일반 `WHERE` 실행만으로는 `id` 인덱스를 만들지 않는다.
- 일반 `WHERE` 경로에서도 `SELECT id, ...`로 내부 PK를 명시적으로 출력할 수 있다.

연관 요구사항:
- `WHERE` 대상이 `id`가 아니면 기존 선형 탐색 유지

### 3.6 인덱스 기반 내부 PK 조회 테스트

대상 함수:
- `test_select_execution_with_id_index`
- `test_select_star_hides_internal_id_column`

검증 내용:
- `WHERE id = 2` 조건에서 내부 PK 기준으로 정확한 한 행을 반환한다.
- 비대상 행이 결과에 섞이지 않는다.
- 실행 과정에서 테이블 인덱스가 메모리에 로드된다.
- `SELECT *`는 내부 PK를 자동으로 노출하지 않고 사용자 컬럼만 출력한다.

연관 요구사항:
- `WHERE id = <number>`일 때 B+ 트리 인덱스 사용
- 인덱스 기반 단건 조회

### 3.7 인덱스 재구성 테스트

대상 함수:
- `test_index_rebuild_after_reset`
- `test_index_rebuild_after_invalidate`
- `test_insert_failure_recovers_via_rebuild`

검증 내용:
- 한 번 로드한 인덱스를 메모리에서 초기화한 뒤에도 다시 재구성할 수 있다.
- 재구성 후 같은 내부 PK 조회 결과를 얻는다.
- 인덱스를 강제로 무효화한 뒤에도 다음 `WHERE id` 조회에서 다시 재구성할 수 있다.
- CSV 저장 후 인덱스 등록 실패가 발생해도 다음 인덱스 조회에서 CSV 기준 재구성으로 복구할 수 있다.

연관 요구사항:
- 인덱스 lazy load
- CSV 기준 인덱스 재구성
- 인덱스 등록 실패 후 복구 정책

### 3.8 오류 처리 테스트

대상 함수:
- `test_cli_error_messages`
- `test_cli_bare_directory_argument_is_not_sql`
- `test_invalid_where_id_value`
- `test_invalid_where_id_value_no_header_output`
- `test_invalid_rebuild_data`
- `test_schema_rejects_explicit_id_column`
- `test_insert_without_explicit_id_column`
- `test_select_where_id_without_explicit_id_column`

검증 내용:
- `SELECT`만 입력한 경우, 지원하지 않는 명령어, 세미콜론 누락, 빈 입력, 알 수 없는 옵션 등 CLI 경계값 입력에서 실제 오류 유형에 맞는 메시지를 출력한다.
- bare argument가 디렉터리일 때 SQL 문자열로 오인하지 않고 파일 경로 오류로 처리한다.
- `WHERE id = abc`처럼 정수가 아닌 값은 오류 처리한다.
- `WHERE id = abc` 오류 시 헤더를 먼저 출력하지 않는다.
- CSV 재구성 중 내부 PK 순서가 깨지지 않아야 한다.
- 내부 PK는 CSV 행 순서 기준으로 재구성 가능해야 한다.
- 명시적 `id` 컬럼 유무와 무관하게 내부 PK 정책이 유지되어야 한다.

연관 요구사항:
- CLI 예외 입력에 대한 세부 오류 메시지
- 파일 경로 입력과 SQL 문자열 입력의 정확한 구분
- `WHERE id` 정수 검증
- 재구성 중 내부 PK 순서 정책

### 3.9 CSV 저장 테스트

대상 함수:
- `test_csv_escape`
- `test_storage_reports_missing_table_file`
- `test_read_entire_file_reports_missing_sql_file`
- `test_storage_row_offset_roundtrip`

검증 내용:
- 쉼표와 큰따옴표가 포함된 값을 CSV 규칙에 맞게 escape 한다.
- 저장 후 다시 읽었을 때 escape 형태가 유지된다.
- append 시 반환한 오프셋으로 같은 행을 다시 정확하게 읽을 수 있다.

연관 요구사항:
- CSV 영속 저장 유지
- 인덱스와 독립적인 저장 계층 안정성

### 3.10 벤치마크 테스트

대상 함수:
- `test_benchmark_main_resets_dataset`

검증 내용:
- `prepare` 모드 실행 전 남아 있던 기존 CSV 데이터를 헤더 기준으로 초기화한다.
- `prepare` 모드는 같은 입력 파라미터로 실행하면 같은 데이터셋을 다시 생성한다.
- `query-only` 모드는 이미 준비된 데이터셋을 그대로 사용하고 CSV를 다시 초기화하지 않는다.
- `prepare`와 `query-only`가 모두 별도 schema/data 작업 디렉터리에서 정상 동작한다.

연관 요구사항:
- 재현 가능한 대량 데이터 생성 경로
- 벤치마크 전용 실행 경로
- `prepare` / `query-only` 모드 분리
- 인덱스/선형 조회 성능 비교의 전제 데이터셋 보장

## 4. 현재 미포함 또는 향후 보강 후보

현재 테스트에 포함되지 않았거나, 별도 보강 여지가 있는 항목은 아래와 같다.

- 대량 삽입 후 인덱스 조회와 선형 조회의 성능 비교 자동 검증

## 5. 요약

현재 테스트 러너는 7주차 핵심인 자동 내부 PK, 인덱스 조회, 재구성, 기본 오류 처리를 커버한다. 특히 명시적 `id` 컬럼 금지, `id` 없는 스키마에서의 INSERT, `WHERE id` 인덱스 조회, `SELECT id`를 통한 내부 PK 확인, `SELECT *`의 비노출 정책, 재구성 후 동일 결과 보장을 회귀 테스트에 포함한다.

즉, 현재 테스트 세트는 구현된 7주차 핵심 기능의 회귀 테스트로 사용할 수 있으며, 이후에는 성능/벤치마크와 복구 실패 시나리오를 보강하는 방식으로 확장하는 것이 적절하다.
