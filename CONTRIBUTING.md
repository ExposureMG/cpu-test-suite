# Contributing to XTS

## Ground rules (Phase B)
- **Do not edit** `include/`, `src/core/`, `src/exec/`, `src/enc/`, `src/timing/`, `src/plat/`, `tools/`, the `Makefile`
  or `isa/`. They are the framework. If you need an interface change, stop and ask for it (describe the call you need).
- Only add or edit the files you own: `src/tests/<family>.c`, `src/conds/<axis>.c`, `src/model/<family>.c`,
  `src/smt/*`, `profiles/<name>.c`, `host/tests/<name>.c`. New files are picked up by wildcard; no list to edit.
  `*_console.c` files are built only for the console, `*_host.c` only for the host.
- Build into your own directory: `make BUILD=build/<you> ...`. Run `make host-test BUILD=build/<you>` before you finish;
  also `make host-dryrun PROFILE=<p> BUILD=build/<you>` for any profile your family joins, and
  `make check-model BUILD=build/<you>` if you touched `src/model`.
- Never read the CPU key, the keyvault or fuse lines 3-6. No storage code: nothing is ever written on the console (no
  `fopen`, no libfat, no USB); output leaves only through the sink (records, `xts_log`) as UDP messages
  (`include/xts/net.h`, `docs/PROTOCOL.md`). Families never call `xts_net_send` directly.
- Public headers are `<xts/...>` (`<xts/xts.h>` pulls in all of them except `<xts/cond_util.h>`). Framework-private
  headers are reached through `-Isrc`: `<core/core.h>`, `<exec/exec_priv.h>`, `<model/model_priv.h>` (model symbols such as
  the larx reservation and `xf_estimate_ok`), `<plat/plat_libxenon.h>` (libxenon externs missing from its headers). No
  relative `"../"` includes and no ad hoc `extern` declarations of framework symbols.
- Kernels must not touch r1, r2, r13 or r30. A deliberately illegal word goes in with `xk_data()` (kernels) or into a
  `.xts_testwords` data section (static words), never into code.

## Commands
| Command | What it does |
|---|---|
| `make PROFILE=p [BUILD=d]` | console ELF `d/xts-p.elf32`, then `check_illegal` on it |
| `make host-test` | `tools/xts_recv.py --selftest`, then all `HTEST`s (host/tests): registry, scheduler, sink + in-RAM results store, wire format, end-to-end runs through the real receiver (`t_net_*`: byte-exact results, injected loss / duplicates / corruption / reordering, test-core crash and hang reports, quarantine), fit maths, encoder round trip, kernel builder, model |
| `make host-dryrun PROFILE=p` | the whole profile on the PC through the real scheduler and sink; the messages go as UDP to `tools/xts_recv.py --exec`, which writes `d/dryrun-p/run/`. Single words are simulated by the model when it has a handler (status pass/fail as on hardware, `"backend":"host-model"`), otherwise `skip`. Every kernel is built with the encoder and every word decoded back: the last log line counts words/kernels/bad words; exit status 2 if any word does not decode as the instruction it was built for. This is how you validate a family without hardware. |
| `make check` | `check-encoder` (C encoder vs `xenon-as -mvmx128 -mcell -many` for all 484 insns), `check-model` (results4 replay), `check-illegal`, `check-records` (schema/vocabulary on the received dry run) |
| `make check-model` | per mnemonic: matched / mismatched (with diffs) / unimplemented; fails only on mismatches |
| `make recv` | `tools/xts_recv.py --out xts-runs`: receive a console run (README "Output") |

## Adding a test family (`src/tests/<family>.c`)
```c
#include <xts/xts.h>

static int my_enum(xts_caselist_t *cl)                 /* once per run; cases must be deterministic */
{
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    if (d->isa != XTS_ISA_ALTIVEC || d->hw != XTS_HW_EXECUTES) continue;    /* descriptors drive enumeration */
    for (int k = 0; k < 8; k++) {
      xts_case_t *c = xts_case_addf(cl, "%s#%d", d->mnem, k);              /* stable, unique id */
      c->insn = id;
      c->arg[0] = k;
    }
  }
  return 0;
}

static int my_run(const xts_case_t *c, xts_ctx_t *ctx)  /* once per case x condition combination x repeat */
{
  static xts_exec_t x;                                  /* ~6 KB: keep it static */
  xts_case_init(&x);                                    /* canary baseline + xts_env (MSR[SF], FPSCR/VSCR bits) */
  int vd = 3, va = 4, vb = 5;
  xts_set_vr(&x, va, xts_vr4(xts_rng_next(&ctx->rng), 1, 2, 3));   /* ctx->rng: deterministic per case */
  xts_set_vr(&x, vb, xts_vr4(0, 0, 0, 7));
  uint32_t w = xts_enc(c->insn, 0, vd, va, vb);         /* operands in asm order; 0 + xts_enc_last_error on error */
  xts_semantic_check(ctx, &x, w, NULL);                 /* runs on the CPU and in the model, emits ONE record */
  return 0;                                             /* <0 = family error (recorded as fail if nothing emitted) */
}

XTS_FAMILY(exec_altivec, .kind = "semantic", .axes = "sf,vscr_nj", .enumerate = my_enum, .run = my_run,
           .block_cases = 64, .desc = "AltiVec instructions vs the model");
```
- Name: the first `_` of the id becomes `.` (`exec_altivec` -> `exec.altivec`). Kinds: `self semantic timing sched mem smt calib`.
- `.axes`: the axes the family is sensitive to; the scheduler only varies those (and only if the profile asks).
- `.block_cases`: cases per scheduler block (a BLOCK message with the record count follows each block). A crash or hang of the test core stops the run with a crash/hang record for the exact case (see "Transport, crashes and quarantine"); a CRUMB message names the case before it runs (every case of `self`/`semantic` families, every 16th of the other kinds).
- `run()` emits records and log lines; they leave right away as REC/LOG messages. Nothing is stored and there is nothing to flush. Do not call `xts_results_foreach` from `run()` (it is for `finish()`).
- Optional `.setup()` (once, before the first block; `<0` skips the family) and `.teardown()`.
- Optional `.finish(ctx)`: summaries after the family's last block of a profile row (see "Family finish()").
- Emitting yourself: `xts_record_t r; xts_record_init(&r); r.metric = "latency"; r.value = v; r.unit = "cycles";
  r.status = XTS_ST_PASS; r.detail = json_text; xts_emit(ctx, &r);` - or `xts_emit_status`, `xts_emit_value`,
  `xts_time_emit_ex` (timing). Build `detail` with `xts_json_t` (`xts/json.h`; `xj_members(j, "{...}")` copies the members
  of a pre-rendered object); a truncated detail is flagged, never silent. Metric and unit names come from the vocabularies
  (below); several statuses combine with `xts_status_worst(a, b)` only.
- Kernels (`xts/kernel.h`): `xk_new` once, then per case `xk_reset`, `xk_emit(k, XI_..., flags, ops...)`, labels
  (`xk_label/xk_bind/xk_b/xk_bc/xk_bdnz`), `xk_loop_begin/end`, at least one `xk_exit`, `xk_finish`, then
  `xts_exec_kernel(&x, k)`. Code addresses go in registers with `xts_set_code_ptr`. Data pointers: `xts_set_ptr`
  (512-byte scratch), `xts_set_mem_ptr` (pointer chains in scratch), `xts_big` + `xts_set_big_ptr` + `xts_prep`
  (working-set buffers, random/sequential pointer cycles, dcbf/touch).
- Results: `x.out` (state), `x.mem_out`, `x.trap` (`trapped`, `vector` 0x700/0x600, `cause` 0x02 trap / 0x08 illegal,
  `srr0`, `where` 0 slot / 1 kernel, `offset`), `x.status` (`XTS_EXEC_OK`; on the host `SIMULATED` or `DRYRUN`).
- Timing (`xts/timing.h`): fill an `xts_tspec_t` (start from `XTS_TSPEC_DEFAULT`: >=5 depths, >=4 repeats) with a
  `body(k, depth, arg)` that emits `depth` units, optional `pre`/`setup` (or `setup2(x, depth, run, arg)`);
  `xts_time_measure` -> `xts_time_emit_ex(ctx, &spec, &res, XI_..., &(xts_temit_t){ .metric = "latency" })` (see "Timing
  records"). r27-r29 are the frame's (and, with `.loop = XTS_LOOP_GPR`, CR field `.loop_crf`; CTR is then the body's).
  `xts_time_differential` does same-shape filler differentials. Status is `pass` if spread and A/B/A shift < 2 % and
  residual < 1 %; `nonlinear` if only the residual is over and it is at least 5x the largest per-depth MAD (repeatable
  structure: the value is the least-squares average, `fit.marginal_cycles` the cost of each depth step); else `noisy`;
  `regressed` if the JSON baseline is off by more than 10 %. `xts_cycles_per_tick` (default 64) is set by the calib family.
- Constants into registers: `xk_li64(k, r, v)` (any 64-bit value, `xts/kgen.h`), `xk_li32` (zero-extended), `xk_li32s`
  (sign-extended). Barriers: `XI_lwsync`, `XI_ptesync` are encoder aliases of `sync` (L=1/2).
- Families never run anything privileged beyond what the framework does, and leave the machine as they found it.

## Adding a condition axis (`src/conds/<axis>.c`)
```c
#include <xts/xts.h>

static const char *const rn_values[] = { "rn", "rz", "rp", "rm", NULL };   /* values[0] = default/idle state */
static int rn_apply(int v)                                                /* <0: cannot apply here -> block skipped */
{
  xts_env.fpscr_clr |= 3;
  xts_env.fpscr_set = (xts_env.fpscr_set & ~3u) | (uint32_t)v;
  return 0;
}
static void rn_revert(int v) { (void)v; xts_env.fpscr_clr &= ~3u; xts_env.fpscr_set &= ~3u; }

XTS_AXIS(fpscr_rn, .values = rn_values, .apply = rn_apply, .revert = rn_revert, .order = 10,
         .desc = "FPSCR[RN] of the case input");
```
- `apply`/`revert` bracket a whole block. Axes that shape cases set `xts_env` fields (sf, fpscr_set/clr,
  vscr_set/clr, code_offset, data_offset/data_kind, cache_state, thread_prio, chains, unroll, speed, sibling,
  other_cores; `xts_env_slot("name")` claims one of `xts_env.user[]` for anything new). `xts_case_init` applies sf,
  FPSCR and VSCR; `xts_exec_kernel` applies `code_offset`; `xk_prio_prologue` (used by the timing frame) applies
  `thread_prio`; families honour the rest.
- Axes that act on the machine do it in apply/revert. Per-case work (e.g. a sibling kernel around each case) uses
  chained hooks: `int h = xts_exec_hook_add(pre, post, ud)` in apply, `xts_exec_hook_remove(h)` in revert (pre in add
  order, post reversed; a refusing pre undoes the earlier ones; the core removes leftovers after every block and logs
  them). The v1 single slot `xts_exec_hooks.pre/post` still works (runs first/last).
- `xts_exec_trap_range(idx, lo, hi)` (idx 0..3, each range < 64 KB) makes a program/alignment interrupt in [lo, hi) count
  as "inside the case" (`trap.where = 2 + idx`). One index per user: sibling 0, other_cores 1..3.
- Records from apply/revert (no case context): `xts_emit_block_info(NULL, "smt:sibling", "info", value, unit, detail)`
  (status info, family/block/conditions of the block being run).
- Families ask `xts_cond_applied(ctx, "axis")` (1 applied, 0 default, -1 unknown) / `xts_cond_value(ctx, "axis")`
  instead of reading `ctx->cond->applied`. Axis helpers: `#include <xts/cond_util.h>`.
- `.applies_to(fam_name, kind)` optionally refuses families; `.order` sets apply order.
- The full condition vector (every registered axis, default value when not applied) is in every record.
- Known hardware fact: FPSCR[NI] cannot be set through the input image on Xenon (src/model/KNOWN.md).

## Adding model handlers (`src/model/<family>.c`)
```c
#include <xts/model.h>

static xts_mres_t m_subfx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  int sf = xm_sf(in);
  uint64_t a = in->gpr[XM_RA(w)], b = in->gpr[XM_RB(w)], r = b - a;
  out->gpr[XM_RD(w)] = r;                               /* out starts as a copy of the (normalised) input */
  if (XM_OE_BIT(w)) xm_set_ov(out, xm_add_ov(~a, b, r, sf));
  if (XM_RC_BIT(w)) xm_cr0(out, r, sf);                 /* after XER, so CR0[SO] is current */
  return XTS_M_OK;                                      /* or XTS_M_TRAP / ILLEGAL / ALIGNMENT / DSI */
}
XTS_MODEL("subfx", m_subfx);                            /* JSON mnemonic; one handler per mnemonic */
```
- One `XTS_MODEL(...)` per source line: the registration symbol is keyed on `__LINE__`, so a macro that expands to
  several registrations on one line collides.
- Hidden model state: the larx/stcx reservation lives in `src/model/ldst.c` (`xts_model_resv_clear/get/set`, declare
  them `extern`). Families testing `stwcx.`/`stdcx.` must establish it explicitly (see KNOWN.md "ldst").
- The dispatcher already handles: decoding, the expected-illegal set (`hardware.status == illegal`), unknown words
  (illegal), and normalising the input (`xm_state_loaded`). On any interrupt result the output is reset to the input.
- Memory: `xm_load(m, ea, n, &v)` / `xm_store(m, ea, n, v)` (big-endian, return `XTS_M_DSI` outside the regions);
  `xts_mem_ptr` for bulk access. Alignment rules are the family's (results4: FP loads/stores need EA%4==0, lwarx/stwcx
  4, ldarx/stdcx 8, dcbz/dcbz128 clear 128 bytes).
- FP: `xm_sf_begin(fpscr)` sets SoftFloat's rounding mode, `xm_sf_flags_to_fpscr()` maps its flags, `xm_fpscr_merge`
  applies sticky/FX/VX/FEX, `xm_fprf_f64` gives FPRF. SoftFloat specialization notes (NaN rules, FMA operand order,
  tininess): `third_party/softfloat-3e/XTS-PowerPC.md`.
- Validate with `make check-model` (results4: 10,776 cases). Fix or record every disagreement in `src/model/KNOWN.md`.
- `XTS_MODEL_VERSION` (xts.h, recorded in sysinfo.json) is bumped by the integrator; note semantic changes in KNOWN.md.

## Adding a profile (`profiles/<name>.c`)
```c
#include <xts/profile.h>

XTS_PROFILE(mine, "what it is for",
  { .families = "self.*" },                                   /* harness self-test first, always */
  { .families = "exec.*", .axes = "sf=*;fpscr_rn=rn,rz" },     /* "*" = every value */
  { .families = "lat.*", .kind = "timing", .repeats = 2, .seed = 7 },
);
```
`make PROFILE=mine`. Rows run in order; families in name order; axis combinations lexicographically; unknown axes are
logged and ignored, rows matching no family are fine. Row fields: `families kind axes repeats seed sweep max_cases`.
`.max_cases = N` caps each matching family in that row to an evenly strided sample (stride `ceil(n/N)`: cases 0, stride,
2*stride ... in enumeration order, so a cap still covers every instruction of an instruction-ordered family); case ids and
seeds are unchanged, only the block keys' chunk numbers refer to the sampled list.

## Host unit tests (`host/tests/<name>.c`)
```c
#include <xts/xts.h>
#include "htest.h"
HTEST(my_model_bits) { HT_EQ_U64(xts_enc(XI_addx, 0, 3, 4, 5), 0x7C642A14); HT_CHECK(1); }
```

## Record schema (results.jsonl / results.csv)
| Field | Meaning |
|---|---|
| `run_id` | the run nonce, 16 hex digits (the receiver's run directory name) |
| `seq` | record number in the run |
| `family`, `case_id` | family name and its stable case id |
| `mnem`, `word` | JSON mnemonic and 8-hex-digit instruction word (empty if none) |
| `cond` | object: every registered axis -> value (CSV: `k=v;k=v`) |
| `block` | key of the scheduler block (`r<row>:<family>:<applied axes or ->:c<chunk>`) |
| `metric`, `value`, `unit` | what was measured (`result`, `latency`, ...), number or null, unit |
| `n`, `min`, `median`, `mad`, `fit_r2` | statistics, null when not applicable |
| `status` | `pass fail trap skip noisy nonlinear regressed hang crash info`; combined with `xts_status_worst`: crash > hang > fail > trap > noisy > nonlinear > regressed > skip > info > pass |
| `detail` | family-defined object; semantic failures carry inputs, CPU result, model result and every differing item; `"truncated":true` is added if it did not fit |

The console sends only results.jsonl lines (REC messages); the receiver writes `results.csv` (same columns, detail
omitted), `failures.jsonl` (every `fail`, `hang` and `crash` record) and `summary.json` (status counts per family and per
`axis=value`, plus a `transport` object with the receiver's verdict).

Hang / crash records (emitted by the suite when the test core is lost, metric `result`, `case_id` = the case that was
running, `*` outside a case; the run stops right after them):
- `detail.reason`, `detail.case_from` (`breadcrumb (RAM, exact)`), `detail.phase`, `detail.case_index`, `detail.repeat`,
  `detail.last_word` (self/semantic families), `detail.report` (the CRASH/HANG message = crash.json on the PC),
  `detail.action`;
- crash: `detail.pir`, `vector`, `srr0`, `dar`, `lr` (from the test core's crash slot);
- hang: `detail.timeout_us`;
- other hardware threads (crash slots of PIR 1/3/5, checked at every block end; the run goes on): `case_id` `pir<N>`,
  status `crash`, in the block that was running (`detail.pir`, `vector`, `srr0/1`, `dar`, `lr`, `ctr`, `r1`, `r3`, `tb`).
- quarantined cases (quarantine.txt): status `skip`, `detail.reason` "quarantined (quarantine.txt) ...".

## Interfaces at a glance
- `xts/isa.h`: `xts_isa[]`, `XI_<mnem>` ids (`.` -> `_`), `xts_insn_t` (match/mask, form, isa, unit, priv, hw status,
  flags, operands with field/class/role, implicit reads/writes, JSON latency/issue), `xts_isa_find`,
  `xts_isa_baseline(insn, "key", &v)` (every numeric JSON timing value).
- `xts/enc.h`: `xts_enc(insn, flags, ops...)`, `xts_encode`, `xts_decode/decode1/decodes_as`, `xts_extract`,
  `xts_field_get/put/check`, `xts_flags_of`, `xts_disasm`.
- `xts/state.h`: `xts_state_t` (= the slot image), vector element helpers, big-endian memory helpers, baseline,
  `xts_state_compare`, `xts_mem_compare`.
- `xts/exec.h`: `xts_exec_t`, `xts_case_init`, setters, `xts_exec_word/kernel/again`, `xts_semantic_check`, big
  buffer + prep, `xts_exec_hooks`, `xts_exec_trap_range`, cache helpers.
- `xts/kernel.h` (+ `xk_loop_close`, `XTS_LOOP_CTR/GPR`), `xts/kgen.h` (`xk_li64/32/32s`, `xts_cond_applied/value`),
  `xts/timing.h` (+ the record emitter), `xts/model.h`, `xts/test.h` (+ `.finish`, `xts_results_foreach`, `xts_run_dir`),
  `xts/cond.h`, `xts/cond_util.h`, `xts/record.h` (+ `xts_status_worst`, vocabularies, `xts_emit_block_info`),
  `xts/json.h`, `xts/profile.h` (+ `.max_cases`), `xts/plat.h` (screen, timebase, SMC, identity; no storage; the
  watchdog hooks are no-ops), `xts/net.h` (transport, framework only), `xts/xcore.h` (core roles, `xts_xc_call`, crash
  slots, uncached memory).

## Timing records (`xts_time_emit_ex`, xts/timing.h)
One emitter for every timing record; families do not build timing records by hand.
```c
xts_time_emit_ex(ctx, &s, &r, XI_addx, &(xts_temit_t){
    .metric = "latency",            /* vocabulary below */
    .per_unit = 3,                  /* value = cycles per spec unit / 3 (3 instructions per unit) */
    .baseline_key = "*",            /* compare with the ISA JSON: latency->"latency", rthroughput/issue_cost->"issue" */
    .case_suffix = "chain",         /* case id "<case id>/chain" (or .case_id = "full id") */
    .word = w,                      /* instruction word of the record, 0 = none */
    .extra_json = "{\"producer\":\"addx\",\"consumer\":\"addx\"}",   /* members copied to the front of detail */
    .detail_fn = my_members, .detail_arg = p,                       /* or write members yourself */
    .floor = 1.0, .no_negative = 1, /* implausible pass / nonlinear values -> noisy */
    .status = XTS_ST_INFO });       /* combined with xts_status_worst (0 = no effect) */
xts_time_emit_ex(ctx, &s, &r, insn, &(xts_temit_t){ .metric = "penalty", XTS_TE_VALUE(p) });   /* value override */
xts_time_emit_diff(ctx, &sa, &ra, &sb, &rb, insn, &(xts_temit_t){ .metric = "extra" });      /* (a - b) / per_unit */
xts_time_emit_value(ctx, &s, &r /* or NULL, NULL */, insn, v, &(xts_temit_t){ .metric = "ratio", .unit = "ratio" });
```
Rules (the same for all three):
- `value` = `cycles_per_unit / per_unit`; `XTS_TE_VALUE(v)` / `xts_time_emit_value` replace it; `_diff` uses a - b.
- `min`, `median`, `mad` are per-unit cycle statistics of the deepest depth divided by `per_unit` and are filled ONLY when
  `value` is that figure (default, or an override equal to it); otherwise null. `n` = runs (a + b for diffs), `fit_r2` =
  the fit's r2 (the smaller one for diffs).
- `status` = the measurement's status (`fail` when `r.failed`: kernel build / setup / exec error, with `"error"`), diffs
  take the worse of both; then the baseline check (only from pass; `regressed` when off by more than `baseline_tol`,
  default 10 %); then `floor` / `no_negative` (pass or nonlinear -> noisy, `"implausible"`); then `xts_status_worst(status, e.status)`.
- `unit` defaults to `cycles`; `mnem` is `xts_isa[insn]` (`insn` -1: the case's).
- Detail = `{<extra_json members>, <detail_fn members>, "fit":{...}, ["ref":{...}], ["baseline":b, "baseline_key":k],
  ["implausible":why, "floor":f], ["error":why]}`.

Fit schema (`xts_time_fit_json(j, key, s, r, per_unit)` writes it; every key is always present, numbers null when the
measurement has none; families that keep several fits in one record add them with this function under their own keys):

| key | meaning |
|---|---|
| `unit`, `iters`, `status`, `why` | spec unit text, loop count, status of this fit, reason (only when non-empty) |
| `cpu` | cycles per spec unit = `slope_ticks * cycles_per_tick` (NOT divided by `per_unit`) |
| `slope_ticks`, `intercept_ticks`, `r2`, `max_resid_rel` | least-squares fit of ticks/iteration against depth |
| `spread`, `aba_shift`, `outliers` | largest MAD/median over the depths; first-depth A/B/A drift (min of 3 re-runs at the end vs min of its repeats; one unrecorded warm-up run precedes the series); repeats more than the spread limit above their median |
| `slope_se` | standard error of the slope in ticks (`sqrt(SSR/(n-2)/Sxx)`); `r.err_cycles` is the sched error bar |
| `depths`, `ticks_med`, `ticks_mad` | depth list; ticks/iteration median and MAD per depth |
| `cycles_per_tick`, `per_unit`, `kernel_words_max` | scale used, events per spec unit, largest kernel |

`xts_tresult_t` v2 fields: `slope_se`, `err_cycles` (`cycles_per_tick * sqrt(slope_se^2 + (mad_deep/depth_deep)^2)`, at
least 0.01: the sched_classes error bar), `failed`. `xts_time_cycles(&r, per)` = usable cycles per event or NaN.
`xts_time_emit(ctx, &s, &r, metric, insn)` (v1) = `xts_time_emit_ex` with `.metric = metric, .baseline_key = metric`.

### Loop frames
`.loop = XTS_LOOP_CTR` (default, the v1 frame word for word: `mtctr r29 ; isync ; mftb r27 ; <32-byte aligned top> body ;
bdnz top ; mftb r28`) or `.loop = XTS_LOOP_GPR` (`isync ; mftb r27 ; top: body ; addi r29,r29,-1 ; cmpwi crF,r29,0 ;
bne crF,top ; mftb r28`, `crF = .loop_crf` 1..7, default cr7): bodies that use CTR (mtctr/bctr/bdnz, CTR-saving units)
need no r26 save any more. Both close with a long-reach form when the body is beyond the bc range (`bdz/beq out ; b top`).
`xk_loop_close(k, top, mode, count_reg, crf)` is the same closure for hand-built loops.
`.setup2(x, depth, run, arg)` replaces `.setup` when set: `run` is the run index (0 .. ndepths*repeats; the warm-up run
uses 0 and the A/B/A re-runs all use ndepths*repeats; on the host the depth index).

## Vocabularies (checked in host builds)
- metric: `latency rthroughput issue_cost penalty extra overlap cycles ratio bytes_per_cycle depth count result estimate
  tininess info`
- unit: `cycles ratio count bytes bytes_per_cycle entries words hz ticks ""`

`xts_emit` checks both in host builds (dry runs, host tests): an unknown name is logged once and the record gets
`"vocab_warning":"..."` in its detail (it is still written). A family that really needs another name registers it once:
`XTS_METRIC("name");` / `XTS_UNIT("name");` (one per source line). `xts_metric_known` / `xts_unit_known` query them.

## Family finish()
`.finish = fn` runs as its own block `r<row>:<family>:finish` right after the family's last case block of each profile
row (after all axis combinations of the row), exactly once per row and run. finish() is not case execution: it may read
results back. `ctx->c` is a synthetic case (`id "finish"`, `insn -1`), `ctx->case_index` -1, `ctx->cond` all defaults,
and `ctx->fin` says what the row did: `blocks`, `ran_this_boot` (= the blocks run; there is no resume), `done_earlier`
(always 0), `quarantined` (blocks with a quarantine.txt skip), `skipped`, `complete`.
`xts_results_foreach("family", cb, ud)` passes every results.jsonl line of the family from the **in-RAM results store**
(`src/core/results.c`): lines of families with a `.finish` hook are always kept (64 MB bound; beyond it lines are
dropped and run.log says so); lines of other families are dropped once the bound is reached (they are still sent).
setup() runs first if it has not been called; finish is skipped if setup failed; teardown() runs after the last finish.
Set `case_id` on every finish record (`"summary"`, `"graph"`, ...).

### Reading results back: `xts/rline.h`
Do not scan `results.jsonl` lines with `strstr`; use the framework reader inside the `xts_results_foreach` callback
(the line and `l.detail` are valid only during the callback):
```c
static int cb(const char *line, void *ud) {
  xts_rline_t l;
  if (xts_rline_parse(line, &l) < 0 || xts_rline_row(&l) != row || strcmp(l.metric, "penalty")) return 0;
  double err = xts_rline_num(&l, "penalty_err");            // detail member (depth 1 only), NaN if absent/null
  char v[24]; xts_rline_str(&l, "verdict", v, sizeof v);    // detail string member (unescaped), 0 if absent
  char oc[16]; xts_rline_cond(&l, "other_cores", oc, sizeof oc);   // one axis value of the record's cond, NULL if absent
  int cs = xts_condtab_add(&tab, &l);                       // slot of the condition vector (zero `tab` first), -1 if full
  return 0;
}
```
`xts_rline_t` has `family case_id metric unit block status value (NaN = null) row cond condh detail`; `row` comes from
the block key `r<row>:...` (-1 if none) and is not filtered for you. The parser is order-independent and handles escapes
and nested detail objects. To write the summary under the condition vector it summarises:
`xts_emit_for_cond(ctx, tab.text[cs], &r)` (`xts_cond_from_text(base, text, &cv)` gives the vector itself).

## Estimate instructions
`xts_semantic_check` on fres/frsqrte/vrefp/vrsqrtefp/vexptefp/vlogefp (+128): when the model returns unimplemented (the
answer needs the hardware table), the CPU result is judged per lane (`"model":"estimate"`, `XTS_M_ESTIMATE`): lanes the
model knows exactly (special operands, probed with the other lanes at +inf) must match bit for bit, other lanes must be
within the architected bound (fres 2^-8, frsqrte 2^-5, vector 2^-12; `xf_estimate_ok`), exp2/log2 lanes without an exact
answer are `unknown`. pass = something checked and all within; fail = any lane out, or anything but the target (and
FPSCR / CR1 for the scalar forms) changed; info = nothing checkable. `xts_estimate_verdict(insn, word, in, out, j)` is the
check itself, `xts_estimate_kind(insn, &vector)` the classifier.

## Transport, crashes and quarantine
- **Sink** (`src/core/sink.c`): `write_record` builds the results.jsonl line and sends it as one REC message
  (`xts_core_send` -> `xts_net_send`; it also counts messages per type, the last frame seq, send failures and the
  FNV-1a `rec_hash` for END) and keeps it in the in-RAM results store. `xts_log` lines are LOG messages (before HELLO
  they wait in a 256 KB RAM buffer). `xts_sink_hello` sends HELLO x3, `xts_sink_block_end` a BLOCK, `xts_sink_end` END
  x3 and drains. Families never send messages themselves (exception: the `self.net` transport self-test, through
  `xts_core_send` so END's counts include it).
- **Wire format:** `docs/PROTOCOL.md`; `src/core/netfmt.{c,h}` is the one implementation of headers, CRC-32, fragments,
  the Ethernet/IPv4/UDP frame and the FNV-1a hash, shared by the console transmitter (`src/plat/net_console.c`), the
  host sender (`src/plat/net_host.c`) and the host tests; `tools/xts_recv.py` is the independent Python decoder.
- **Run driver** (`src/core/main.c`): HELLO -> blocks -> END. Before a case: the quarantine check, then (every case for
  self/semantic/test families, every `xts_crumb_every_timing`-th otherwise) a CRUMB message confirmed on the wire with
  `xts_net_wait_sent` before the case is released to the test core.
- **Test core lost:** the exec layer runs code on the test core with `xts_core_xc_call(fn, a, b)` (core.h), which
  uses the per-kind timeout (`xts_core_xc_timeout_us`: 2 s self/semantic/test, 10 s timing/sched/calib, 60 s mem/smt).
  On `XTS_XC_CRASHED` / `XTS_XC_TIMEOUT` it calls `xts_core_test_core_lost(reason)`, which never returns: crash/hang
  record (from `xts_crumb`) -> CRASH report (`xts_crash_report_json`: the PIR 0 crash slot, crumb, input registers, log
  tail) or HANG report (`xts_hang_report_json`: crumb, timeout, heartbeats) -> BLOCK -> END `crashed`/`hang` -> drain ->
  `xts_plat_halt`. `src/core/crash.c` holds only RAM state (crumb, the 50-line log ring) and these pure builders.
- **Quarantine:** `quarantine.txt` (repo root; `<family> <case_id>` or `<family> *` per line, `#` comments) is turned
  into `$(BUILD)/gen/quarantine.h` by `tools/gen_quarantine.py` and compiled into every ELF; listed cases are `skip`
  records. `tools/xts_recv.py --quarantine-out quarantine.txt` appends a suggested line per CRASH/HANG report. Host
  tests replace the list with `xts_quarantine_set`.
- **Host simulation:** `src/plat/xcore_host.c` runs `xts_xc_call` in-line; inside it `xts_host_xc_crash(vector, srr0,
  gpr)` / `xts_host_xc_hang()` (core.h, host only) make it return CRASHED / TIMEOUT, `xts_host_xc_set_slot(pir, c)` fills
  another PIR's crash slot. `xts_host_crash_sim` + `xts_plat_self_crash()` drive `self.crash` on the PC. Host tests
  run whole runs through the real receiver with `ht_recv_run` (`host/tests/htest.h`).

## Migration guide (Step 2: families onto the v2 API)
Keep case ids and metric names exactly as they are; detail keys move to the unified schema. Delete the local helper after
moving its callers.

| old (where) | new |
|---|---|
| tsys block `emit_meas(ctx, sub, metric, insn, hw, w, m, tm)` (sysops/calib/branch) | `xts_time_emit_ex(ctx, &m->s, &m->r, insn, &(xts_temit_t){ .metric, .per_unit = tm->per, .baseline_key = tm->baseline ? "*" : NULL, .word = hw ? w : 0, .case_suffix = sub, .detail_fn = <j_meta> })` |
| tsys `emit_diff(..., a, b, tm)` | `xts_time_emit_diff(ctx, &a->s, &a->r, &b->s, &b->r, insn, &e)` (a/b compact objects -> "fit"/"ref") |
| tsys `emit_val(..., value, status, m, tm, note)` | `xts_time_emit_value(ctx, m ? &m->s : NULL, m ? &m->r : NULL, insn, value, &(xts_temit_t){ .metric, .unit, .status = status, .detail_fn = <meta + note> })` |
| tsys `emit_failed`, `mstatus`, `mrun` rc | automatic: `xts_time_measure` sets `r.failed`, the emitter records fail + `"error"` |
| tsys `sev`/`worst`, tlat `worse`/`comb`, tmem `st_rank`/`mu_status`, `sc_status2` | `xts_status_worst(a, b)` |
| tsys `cyc(m)` | `xts_time_cycles(&m->r, 1)` |
| tsys `subid(ctx, "x")` | `.case_suffix = "x"` |
| `j_meta` (producer/consumer/variant/distance/insns_per_unit) | `.extra_json` or a `.detail_fn` writing the same members |
| `j_timing`, `j_compact`, tlat `tlat_json_fit`, tmem `mu_fit_json`, tsched `sc_json_meas` | the emitter's `"fit"`; extra fits: `xts_time_fit_json(j, "key", s, r, per)` inside a `detail_fn` |
| tlat `tlat_rec_stats(rec, sp, k, r)` + inline record | `.per_unit = k->per_unit` (statistics follow the rule above) |
| tmem `mu_emit(ctx, d, metric, insn, value, unit, s, r, r2, status, floor)` | `xts_time_emit_value(ctx, s, r, insn, value, &(xts_temit_t){ .metric, .unit, .floor = floor, .no_negative = 1, .status = status < 0 ? 0 : status, .extra_json = <d's members>, .detail_fn = <"ref" via xts_time_fit_json(j,"ref",s,r2,1)> })`; use `xts_time_emit_diff` when value is a - b |
| tsched `sc_emit(...)` (median = cycles even for penalties) | `xts_time_emit_ex` / `_value`; `m->err` -> `r.err_cycles`; slope SE -> `r.slope_se` (put `err_cycles` in the detail if the graph needs it) |
| tput_indep inline records + curve JSON | `xts_time_emit_ex` with `.per_unit`; the curve in a `detail_fn` |
| r26 `R_SAVE` CTR save/restore in bodies | `s.loop = XTS_LOOP_GPR` (CTR free; keep cr<loop_crf> out of the body) |
| `mu_li32` (drops bit 31), `xe_emit_const`, smt `emit_li`/`emit_addr` | `xk_li64` / `xk_li32` / `xk_li32s` |
| raw lwsync/ptesync words (`g_raw(0x7C2004AC)`) | `xk_emit(k, XI_lwsync, 0)`, `XI_ptesync` |
| `cv->applied` scans (`tlat_axis_nj`, `mu_axis_applied`) | `xts_cond_applied(ctx, "axis")`, `xts_cond_value(ctx, "axis")` |
| `#include "../conds/cond_util.h"` | `#include <xts/cond_util.h>` |
| `extern xts_model_resv_clear`, `extern xf_estimate_ok` | `#include <model/model_priv.h>` (exec families only) |
| exec_est own bound checks | `xts_estimate_verdict` / `XTS_M_ESTIMATE` (or keep exact-bit sweeps and use it for the verdict) |
| smt `current_block()` + `xts_sink_emit_core` (old crash-page parsing) | `xts_emit_block_info(NULL, "smt:<axis>", "info", value, unit, detail)` |
| smt `prev_hooks` save/restore of `xts_exec_hooks` | `xts_exec_hook_add` / `xts_exec_hook_remove` |
| `xts_exec_trap_range(0, ...)` from several axes | one index each (sibling 0, other_cores 1..3) |
| libxenon externs in smt_console.c | `#include <plat/plat_libxenon.h>` |
| end-of-family summaries done in the last `run()` | `.finish = fn` (+ `xts_results_foreach`) |
| `../core/core.h` from families/SMT | public API above; `<core/core.h>` is framework-only |

Include-rule exceptions (deliberate): `src/tests/self_crash.c` uses `<core/core.h>` (it drives the crash path on purpose), and `host/tests/t_rob_smoke.c` includes `profiles/*.c` to count a profile's records. `profiles/smoke.c` lists its `exec.*` families explicitly with `.max_cases` budgets (a single `exec.*` row would exceed the 500-record smoke budget), so a new exec family does not join `smoke` automatically - add a row.
