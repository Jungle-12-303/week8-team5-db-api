/*
 * service/db_service.c
 *
 * 이 파일은 API 계층과 엔진 어댑터 사이를 잇는 service 계층이다.
 *
 * 현재 구현은 매우 얇지만, 구조적으로는 중요한 경계다.
 *
 * 이유:
 * - API 핸들러가 engine adapter 구조체와 세부 설정을 직접 다루지 않게 한다.
 * - 이후 공통 정책, 로깅, 권한 검사, 입력 보정 같은 로직을 넣을 자리를 남겨 둔다.
 * - 문서에서 정의한 controller/api -> service -> engine 의 의존 방향을 유지한다.
 */
#include "sqlparser/service/db_service.h"

/*
 * API 계층이 받은 SQL 문자열을 내부 엔진 실행 요청으로 위임한다.
 *
 * 지금은 adapter_config를 그대로 넘기는 단순 위임이지만,
 * "핸들러가 곧바로 엔진을 호출하지 않는다"는 구조적 의미가 더 중요하다.
 */
int db_service_execute_sql(DbService *service, const char *sql, SqlEngineAdapterResult *result) {
    return sql_engine_adapter_execute(&service->adapter_config, sql, result);
}
