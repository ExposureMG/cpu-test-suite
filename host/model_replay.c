/* Host replay of recorded hardware cases through the reference model (driven by tools/check_model.py).
 *
 *   xts-model-replay CASES.txt
 *
 * Case file (written by check_model.py from OpenXenos results4 + the regenerated plan inputs):
 *   SCRATCH <hex>                    address the scratch memory had on the console
 *   C <idx> <mnem> <word>            start of a case; input = canary baseline, then:
 *   I gpr|fpr|vr|mem <n> <hex>       input register / 16-byte scratch block n
 *   I cr|xer|fpscr|vscr|lr|ctr|mode <hex>
 *   P <reg> <off>                    gpr <reg> = SCRATCH + off
 *   T <srr1 hex>                     the console took an interrupt (state unchanged)
 *   O gpr|fpr|vr|mem <n> <hex>       an output item that differed from the input (only changes were recorded)
 *   O cr|xer|fpscr|vscr|lr|ctr <hex>
 *   E                                end of case
 * (I and P lines come before T, O and E.) Output, one line per case:
 *   R <idx> <mnem> match|mismatch|unimpl|badinput <model result> [<what>:model=<hex>,hw=<hex> ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/enc.h>
#include <xts/model.h>
#include <xts/state.h>

static void put_vr(xts_vr_t *v, const char *hex)
{
  for (int i = 0; i < 4; i++) {
    char w[9];
    memcpy(w, hex + 8 * i, 8);
    w[8] = 0;
    v->w[i] = (uint32_t)strtoul(w, NULL, 16);
  }
}

static int put_mem(uint8_t *mem, int block, const char *hex)
{
  if (block < 0 || block >= XTS_SCRATCH_BYTES / 16 || strlen(hex) < 32) return -1;
  for (int i = 0; i < 16; i++) {
    char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
    mem[16 * block + i] = (uint8_t)strtoul(b, NULL, 16);
  }
  return 0;
}

static int apply(xts_state_t *s, uint8_t *mem, const char *kind, const char *a, const char *b)
{
  if (!strcmp(kind, "gpr")) s->gpr[atoi(a) & 31] = strtoull(b, NULL, 16);
  else if (!strcmp(kind, "fpr")) s->fpr[atoi(a) & 31] = strtoull(b, NULL, 16);
  else if (!strcmp(kind, "vr")) put_vr(&s->vr[atoi(a) & 127], b);
  else if (!strcmp(kind, "mem")) return put_mem(mem, atoi(a), b);
  else if (!strcmp(kind, "cr")) s->cr = strtoull(a, NULL, 16);
  else if (!strcmp(kind, "xer")) s->xer = strtoull(a, NULL, 16);
  else if (!strcmp(kind, "fpscr")) s->fpscr = strtoull(a, NULL, 16);
  else if (!strcmp(kind, "vscr")) put_vr(&s->vscr, a);
  else if (!strcmp(kind, "lr")) s->lr = strtoull(a, NULL, 16);
  else if (!strcmp(kind, "ctr")) s->ctr = strtoull(a, NULL, 16);
  else if (!strcmp(kind, "mode")) s->mode = strtoull(a, NULL, 16);
  else return -1;
  return 0;
}

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: %s CASES.txt\n", argv[0]); return 2; }
  FILE *f = fopen(argv[1], "r");
  if (!f) { perror(argv[1]); return 2; }
  static char line[512];
  static xts_state_t in, exp, out;
  static uint8_t mem_in[XTS_SCRATCH_BYTES], mem_exp[XTS_SCRATCH_BYTES], mem_out[XTS_SCRATCH_BYTES];
  static xts_state_diff_t d[32];
  uint64_t scratch_addr = 0, trap_srr1 = 0;
  char mnem[32] = "";
  long idx = -1;
  uint32_t word = 0;
  int trapped = 0, bad_input = 0, exp_ready = 0, in_case = 0;
  while (fgets(line, sizeof line, f)) {
    char t[8] = "", k[40] = "", a[80] = "", b[80] = "";
    if (sscanf(line, "%7s %39s %79s %79s", t, k, a, b) < 1) continue;
    if (!strcmp(t, "SCRATCH")) { scratch_addr = strtoull(k, NULL, 16); continue; }
    if (!strcmp(t, "C")) {
      idx = atol(k);
      snprintf(mnem, sizeof mnem, "%.31s", a);
      word = (uint32_t)strtoul(b, NULL, 16);
      xts_state_baseline(&in);
      xts_scratch_baseline(mem_in, XTS_SCRATCH_BYTES);
      trapped = bad_input = exp_ready = 0;
      trap_srr1 = 0;
      in_case = 1;
      continue;
    }
    if (!in_case) continue;
    if (!strcmp(t, "I")) { if (apply(&in, mem_in, k, a, b) < 0) bad_input = 1; continue; }
    if (!strcmp(t, "P")) { in.gpr[atoi(k) & 31] = scratch_addr + strtoull(a, NULL, 10); continue; }
    if (!exp_ready) {
      exp = in;
      memcpy(mem_exp, mem_in, sizeof mem_exp);
      exp_ready = 1;
    }
    if (!strcmp(t, "T")) { trapped = 1; trap_srr1 = strtoull(k, NULL, 16); continue; }
    if (!strcmp(t, "O")) { if (apply(&exp, mem_exp, k, a, b) < 0) bad_input = 1; continue; }
    if (strcmp(t, "E")) continue;
    in_case = 0;
    if (bad_input) { printf("R %ld %s badinput -\n", idx, mnem); continue; }

    xts_mem_t m;
    xts_mem_init(&m);
    memcpy(mem_out, mem_in, sizeof mem_out);
    xts_mem_add(&m, scratch_addr, mem_out, XTS_SCRATCH_BYTES);
    xts_mres_t r = xts_model_exec(NULL, word, &in, &m, &out);
    if (r == XTS_M_UNIMPLEMENTED) { printf("R %ld %s unimpl %s\n", idx, mnem, xts_mres_name(r)); continue; }
    char msg[1024] = "";
    int o = 0, bad = 0;
    if (trapped) {
      uint32_t cause = (uint32_t)(trap_srr1 >> 16) & 0xFF;
      xts_mres_t want = cause == 0x02 ? XTS_M_TRAP : cause == 0x08 ? XTS_M_ILLEGAL : cause == 0x04 ? XTS_M_PRIVILEGED
                      : XTS_M_ALIGNMENT;   /* v1 runner did not record the vector; no cause bits = alignment (0x600) */
      if (r != want) {
        bad = 1;
        o += snprintf(msg + o, sizeof msg - o, " interrupt:model=%s,hw=%s(srr1=%016llX)", xts_mres_name(r),
                      xts_mres_name(want), (unsigned long long)trap_srr1);
      }
    } else if (r != XTS_M_OK) {
      bad = 1;
      o += snprintf(msg + o, sizeof msg - o, " interrupt:model=%s,hw=none", xts_mres_name(r));
    }
    int n = xts_state_compare(&out, &exp, d, 32);
    int nm = xts_mem_compare(mem_out, mem_exp, XTS_SCRATCH_BYTES, d + (n < 32 ? n : 32), 32 - (n < 32 ? n : 32));
    for (int i = 0; i < n + nm && i < 32; i++) {
      int wide = !strncmp(d[i].what, "vr", 2) || !strncmp(d[i].what, "mem", 3);
      if (wide)
        o += snprintf(msg + o, sizeof msg - o > 0 ? sizeof msg - o : 0, " %s:model=%016llX%016llX,hw=%016llX%016llX", d[i].what,
                      (unsigned long long)d[i].a[0], (unsigned long long)d[i].a[1], (unsigned long long)d[i].b[0],
                      (unsigned long long)d[i].b[1]);
      else
        o += snprintf(msg + o, sizeof msg - o > 0 ? sizeof msg - o : 0, " %s:model=%016llX,hw=%016llX", d[i].what,
                      (unsigned long long)d[i].a[0], (unsigned long long)d[i].b[0]);
      if (o >= (int)sizeof msg) break;
    }
    if (n + nm) bad = 1;
    printf("R %ld %s %s %s%s\n", idx, mnem, bad ? "mismatch" : "match", xts_mres_name(r), msg);
  }
  fclose(f);
  return 0;
}
