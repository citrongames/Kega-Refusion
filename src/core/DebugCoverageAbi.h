#ifndef KEGA_DEBUG_COVERAGE_ABI_H
#define KEGA_DEBUG_COVERAGE_ABI_H
#include <stdint.h>
#include <stddef.h>
#define KR_COV_VERSION 1u
#define KR_COV_MAX_SOURCES 64u
enum kr_cov_kind { KR_COV_EXEC=1, KR_COV_READ=2 };
enum kr_cov_source_flag { KR_COV_SOURCE_ROM=1, KR_COV_SOURCE_RAM=2, KR_COV_SOURCE_PARTIAL=4 };
typedef struct kr_cov_source {
    uint32_t id, size, exec_cpu_mask, read_cpu_mask, flags;
    uint32_t inspect_domain, inspect_offset;
    char name[48];
} kr_cov_source;
typedef struct kr_cov_event {
    uint32_t cpu, kind, source, offset, size;
} kr_cov_event;
/* Owner-thread, non-yielding observer. Spans refer to canonical physical bytes,
 * not logical bus addresses. source=0 records an observed access whose origin
 * is unknown; it must never mark coverage at an inferred physical location.
 * EXEC marks consumed instruction-stream bytes, including operands. Speculative
 * prefetch and side-effect-free debugger/trace peeks are not execution.
 * READ marks guest CPU data reads, not DMA or instruction fetches.
 * Source IDs are stable physical allocations, 1..63; descriptors identify
 * actual supported CPUs. inspect_domain=0 means no exact memory-view link.
 * Runtime metadata is not part of native save states. */
typedef void (*kr_cov_callback)(void*,const kr_cov_event*);
/* size_t retro_kegarefusion_coverage_sources_v1(unsigned version,kr_cov_source*,size_t);
 * bool retro_kegarefusion_coverage_bind_v1(unsigned version,uint32_t exec_cpus,
 *      uint32_t read_cpus,kr_cov_callback,void*);
 * A null callback detaches independently of debugger and trace hooks.
 */
#endif
