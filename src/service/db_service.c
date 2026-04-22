/*
 * service/db_service.c
 *
 * API 계층과 엔진 어댑터 사이의 아주 얇은 서비스 레이어다.
 *
 * 현재 최소 구현에서는 별도 정책 없이 어댑터 호출만 위임하지만,
 * 라우터/핸들러가 엔진 세부사항을 직접 알지 않게 하는 경계 역할을 유지한다.
 */
#include "sqlparser/service/db_service.h"

/* API 요청을 내부 SQL 실행 요청으로 넘기는 서비스 진입점이다. */
int db_service_execute_sql(DbService *service, const char *sql, SqlEngineAdapterResult *result) {
    return sql_engine_adapter_execute(&service->adapter_config, sql, result);
}
