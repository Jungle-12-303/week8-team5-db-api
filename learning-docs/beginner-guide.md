# SQL Processor 7주차 초심자 가이드

이 문서는 7주차 기준 현재 코드베이스를 처음 읽는 사람을 위한 입문 자료입니다.

목표는 두 가지입니다.

- 이 프로젝트가 전체적으로 어떻게 돌아가는지 감을 잡는다.
- 실제 코드를 어디서부터 읽어야 하는지 길을 잃지 않게 한다.

## 1. 이 프로젝트가 하는 일

이 프로젝트는 아주 작은 파일 기반 SQL 처리기입니다.

사용자가 `INSERT` 나 `SELECT` 문장을 입력하면 프로그램은 아래 순서로 동작합니다.

1. SQL 문장을 읽습니다.
2. 문장을 잘게 나눕니다.
3. 문장이 무슨 뜻인지 해석합니다.
4. 실제 `schema/` 와 `data/` 폴더를 읽거나 수정합니다.
5. 필요하면 `id` 인덱스를 사용합니다.
6. 결과를 화면에 출력합니다.

즉, "SQL 문장 -> 해석 -> 실행 -> CSV 저장/조회 -> 결과 출력" 흐름으로 생각하면 됩니다.

## 2. 7주차에서 새로 추가된 것

6주차와 비교했을 때 이번 주에 가장 중요한 변화는 아래입니다.

- 레코드에 `id`가 자동으로 붙습니다.
- `id`를 키로 하는 메모리 기반 B+ 트리가 생겼습니다.
- `WHERE id = 123` 형태의 조회는 인덱스를 사용합니다.
- 일반 `WHERE department = '...'` 조회는 계속 CSV 선형 탐색을 사용합니다.
- 대량 데이터 삽입과 성능 비교를 위한 벤치마크 실행 파일이 추가됐습니다.

즉, 이번 주의 핵심은 "기존 SQL 처리기를 유지한 채, `id` 조회만 빠르게 만드는 것"입니다.

## 3. 가장 먼저 이해하면 좋은 흐름

처음 코드를 읽을 때는 아래 순서가 가장 쉽습니다.

1. `src/app/main.c`
2. `src/sql/lexer.c`
3. `src/sql/parser.c`
4. `src/execution/executor.c`
5. `src/storage/schema.c`
6. `src/storage/storage.c`
7. `src/index/table_index.c`
8. `src/index/bptree.c`
9. `src/benchmark/benchmark_main.c`

이 순서대로 보면 "입력 -> 해석 -> 실행 -> 저장 -> 인덱스 -> 성능 측정" 흐름이 자연스럽게 이어집니다.

## 4. 폴더 구조 한눈에 보기

- `src/app`
  프로그램 시작점이 있습니다.
- `src/sql`
  SQL 문장을 읽고 해석하는 코드가 있습니다.
- `src/execution`
  해석된 SQL을 실제 동작으로 바꾸는 코드가 있습니다.
- `src/storage`
  스키마와 CSV 파일을 읽고 쓰는 코드가 있습니다.
- `src/index`
  B+ 트리와 테이블별 인덱스 관리 코드가 있습니다.
- `src/benchmark`
  대량 삽입과 성능 비교를 위한 실행 파일이 있습니다.
- `src/common`
  문자열, 파일, 리스트 같은 공용 도구 함수가 있습니다.
- `include/sqlparser`
  `.h` 헤더 파일이 모여 있습니다.
- `schema`
  테이블 구조 정보가 있습니다.
- `data`
  실제 데이터가 CSV 형태로 저장됩니다.
- `benchmark-workdir`
  대량 테스트 전용 스키마와 CSV 샘플이 있습니다.
- `tests`
  테스트 코드가 있습니다.

## 5. 핵심 용어를 아주 쉽게 설명하면

- `lexer`
  긴 SQL 문장을 작은 조각으로 자르는 단계입니다.
- `parser`
  잘린 조각을 보고 SQL의 의미를 해석하는 단계입니다.
- `AST`
  해석 결과를 담아두는 구조체입니다.
- `schema`
  테이블 이름과 컬럼 순서 같은 구조 정보입니다.
- `executor`
  해석된 결과를 실제 동작으로 연결하는 부분입니다.
- `B+ 트리`
  정렬된 키를 빠르게 찾기 위한 트리 구조입니다.
- `index`
  원하는 레코드를 더 빨리 찾기 위한 보조 자료구조입니다.
- `offset`
  CSV 파일 안에서 특정 행이 시작하는 위치입니다.

## 6. 실제 실행 흐름

### 6-1. 시작점

`src/app/main.c`가 프로그램의 시작점입니다.

이 파일은 아래 일을 합니다.

1. 사용자가 SQL 파일을 줬는지, SQL 문장을 직접 줬는지, REPL로 들어왔는지 확인
2. SQL 문자열 준비
3. `lex_sql()` 호출
4. `parse_statement()` 호출
5. `execute_statement()` 호출
6. 결과 메시지 출력

즉 `main.c`는 전체 흐름을 연결하는 지휘자 역할입니다.

### 6-2. SQL을 조각내기

`src/sql/lexer.c`는 SQL 문장을 토큰으로 나눕니다.

예를 들어 아래 SQL이 있으면:

```sql
SELECT name, age FROM student WHERE id = 10;
```

대략 이런 조각으로 나눕니다.

- `SELECT`
- `name`
- `,`
- `age`
- `FROM`
- `student`
- `WHERE`
- `id`
- `=`
- `10`
- `;`

### 6-3. SQL을 이해하기

`src/sql/parser.c`는 토큰 목록을 보고 문장을 해석합니다.

예를 들어:

```sql
INSERT INTO student (department, student_number, name, age)
VALUES ('컴퓨터공학과', '2024001', '김민수', 20);
```

를 해석한 결과는 대략 이런 정보가 됩니다.

- 문장 종류: `INSERT`
- 테이블 이름: `student`
- 컬럼 목록: `department`, `student_number`, `name`, `age`
- 값 목록: `'컴퓨터공학과'`, `'2024001'`, `'김민수'`, `20`

이 정보를 담는 구조체 정의는 `include/sqlparser/sql/ast.h`에 있습니다.

### 6-4. 실제 동작 수행하기

`src/execution/executor.c`는 parser가 만든 결과를 실제 동작으로 바꿉니다.

여기서 7주차 핵심 로직이 많이 들어갑니다.

- `INSERT`면 새 `id`를 계산합니다.
- 완성된 행을 CSV에 저장합니다.
- 저장한 행의 위치를 인덱스에 등록합니다.
- `SELECT ... WHERE id = ...`면 정수인지 검사한 뒤 인덱스를 사용합니다.
- 일반 `WHERE`면 기존 선형 탐색으로 내려보냅니다.

즉 executor는 "해석된 뜻"을 "실제 행동"으로 바꾸는 단계이면서, 이번 주 확장의 중심입니다.

### 6-5. 스키마와 CSV 다루기

`src/storage/schema.c`는 테이블 구조가 맞는지 확인합니다.

예를 들어 `student` 테이블이라면 아래 두 파일을 함께 봅니다.

- `schema/student.meta`
- `data/student.csv`

여기서 확인하는 것은 주로 아래 내용입니다.

- 메타 파일이 존재하는가
- 테이블 이름이 맞는가
- 컬럼 목록이 있는가
- CSV 헤더와 스키마 컬럼 순서가 같은가

`src/storage/storage.c`는 실제 CSV 한 줄을 읽고, 나누고, 추가하는 일을 맡습니다.

또한 7주차에서는 특정 오프셋의 행만 직접 읽는 기능도 맡습니다.

### 6-6. 인덱스는 어디서 관리하나

`src/index/table_index.c`는 테이블별 인덱스를 관리합니다.

이 파일의 핵심 역할은 아래와 같습니다.

- 어떤 테이블의 인덱스가 이미 메모리에 있는지 관리
- 아직 없으면 CSV를 다시 읽어 인덱스를 재구성
- 새 `id`와 행 위치를 인덱스에 등록
- 중복 `id`, 누락된 `id`, 잘못된 `id`를 오류로 처리

`src/index/bptree.c`는 실제 B+ 트리 자료구조 자체를 담당합니다.

쉽게 말하면:

- `table_index.c`는 "테이블 단위 인덱스 관리자"
- `bptree.c`는 "키를 빠르게 찾는 트리 엔진"

입니다.

### 6-7. 벤치마크는 어디서 보나

`src/benchmark/benchmark_main.c`는 대량 성능 측정용 별도 실행 파일입니다.

이 파일은 아래 일을 합니다.

1. 벤치마크용 CSV를 초기화
2. 같은 규칙으로 대량 데이터를 생성
3. 1,000,000건 이상 삽입 가능
4. `WHERE id = ...` 조회 시간을 측정
5. 일반 컬럼 `WHERE ...` 조회 시간을 측정
6. 두 결과를 비교

즉, 발표에서 "인덱스가 왜 필요한지"를 숫자로 보여 주는 코드입니다.

## 7. 데이터 파일은 어떻게 생겼나

예를 들어 `student` 테이블은 메타 파일이 이런 식입니다.

```txt
table=학생
columns=id,department,student_number,name,age
```

CSV 파일은 보통 이렇게 시작합니다.

```txt
id,department,student_number,name,age
1,컴퓨터공학과,2023001,김학생,20
2,전자공학과,2023002,이학생,21
```

즉:

- `schema/*.meta` = 구조 설명서
- `data/*.csv` = 실제 데이터

라고 생각하면 이해하기 쉽습니다.

## 8. 현재 지원하는 SQL

현재 지원하는 주요 SQL은 아래입니다.

- `INSERT INTO table_name (col1, col2, ...) VALUES (...);`
- `SELECT * FROM table_name;`
- `SELECT col1, col2 FROM table_name;`
- `SELECT ... FROM table_name WHERE column = value;`
- `SELECT ... FROM table_name WHERE id = 123;`

현재 제외하는 기능 예시:

- `UPDATE`
- `DELETE`
- `JOIN`
- `ORDER BY`
- `GROUP BY`
- 복합 `WHERE`

## 9. 직접 실행해보기

빌드:

```bash
make all
```

테스트:

```bash
make test
```

벤치마크:

```bash
make benchmark
```

실행 예시:

```bash
./build/bin/sqlparser -e "SELECT * FROM 학생;"
./build/bin/sqlparser -e "SELECT name, age FROM 학생 WHERE department = '컴퓨터공학과';"
./build/bin/sqlparser -e "SELECT * FROM 학생 WHERE id = 1;"
./build/bin/sqlparser -e "INSERT INTO 학생 (department, student_number, name, age) VALUES ('컴퓨터공학과', '2024001', '김민수', 20);"
```

벤치마크 예시:

```bash
./build/bin/benchmark_runner benchmark-workdir/schema benchmark-workdir/data student 1000000 100
```

## 10. 초보자 기준으로 꼭 기억하면 좋은 점

- 이 프로젝트는 진짜 DBMS 전체가 아니라, 작은 SQL 처리기입니다.
- 데이터를 서버가 아니라 CSV 파일에 저장합니다.
- SQL을 바로 실행하는 것이 아니라 `읽기 -> 해석 -> 실행` 단계를 거칩니다.
- `id` 조회만 인덱스를 타고, 나머지는 기존 방식대로 동작합니다.
- 인덱스는 메모리에 있고, CSV가 항상 기준입니다.
- 그래서 메모리 인덱스가 비어 있어도 CSV를 다시 읽어 복구할 수 있습니다.

## 11. 디버깅할 때 어디에 브레이크포인트를 두면 좋은가

처음 디버깅할 때는 아래 위치가 이해하기 좋습니다.

- `src/app/main.c`
  입력 방식 분기와 `run_sql_text`
- `src/sql/parser.c`
  `INSERT` 와 `SELECT` 를 구분하는 부분
- `src/execution/executor.c`
  `execute_insert`, `execute_select`
- `src/index/table_index.c`
  인덱스 재구성과 등록
- `src/index/bptree.c`
  키 삽입과 검색
- `src/storage/storage.c`
  CSV append와 오프셋 기반 행 읽기

## 12. 마지막으로 한 문장 정리

이 프로젝트는 "간단한 SQL 문장을 읽어서 CSV 파일을 작은 데이터베이스처럼 다루고, `id` 조회만 B+ 트리로 빠르게 만드는 프로그램"입니다.

처음에는 어렵게 보여도 아래 한 줄만 기억하면 됩니다.

`main -> lexer -> parser -> executor -> storage/index -> output`
