# SQL Processor Week 7

파일 기반 SQL 처리기에 숨은 내부 PK `__internal_id` 자동 부여와 메모리 기반 B+ 트리 인덱스를 붙여, `WHERE id = ...` 조회를 최적화하는 7주차 프로젝트입니다.

### 문제 정의

6주차 SQL 처리기는 CSV를 선형 탐색하므로, 데이터가 커질수록 특정 레코드를 찾는 비용이 계속 커집니다.

### 7주차 목표

- `INSERT` 시 레코드에 숨은 내부 PK `__internal_id`를 자동 부여한다.
- `__internal_id`를 키로 사용하는 메모리 기반 B+ 트리를 만든다.
- `WHERE id = <number>` 조회는 인덱스를 사용하고, 다른 컬럼 조건은 기존 선형 탐색을 유지한다.

### B+Tree

![Week 7 B+Tree Structure](docs/diagrams/week7-bptree-structure-1to9.svg)

### 핵심 구현

- 기존 `INSERT`, `SELECT`, `SELECT ... WHERE` 흐름 유지
- 숨은 내부 PK `__internal_id` 자동 부여
- 테이블별 메모리 B+ 트리 인덱스 유지
- `WHERE id = <number>` 인덱스 조회
- CSV 기준 인덱스 재구성
- 1,000,000건 이상 삽입 가능한 성능 측정 진입점 제공

### 1. 전체 구조

![Week 7 Overall Flow](docs/diagrams/week7-overall-flow.svg)

### 2. INSERT 시 자동 ID 부여와 인덱스 등록

![Week 7 Insert Flow](docs/diagrams/week7-insert-index-flow.svg)

### 3. `WHERE id = ...` 조회 시 인덱스 사용

![Week 7 Select Id Flow](docs/diagrams/week7-select-id-flow.svg)

## 프로젝트가 푸는 문제

CSV 기반 저장소는 구현이 단순하지만, 원하는 레코드를 찾으려면 보통 처음부터 끝까지 읽어야 합니다.

예를 들어 `student.csv`에 1,000,000건이 들어 있을 때:

- `WHERE name = '김민수'`는 선형 탐색이 필요합니다.
- `WHERE id = 500000`도 인덱스가 없으면 같은 선형 탐색입니다.

이번 구현에서는 `id` 기준 조회만이라도 빠르게 만들기 위해 B+ 트리를 붙였습니다.

## 핵심 아이디어

### 1. CSV는 계속 영속 저장의 기준이다

- 실제 데이터는 계속 `data/*.csv`에 저장합니다.
- 인덱스는 메모리 구조이므로, 필요하면 CSV를 다시 읽어 재구성합니다.

### 2. `id`는 사용자 컬럼이 아니라 시스템 예약 키다

- 사용자 스키마와 CSV에는 일반 컬럼만 저장합니다.
- 시스템은 각 행에 대해 숨은 내부 PK `__internal_id`를 계산합니다.
- SQL 문법에서는 `WHERE id = ...`가 이 내부 키를 가리킵니다.

### 3. `WHERE id = ...`만 인덱스를 탄다

- `WHERE id = 1000`은 B+ 트리로 찾습니다.
- `WHERE department = '컴퓨터공학과'`는 기존 CSV 선형 탐색을 유지합니다.

이렇게 해야 기존 구조를 크게 깨지 않고 7주차 요구사항에 정확히 맞출 수 있습니다.

## 지원 기능

- `INSERT`
- `SELECT *`
- `SELECT column1, column2`
- `SELECT ... WHERE column = value`
- `WHERE id = <number>` 인덱스 조회
- `id` 자동 부여
- SQL 파일 실행
- SQL 문자열 직접 실행
- REPL 실행
- 별도 성능 비교 바이너리

## 디렉터리 구조

- `src/app`
  CLI 입력, 파일 입력, REPL 시작점
- `src/sql`
  lexer, parser, AST
- `src/execution`
  `INSERT`/`SELECT` 실행 분기, `id` 자동 생성, 인덱스 경로 선택
- `src/storage`
  스키마 로딩, CSV 검증, CSV 읽기/쓰기, CSV 순회
- `src/index`
  B+ 트리와 테이블별 인덱스 관리
- `src/benchmark`
  대량 삽입 및 조회 성능 측정용 별도 진입점
- `tests`
  단위/기능 테스트
- `docs`
  요구사항, 아키텍처, 테스트 문서
- `learning-docs`
  초심자용 학습 문서

## SQL 동작 규칙

### INSERT

- 사용자는 `id`를 빼고 나머지 컬럼만 넣습니다.
- 최종 저장되는 내부 PK `__internal_id`는 시스템이 자동으로 결정합니다.
- 새 내부 PK는 현재 테이블의 최대 `__internal_id + 1`입니다.
- 저장 성공 후 `__internal_id -> CSV 행 위치`가 메모리 B+ 트리에 등록됩니다.

예시:

```sql
INSERT INTO 학생 (department, student_number, name, age)
VALUES ('컴퓨터공학과', '2024001', '김민수', 20);
```

### SELECT

- 일반 `SELECT`와 일반 `WHERE`는 기존 CSV 선형 탐색을 사용합니다.
- `WHERE id = <number>`는 정수 검증 후 B+ 트리 인덱스를 사용합니다.
- `WHERE id = abc` 같은 입력은 오류입니다.
- `SELECT id`는 시연과 검증을 위해 내부 `__internal_id`를 보여 줍니다.
- `SELECT *`는 기존 의미를 유지하기 위해 사용자 컬럼만 출력하고, 내부 `__internal_id`는 자동 포함하지 않습니다.

예시:

```sql
SELECT * FROM 학생;
SELECT id, name FROM 학생;
SELECT name, age FROM 학생 WHERE department = '컴퓨터공학과';
SELECT * FROM 학생 WHERE id = 1000;
```

## 데모 시연

SQL 파서 REPL을 실행한 뒤 아래 순서로 입력합니다.

```bash
./build/bin/sqlparser
```

먼저 테스트 대상 레코드를 삽입합니다.

```sql
INSERT INTO student (department, student_number, name, age) VALUES ('경제학과', '2026005', '김금융', 21);
```

입력된 결과를 일반 컬럼 조건으로 확인합니다.

```sql
SELECT id, department, student_number, name, age FROM student WHERE name = '김금융';
```

B+ Tree 인덱스 조회 비교용으로 `id` 조건 조회를 실행합니다.

```sql
SELECT id, department, student_number, name, age FROM student WHERE id = 101014;
```

## 조회 성능 비교

조회 성능 비교는 별도 바이너리로 실행하며, `prepare`와 `query-only` 두 모드를 지원합니다.

```bash
./build/bin/benchmark_runner prepare <schema_dir> <data_dir> <table_name> <row_count>
./build/bin/benchmark_runner query-only <schema_dir> <data_dir> <table_name> <target_id> [query_repeat]
```

예시:

```bash
./build/bin/benchmark_runner prepare benchmark-workdir/schema benchmark-workdir/data student 1000000
./build/bin/benchmark_runner query-only benchmark-workdir/schema benchmark-workdir/data student 500000 10
```

인자 의미:

- `<schema_dir>`: 성능 비교 대상 스키마 폴더
- `<data_dir>`: 성능 비교 대상 CSV 폴더
- `<table_name>`: 대상 테이블 이름
- `<row_count>`: `prepare` 모드에서 생성하고 삽입할 총 레코드 수
- `<target_id>`: `query-only` 모드에서 인덱스 조회 대상으로 삼을 `id`
- `[query_repeat]`: `query-only` 모드에서 같은 조회를 몇 번 반복해서 평균을 낼지 정하는 선택 인자

100만 건 시연용 권장 명령:

```bash
make benchmark
./build/bin/benchmark_runner prepare benchmark-workdir/schema benchmark-workdir/data student 1000000
./build/bin/benchmark_runner query-only benchmark-workdir/schema benchmark-workdir/data student 500000 10
./build/bin/sqlparser -e "SELECT id, department, student_number, name, age FROM 학생 WHERE id = 500000;"
```

참고:

- 더 안정적인 평균값이 필요하면 `query_repeat`를 `100`으로 올릴 수 있습니다.
- 다만 선형 조회는 100만 건을 매번 순회하므로, `100`회 반복은 발표 시연용으로는 오래 걸릴 수 있습니다.

실제 측정 결과:

```text
Query target id: 1000000
Query target column: department
Query target value: department_1000000
Query repeats: 10
Indexed query avg time: 0.085954 sec
Linear query avg time: 0.224854 sec
```

- 아래 이미지는 1,000,000건 데이터셋에서 `WHERE id = ...` 인덱스 조회와 일반 컬럼 조건 조회를 비교한 실제 측정 결과입니다.
- 단일 CLI 첫 실행은 인덱스 재구성 비용이 섞일 수 있으므로, 발표에서는 `query-only` 기준 평균 시간을 해석하는 것이 맞습니다.
- 시연은 `10`회 반복으로 빠르게 보여주고, 아래 `100`회 반복 결과는 참고 자료로 함께 제시합니다.

- 단일 CLI 조회는 기능 시연용이고, `query-only`는 성능 비교용입니다.
- `benchmark_runner` 실행 전에는 `make benchmark`를 먼저 실행해야 최신 바이너리를 사용할 수 있습니다.
- Windows 시연에서는 PowerShell 또는 Windows Terminal을 사용하는 편이 안정적이고, 한글이 깨지면 UTF-8 코드페이지 설정을 먼저 확인하세요.


## 이번 주 발표에서 강조할 포인트

- 이전 차수 코드를 버리지 않고 7주차 요구를 덧붙였는가
- `WHERE id` 경로만 인덱스를 타도록 책임 분리가 되어 있는가
- 성능 비교와 테스트를 통해 결과를 검증했는가
- 구현한 코드를 팀원이 직접 설명할 수 있는가

## 참고 문서

- [docs/requirements.md](/C:/developer_folder/jungle-sql-processor-2nd/docs/requirements.md)
- [docs/architecture.md](/C:/developer_folder/jungle-sql-processor-2nd/docs/architecture.md)
- [docs/test-cases.md](/C:/developer_folder/jungle-sql-processor-2nd/docs/test-cases.md)
- [learning-docs/beginner-guide.md](/C:/developer_folder/jungle-sql-processor-2nd/learning-docs/beginner-guide.md)
- [learning-docs/docker-basics-for-week7.md](/C:/developer_folder/jungle-sql-processor-2nd/learning-docs/docker-basics-for-week7.md)
- [learning-docs/makefile-basics-for-sql-processor.md](/C:/developer_folder/jungle-sql-processor-2nd/learning-docs/makefile-basics-for-sql-processor.md)
