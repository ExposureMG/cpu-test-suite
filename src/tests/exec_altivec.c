/* exec.altivec - the 137 AltiVec instructions the hardware executes plus the 8 Cell VMX additions (lvlx lvrx stvlx stvrx and
 * their LRU forms), v0..v31, CPU vs reference model.
 *
 * Values are NJ / SAT sensitive: float lanes come from FP32 + denormals of both signs + conversion boundaries + NaN payloads,
 * integer lanes from saturation edges (0x7FFF/0x8000 pairs, 0x7F/0x80 bytes, splats); 30 % of the cases start with VSCR[SAT]
 * set (sticky behaviour); NJ is swept by the vscr_nj axis. Small immediates are enumerated exhaustively (vsplt* element
 * index, vspltis*, vsldoi SHB, vcfsx / vctsxs scale, dst STRM), crossed with the flag variants (Rc, T, ALL). Memory forms
 * (lvx lvebx lvehx lvewx lvlx lvrx lvsl lvsr, stvx ..., dst*) use a pointer to scratch+128 and an index from MEM_OFFSETS,
 * so every EA alignment (0..15) is covered. Register aliasing (VD = VA = VB etc.) is drawn 25 % of the time. */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include "exec_common.h"

static int select_av(const xts_insn_t *d)
{
  return (d->isa == XTS_ISA_ALTIVEC || d->isa == XTS_ISA_CELLVMX) && d->priv == XTS_PRIV_USER && d->hw == XTS_HW_EXECUTES;
}
static int av_enumerate(xts_caselist_t *cl) { return xe_vec_enumerate(cl, select_av, 1, NULL); }

XTS_FAMILY(exec_altivec, .kind = "semantic", .axes = "sf,vscr_nj", .enumerate = av_enumerate, .run = xe_vec_run,
           .block_cases = 48, .desc = "AltiVec + Cell VMX instructions vs the reference model (NJ / SAT sensitive values)");
