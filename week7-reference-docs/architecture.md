# SQL Processor 7주차 아키텍처 문서

## 1. 문서 목적

이 문서는 현재 SQL 처리기에 7주차 요구사항인 B+ 트리 인덱스를 추가할 때의 구조와 계층 책임을 정의한다.

이번 문서의 핵심 설계 결정은 다음과 같다.

- 기존 `app -> sql -> execution -> storage` 흐름을 유지한다.
- `id`는 사용자 스키마 컬럼이 아니라 SQL 처리기 내부에서 자동 부여하는 숨은 PK로 본다.
- 숨은 PK의 내부 구현명은 `__internal_id`로 통일한다.
- `WHERE id = <value>`는 숨은 내부 `__internal_id` 기준 조회로 해석한다.
- 내부 인덱스는 `__internal_id -> row_offset` 형태의 메모리 기반 B+ 트리로 관리한다.
- append-only 전제를 이용해 CSV 행 순서대로 `__internal_id`를 재구성한다.

## 2. 계층 구조

```text
app -> sql
app -> execution
execution -> storage
execution -> index
storage -> common
index -> common
benchmark -> execution / storage / index
```

## 3. 모듈 책임

### 3.1 app 계층

`src/app/main.c`

책임:

- CLI 인자 처리
- REPL 시작
- SQL 파일 입력 / 직접 입력 / stdin 입력 분기
- 일반 SQL 실행 흐름 시작

비책임:

- SQL 문법 해석
- B+ 트리 직접 조작

### 3.2 sql 계층

`src/sql/lexer.c`, `src/sql/parser.c`, `src/sql/ast.c`

책임:

- SQL 문자열을 토큰으로 분리
- 토큰을 AST로 파싱
- `INSERT`, `SELECT`, `SELECT ... WHERE` 구조 표현
- `WHERE id = value`를 일반 WHERE 표현과 같은 방식으로 AST에 담기

비책임:

- 실제 저장소 접근
- 인덱스 검색

### 3.3 execution 계층

`src/execution/executor.c`

책임:

- AST를 실제 동작으로 연결
- INSERT 시 내부 `__internal_id` 자동 생성
- 일반 SELECT와 인덱스 SELECT 경로 분기
- `WHERE id = value`일 때 value를 정수로 검증한 뒤 인덱스 경로 선택

비책임:

- CSV 파싱 세부 구현
- B+ 트리 노드 조작 세부 구현

### 3.4 storage 계층

`src/storage/schema.c`, `src/storage/storage.c`

책임:

- `.meta` 스키마 로드
- CSV 헤더 검증
- CSV append
- CSV 전체 스캔
- 오프셋 기반 단일 행 읽기

비책임:

- SQL 문법 해석
- 인덱스 사용 여부 결정

### 3.5 index 계층

`src/index/bptree.c`, `src/index/table_index.c`

책임:

- B+ 트리 생성/삽입/탐색
- 테이블별 인덱스 레지스트리 관리
- CSV 기준 lazy load / rebuild
- `__internal_id`와 row offset 매핑 유지
- 다음 자동 `id` 추적

비책임:

- SQL 파싱
- 사용자 출력 포맷

### 3.6 benchmark 계층

`src/benchmark/benchmark_main.c`

책임:

- 데이터 준비 모드(`prepare`)
- 조회 전용 측정 모드(`query-only`)
- 인덱스 조회와 선형 조회 평균 시간 비교

비책임:

- 일반 CLI 진입

## 4. 핵심 데이터 모델

### 4.1 사용자 행

CSV에는 사용자 스키마에 정의된 컬럼만 저장한다.

예:

```csv
department,student_number,name,age
컴퓨터공학과,2024001,김민수,20
```

### 4.2 숨은 내부 `__internal_id`

- 사용자 스키마에는 나타나지 않는다.
- INSERT 시 execution 계층이 자동 생성한다.
- B+ 트리 key로 사용된다.
- SQL 표면 문법에서는 `WHERE id = <value>`로만 직접 조회 대상으로 등장한다.
- 선택 컬럼에 한해 `SELECT id`는 내부 `__internal_id`를 노출하는 가상 컬럼으로 해석할 수 있다.
- 반대로 `SELECT *`는 사용자 스키마 컬럼만 확장하고, 숨은 내부 `__internal_id`는 자동으로 포함하지 않는다.

### 4.3 사용자 `id` 컬럼 금지 정책

- 사용자 스키마에 명시적 `id` 컬럼을 두는 방식은 7주차 목표 아키텍처에서 폐기한다.
- `id`는 SQL 표면 문법에서 내부 예약 키 `__internal_id`를 가리키는 이름으로 취급한다.
- 따라서 사용자 컬럼명이 `id`이면 파서, 실행기, 결과 표현 계층에서 의미 충돌이 생긴다.
- 이 문서는 사용자 스키마에서 `id`를 예약 이름으로 간주하는 설계를 기준으로 한다.

### 4.3 인덱스 value

- CSV 파일에서 해당 행이 시작하는 바이트 오프셋

이 방식을 택하는 이유:

- 저장소가 append-only 구조다.
- `UPDATE`, `DELETE`가 없어 기존 행 위치가 안정적이다.
- 인덱스 조회 후 한 행만 직접 읽기 쉽다.

## 5. INSERT 흐름

7주차 INSERT 흐름은 아래와 같다.

1. `app/main.c`가 SQL 입력을 받는다.
2. `sql/parser.c`가 `InsertStatement`를 만든다.
3. `execution/executor.c`가 대상 테이블 스키마를 읽는다.
4. execution 계층이 테이블 인덱스 레지스트리에서 다음 `__internal_id`를 계산한다.
5. execution 계층이 사용자 컬럼 기준 CSV 행을 구성한다.
6. `storage/storage.c`가 CSV 끝에 새 행을 append한다.
7. append 성공 후 row offset을 반환한다.
8. `index/table_index.c`가 `__internal_id -> row_offset`을 B+ 트리에 등록한다.

실패 처리:

- CSV 저장 후 인덱스 등록이 실패하면 현재 실행은 오류를 반환한다.
- 해당 테이블 인덱스는 메모리에서 무효화한다.
- 다음 인덱스 접근 시 CSV를 기준으로 다시 재구성한다.

## 6. SELECT 흐름

### 6.1 일반 SELECT

1. parser가 `SelectStatement`를 만든다.
2. executor가 `WHERE` 존재 여부와 컬럼 정보를 확인한다.
3. `WHERE`가 없거나 `id`가 아니면 기존 storage 전체 스캔 경로를 사용한다.

### 6.2 인덱스 SELECT

1. parser가 `WHERE id = value`를 포함한 `SelectStatement`를 만든다.
2. executor가 비교 값이 정수인지 확인한다.
3. 해당 테이블 인덱스가 없거나 무효화된 상태면 index 계층이 CSV를 읽어 재구성한다.
4. index 계층이 B+ 트리에서 `__internal_id`를 탐색한다.
5. 탐색 성공 시 storage 계층이 해당 오프셋의 행 하나만 읽는다.
6. executor가 기존 SELECT 출력 형식에 맞춰 결과를 출력한다.

## 7. 인덱스 재구성

### 7.1 재구성 규칙

인덱스 재구성 시 CSV 데이터 행을 처음부터 끝까지 읽으며:

- 첫 번째 데이터 행에 내부 `id = 1`
- 두 번째 데이터 행에 내부 `id = 2`
- ...

처럼 행 순서대로 `__internal_id`를 다시 부여한다.

즉 append-only 전제 아래에서는:

- 같은 CSV 내용
- 같은 행 순서

라면 항상 같은 `id -> row_offset` 매핑이 나온다.

### 7.2 next_id 계산

재구성 후:

- 마지막으로 부여된 `__internal_id`가 `N`이면
- 다음 INSERT의 `__internal_id`는 `N + 1`

이다.

## 8. 테이블 인덱스 관리자

권장 개념 구조:

```c
typedef struct {
    char *table_name;
    int loaded;
    BPlusTree tree;
    int next_internal_id;
} TableIndex;
```

역할:

- 특정 테이블 인덱스가 메모리에 있는지 확인
- 없으면 CSV 기준 재구성
- 등록 실패 후 무효화된 인덱스를 다시 재구성
- 다음 자동 `__internal_id` 추적

## 9. 문서화된 설계 원칙

- `app`은 입력 방식만 결정하고 인덱스를 직접 다루지 않는다.
- `sql`은 SQL 의미를 해석하지만 저장소를 직접 읽지 않는다.
- `execution`이 인덱스 사용 여부를 최종 결정한다.
- `storage`는 사용자 컬럼 기준 CSV 저장/읽기만 담당한다.
- `index`는 숨은 내부 `__internal_id`와 오프셋 매핑만 담당한다.

## 10. 기존 코드와의 연결 지점

- `src/app/main.c`
  일반 SQL 처리기 CLI 진입점
- `src/benchmark/benchmark_main.c`
  벤치마크 전용 진입점
- `src/sql/parser.c`
  기존 WHERE AST 표현을 그대로 사용하되, `id`를 가상 내부 키로 해석
- `src/execution/executor.c`
  자동 `id` 생성과 인덱스 조회 경로 분기 추가
- `src/storage/storage.c`
  append 시 오프셋 반환, 오프셋 기반 단건 조회, CSV 전체 순회 제공
- `src/index/table_index.c`
  행 순서 기반 `__internal_id` 재구성과 B+ 트리 등록 담당

## 11. 결론

이번 설계는 기존 SQL 처리기 구조를 유지하면서, 사용자 스키마에 `id` 컬럼을 강제하지 않고도 `__internal_id`를 자동 부여해 B+ 트리 인덱스를 적용하는 것을 목표로 한다.

핵심 요약:

- 사용자 컬럼은 CSV에 그대로 저장
- `__internal_id`는 execution/index 계층이 숨은 상태로 관리
- `WHERE id = ?`는 내부 `__internal_id` 기준 인덱스 조회
- CSV 재스캔 시 행 순서대로 `__internal_id`를 복구
