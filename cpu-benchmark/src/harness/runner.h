/*
 * runner.h - runs every kernel of one or two variants and keeps the
 * statistics (DESIGN.md section 1).  Portable: uses only platform.h.
 */
#ifndef BENCH_RUNNER_H
#define BENCH_RUNNER_H

#include <stdint.h>

#include "bench.h"
#include "platform.h"

/* Compile-time knobs (DESIGN.md section 6). */
#ifndef BENCH_SAMPLES
#define BENCH_SAMPLES 7 /* samples per variant and kernel */
#endif
#ifndef BENCH_SAMPLE_MS
#define BENCH_SAMPLE_MS 60 /* target length of one sample  */
#endif

#define BENCH_MAX_VARIANTS 2
#define BENCH_MAX_SAMPLES 31
#define BENCH_MAX_KERNELS 128
#define BENCH_GUARD_BYTES PLAT_GUARD_BYTES

/* Kernel status bits; 0 is ok.  Any bit set excludes the kernel from the
 * geomeans. */
#define BST_MISMATCH 0x01u /* A and B results differ (beyond tolerance)  */
#define BST_UNSTABLE 0x02u /* a run() returned a different value         */
#define BST_OVERRUN 0x04u  /* guard pattern around arena_bytes damaged   */
#define BST_BADDESC 0x08u  /* descriptor invalid or A/B descriptors differ */
#define BST_MISSING 0x10u  /* placeholder: kernel source does not exist  */
#define BST_NOTIME 0x20u   /* the timer did not advance                  */
#define BST_WRONG_A 0x40u  /* A's result differs from the reference      */
#define BST_WRONG_B 0x80u  /* B's result differs from the reference      */

/* bench_kres.ref: how the results were checked against the reference
 * results of src/harness/refs.h (big-endian, from the qemu run). */
enum {
	REF_NONE = 0,  /* no reference recorded for this kernel            */
	REF_CHECKED,   /* compared; BST_WRONG_* is set where they differ    */
	REF_STALE,     /* recorded for a different descriptor: not compared */
	REF_BYTEORDER  /* big-endian-only result on a little-endian host    */
};

typedef struct bench_vres {
	double tmin, tmed, tmax;          /* ticks per run()             */
	double sample[BENCH_MAX_SAMPLES]; /* ticks per run(), in order   */
	double ref;                       /* warm-up result (reference)  */
	double bad_value;                 /* first result != ref         */
	uint32_t bad_calls;               /* calls whose result != ref   */
	uint32_t calls;                   /* run() calls in total        */
} bench_vres;

typedef struct bench_kres {
	const bench_kernel *k;                     /* variant A's descriptor  */
	const bench_kernel *kv[BENCH_MAX_VARIANTS];
	bench_vres v[BENCH_MAX_VARIANTS];
	uint32_t status;   /* BST_* bits                                     */
	const char *note;  /* why BST_BADDESC was set                        */
	int index;         /* position in the variant tables                */
	uint32_t reps;     /* run() calls per sample (same for A and B)      */
	int samples;       /* samples per variant                           */
	double speedup;    /* time B / time A (two variants)                */
	double rel_diff;   /* |A-B| / max(|A|,|B|) of the reference results */
	int ref;           /* REF_*                                          */
	double ref_value;  /* the reference result (REF_CHECKED)             */
	uint64_t ticks;    /* wall time spent on this kernel                */
} bench_kres;

typedef struct bench_results {
	int nvar;
	const bench_variant_info *var[BENCH_MAX_VARIANTS];
	int ntable;         /* kernels in variant A's table                  */
	int nk;             /* kernels run (after --only)                    */
	bench_kres k[BENCH_MAX_KERNELS];
	const char *error;  /* table-level problem (lengths differ, ...)     */
	int samples, sample_ms, quick;
	uint64_t tb_hz;
	double cyc_per_tick;
	uint64_t total_ticks;
	int nfailed;
	int nref;           /* kernels whose results were checked (REF_CHECKED) */
	/* geometric means of the speedups of the kernels that passed */
	double geo[BENCH_CAT_COUNT];
	int ngeo[BENCH_CAT_COUNT];
	double geo_all;
	int ngeo_all;
} bench_results;

/* Runs the kernels; returns 0, or -1 on a fatal setup problem (r->error;
 * a non-fatal table problem also sets r->error but returns 0).
 * progress(r, i, 0) is called before kernel r->k[i] starts and
 * progress(r, i, 1) after it finished; it may be NULL. */
int bench_run_all(bench_results *r, const bench_variant_info *const *vars,
		  int nvar, const bench_opts *o,
		  void (*progress)(const bench_results *r, int i, int phase),
		  void *arena);

/* "ok", "MISMATCH", "WRONG B", ... (the most severe bit; nvar decides
 * between "WRONG A" and "WRONG" for single-variant images). */
const char *bench_status_word(uint32_t status, int nvar);
/* "none", "checked", "stale", "byteorder" for bench_kres.ref. */
const char *bench_ref_word(int ref);
/* "INT", "BR", ... ; "?" when out of range. */
const char *bench_cat_name(uint32_t cat);
/* Conversions of ticks per run(). */
double bench_us(const bench_results *r, double ticks);
double bench_cyc_per_unit(const bench_results *r, const bench_kernel *k,
			  double ticks); /* 0 if unknown */
double bench_munits_per_s(const bench_results *r, const bench_kernel *k,
			  double ticks);
/* (median - min) / min, in percent. */
double bench_noise_pct(const bench_vres *v);

#endif /* BENCH_RUNNER_H */
