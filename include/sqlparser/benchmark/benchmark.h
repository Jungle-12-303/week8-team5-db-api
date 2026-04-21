/*
 * benchmark/benchmark.h
 *
 * 벤치마크 전용 진입점을 선언하는 헤더다.
 * 일반 CLI와 별도 바이너리에서 재사용하기 위해 main 로직을 함수로 분리해 둔다.
 */
#ifndef SQLPARSER_BENCHMARK_H
#define SQLPARSER_BENCHMARK_H

/* benchmark_runner의 실제 진입 함수다. */
int benchmark_main(int argc, char *argv[]);

#endif
