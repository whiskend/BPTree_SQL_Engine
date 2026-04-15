#ifndef BENCHMARK_H
#define BENCHMARK_H

#include <stddef.h>

typedef struct {
    size_t row_count;
    size_t probe_count;
    double insert_total_ms;
    double id_select_total_ms;
    double id_select_avg_ms;
    double non_id_select_total_ms;
    double non_id_select_avg_ms;
    double speedup_ratio;
} BenchmarkReport;

int run_benchmark(const char *db_dir,
                  const char *table_name,
                  size_t row_count,
                  size_t probe_count,
                  BenchmarkReport *out_report,
                  char *errbuf, size_t errbuf_size);

#endif
