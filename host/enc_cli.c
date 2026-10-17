/* Host helper for tools/check_encoder.py and tools/check_model.py: drives the C encoder/decoder from text.
 *
 *   xts-enc-cli               reads lines "E <insn id> <flags> <op0> <op1> ..." -> "<8 hex digits>" or "ERR <code> <op>"
 *                             and lines   "D <hex word>"                        -> "<id> <id> ..." (all decode hits) or "-"
 *                             and lines   "X <hex word>"                        -> disassembly text
 *   xts-enc-cli --table       prints "<id> <mnem> <nops> <flags>" for every descriptor
 *   xts-enc-cli --aliases     prints "<id> <alias name>" for every encoding alias (XI_lwsync ...)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/enc.h>

int main(int argc, char **argv)
{
  if (argc > 1 && !strcmp(argv[1], "--aliases")) {
    for (int i = 0; i < xts_isa_alias_count; i++) printf("%d %s\n", xts_isa_count + i, xts_isa_aliases[i].name);
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "--table")) {
    for (int i = 0; i < xts_isa_count; i++)
      printf("%d %s %d %u\n", i, xts_isa[i].mnem, xts_isa[i].nops, xts_isa[i].flags);
    return 0;
  }
  char line[512];
  while (fgets(line, sizeof line, stdin)) {
    char *p = line;
    if (line[0] == 'E') {
      p++;
      int id = (int)strtol(p, &p, 10);
      unsigned flags = (unsigned)strtoul(p, &p, 10);
      int64_t ops[XTS_MAX_OPS] = { 0 };
      int n = 0;
      while (n < XTS_MAX_OPS) {
        char *q;
        long long v = strtoll(p, &q, 10);
        if (q == p) break;
        ops[n++] = v;
        p = q;
      }
      const xts_insn_t *d = xts_insn(id);
      uint32_t w = 0;
      int bad = -1, e = (!d || n != d->nops) ? XTS_ENC_NOPS : xts_encode(id, flags, ops, &w, &bad);
      if (e) printf("ERR %d %d\n", e, bad);
      else printf("%08X\n", w);
    } else if (line[0] == 'D') {
      uint32_t w = (uint32_t)strtoul(line + 1, NULL, 16);
      int ids[16], n = xts_decode(w, ids, 16);
      if (!n) printf("-\n");
      for (int i = 0; i < n && i < 16; i++) printf(i ? " %d" : "%d", ids[i]);
      if (n) printf("\n");
    } else if (line[0] == 'X') {
      char buf[128];
      printf("%s\n", xts_disasm((uint32_t)strtoul(line + 1, NULL, 16), buf, sizeof buf));
    }
    fflush(stdout);
  }
  return 0;
}
