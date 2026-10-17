/* XTS exec engine - private interfaces between src/exec modules (and framework code that needs them). Not for families:
 * use xts/exec.h. Include as <exec/exec_priv.h> (the Makefile puts src/ on the include path). */
#ifndef XTS_EXEC_PRIV_H
#define XTS_EXEC_PRIV_H
#include <stdint.h>
#include <xts/exec.h>

/* big buffer (exec_common.c): the program's pointer to it, and the address the case code sees for a host pointer
 * (backend: console identity, host a fixed fake address) */
void *xts_big_ptr_internal(void);
uint64_t xts_exec_big_addr(const void *p);

/* resolve deferred code pointers into `st` right before a run (exec_common.c) */
void xts_exec_resolve_code_ptrs(const xts_exec_t *x, xts_state_t *st, uint64_t kernel_start);

/* exec hooks (exec_common.c): run the v1 slot + the chained hooks around a run. pre returns <0 if a hook refused (then
 * the post hooks of the pre hooks that ran have been called and x->status/err are set); post runs them in reverse. */
int xts_exec_hooks_run_pre(xts_exec_t *x);
void xts_exec_hooks_run_post(xts_exec_t *x);

/* extra trap ranges (exec_common.c): the table the backends patch into / check against */
typedef struct xts_trange { uint64_t lo, hi; } xts_trange_t;
extern xts_trange_t xts_exec_ranges[XTS_EXEC_EXTRA_RANGES];
/* backend hook called after a range changed (console: re-patch the vector stubs); may be NULL */
extern int (*xts_exec_ranges_changed)(void);
/* memory guard (exec_common.c): 1 = do not execute `word`: model_result XTS_M_DSI, or a memory-unit word (whatever the model
 * said) whose access window leaves the scratch area and the big buffer (see xts_exec_ea_outside in xts/exec.h); `why` says so.
 * model_result / fault_ea: what xts_model_exec returned for the same input (xts_mem_t.fault_ea). */
int xts_exec_mem_guard(int id, uint32_t word, const xts_state_t *in, int model_result, uint64_t fault_ea, char *why,
                       int why_size);
/* the access window of a memory word: 1 and the EA and [lo, hi) of the first reading, 0 if the word touches no memory */
int xts_exec_ea_window(int id, uint32_t word, const xts_state_t *in, uint64_t *ea, uint64_t *lo, uint64_t *hi);
/* backend assertion used by xts_exec_word on both backends: 0, or -1 with x->status = XTS_EXEC_NOTRUN and x->err set */
int xts_exec_word_assert(xts_exec_t *x, uint32_t word);
#endif
