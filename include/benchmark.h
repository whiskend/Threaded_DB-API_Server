#ifndef BENCHMARK_H
#define BENCHMARK_H

#include <stddef.h>

/* 대량 insert와 indexed/full-scan SELECT 측정 결과를 한 번에 담는 리포트 구조체다. */
typedef struct {
    size_t row_count;               /* 벤치마크에서 실제로 삽입한 총 row 수다. */
    size_t probe_count;             /* SELECT 성능 측정에 사용한 반복 조회 횟수다. */
    double insert_total_ms;         /* 전체 insert 구간에 걸린 총 시간(ms)이다. */
    double id_select_total_ms;      /* id 인덱스 조회 전체에 걸린 총 시간(ms)이다. */
    double id_select_avg_ms;        /* id 인덱스 조회 1회 평균 시간(ms)이다. */
    double non_id_select_total_ms;  /* 비인덱스 조회 전체에 걸린 총 시간(ms)이다. */
    double non_id_select_avg_ms;    /* 비인덱스 조회 1회 평균 시간(ms)이다. */
    double speedup_ratio;           /* 비인덱스 평균 시간을 인덱스 평균 시간으로 나눈 배수다. */
} BenchmarkReport;

/*
 * db_dir와 table_name을 대상으로 row_count건 insert와 probe_count회 조회를 수행해
 * out_report에 총합/평균 시간과 속도 배수를 채우고 상태 코드를 반환한다.
 */
int run_benchmark(const char *db_dir,
                  const char *table_name,
                  size_t row_count,
                  size_t probe_count,
                  BenchmarkReport *out_report,
                  char *errbuf, size_t errbuf_size);

#endif
