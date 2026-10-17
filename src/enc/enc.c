/* Instruction encoder/decoder: C port of OpenXenos isa/hwtest/isaenc.py, table-driven (xts/isa.h, generated table). */
#include <stdio.h>
#include <string.h>
#include <xts/enc.h>

int xts_enc_last_error, xts_enc_last_bad_op;

static int flag_bit(const xts_insn_t *d, unsigned f)
{
  switch (f) {
  case XTS_RC:
    if (!strcmp(d->form_name, "VC")) return 10;
    if (!strcmp(d->form_name, "VX128_R")) return 6;
    return 0;
  case XTS_OE: return 10;
  case XTS_LK: return 0;
  case XTS_AA: return 1;
  case XTS_T: return 25;
  case XTS_A: return 25;
  }
  return -1;
}

static int64_t value_min(const xts_field_t *f)
{
  if (f->kind == XTS_FK_SIGNED || f->kind == XTS_FK_DISP4) return -((int64_t)1 << (f->bits - 1));
  return 0;
}

static int64_t value_max(const xts_field_t *f)
{
  if (f->kind == XTS_FK_SIGNED || f->kind == XTS_FK_DISP4) return ((int64_t)1 << (f->bits - 1)) - 1;
  return ((int64_t)1 << f->bits) - 1;
}

int xts_field_check(int field, int64_t v)
{
  if (field <= XTS_F_NONE || field >= XTS_F_COUNT) return 0;
  const xts_field_t *f = &xts_fields[field];
  if (v < value_min(f) || v > value_max(f)) return 0;
  if (f->kind == XTS_FK_DISP4 && (v & 3)) return 0;
  return 1;
}

uint32_t xts_field_put(int field, int64_t v, uint32_t word)
{
  const xts_field_t *f = &xts_fields[field];
  uint64_t u = (uint64_t)v;
  if (f->kind == XTS_FK_SWAP10) {
    u &= 0x3FF;
    u = ((u & 31) << 5) | (u >> 5);
  } else if (f->kind == XTS_FK_ROT6) {
    u &= 63;
    u = ((u & 31) << 1) | ((u >> 5) & 1);
  }
  for (int i = 0; i < f->nparts; i++) {
    const xts_field_part_t *p = &f->part[i];
    uint32_t m = (p->width >= 32) ? 0xFFFFFFFFu : ((1u << p->width) - 1);
    if (f->kind == XTS_FK_ROT6 || f->kind == XTS_FK_SWAP10)
      word = (word & ~(m << p->shift)) | (((uint32_t)u & m) << p->shift);
    else
      word = (word & ~(m << p->shift)) | ((uint32_t)(u >> p->dest) & m) << p->shift;
  }
  return word;
}

int64_t xts_field_get(int field, uint32_t word)
{
  if (field <= XTS_F_NONE || field >= XTS_F_COUNT) return 0;
  const xts_field_t *f = &xts_fields[field];
  uint64_t u = 0;
  for (int i = 0; i < f->nparts; i++) {
    const xts_field_part_t *p = &f->part[i];
    uint32_t m = (p->width >= 32) ? 0xFFFFFFFFu : ((1u << p->width) - 1);
    if (f->kind == XTS_FK_ROT6 || f->kind == XTS_FK_SWAP10)
      u |= (uint64_t)((word >> p->shift) & m);
    else
      u |= (uint64_t)((word >> p->shift) & m) << p->dest;
  }
  if (f->kind == XTS_FK_SWAP10) return (int64_t)(((u & 31) << 5) | (u >> 5));
  if (f->kind == XTS_FK_ROT6) return (int64_t)(((u & 1) << 5) | (u >> 1));
  if (f->kind == XTS_FK_SIGNED || f->kind == XTS_FK_DISP4) {
    uint64_t sign = (uint64_t)1 << (f->bits - 1);
    return (int64_t)((u ^ sign) - sign);
  }
  return (int64_t)u;
}

static const xts_insn_alias_t *alias_of(int id)
{
  return (id >= xts_isa_count && id < xts_isa_count + xts_isa_alias_count) ? &xts_isa_aliases[id - xts_isa_count] : NULL;
}

int xts_encode(int insn, unsigned flags, const int64_t *ops, uint32_t *word, int *bad_op)
{
  if (bad_op) *bad_op = -1;
  const xts_insn_alias_t *al = alias_of(insn);
  if (al) {
    int e = xts_encode(al->base, flags, ops, word, bad_op);
    if (!e) *word = (*word & ~al->fix_mask) | al->set_bits;
    return e;
  }
  if (insn < 0 || insn >= xts_isa_count) return XTS_ENC_BAD_INSN;
  const xts_insn_t *d = &xts_isa[insn];
  if (flags & ~(unsigned)d->flags) return XTS_ENC_BAD_FLAGS;
  uint32_t w = d->match | d->extra_match;
  int vd = -1;
  for (int i = 0; i < d->nops; i++)
    if (d->ops[i].field == XTS_F_VD128) vd = i;
  for (int i = 0; i < d->nops; i++) {
    const xts_operand_t *o = &d->ops[i];
    if (!xts_field_check(o->field, ops[i])) { if (bad_op) *bad_op = i; return XTS_ENC_RANGE; }
    if (o->field == XTS_F_VS128 && vd >= 0 && ops[i] != ops[vd]) { if (bad_op) *bad_op = i; return XTS_ENC_TIED; }
    w = xts_field_put(o->field, ops[i], w);
  }
  for (unsigned f = 1; f <= XTS_A; f <<= 1)
    if (flags & f) w |= 1u << flag_bit(d, f);
  *word = w;
  return XTS_ENC_OK;
}

uint32_t xts_enc_a(int insn, unsigned flags, const int64_t *ops, int nops)
{
  xts_enc_last_bad_op = -1;
  const xts_insn_t *d = xts_insn(insn);
  if (!d) { xts_enc_last_error = XTS_ENC_BAD_INSN; return 0; }
  if (nops != d->nops) { xts_enc_last_error = XTS_ENC_NOPS; return 0; }
  uint32_t w = 0;
  int e = xts_encode(insn, flags, ops, &w, &xts_enc_last_bad_op);
  xts_enc_last_error = e;
  return e ? 0 : w;
}

const char *xts_enc_strerror(int err)
{
  switch (err) {
  case XTS_ENC_OK: return "ok";
  case XTS_ENC_BAD_INSN: return "bad instruction id";
  case XTS_ENC_BAD_FLAGS: return "flag not accepted by this instruction";
  case XTS_ENC_RANGE: return "operand out of range";
  case XTS_ENC_TIED: return "tied operand differs (VS128 must equal VD128)";
  case XTS_ENC_NOPS: return "wrong operand count";
  }
  return "?";
}

int xts_decode(uint32_t word, int *ids, int max)
{
  int n = 0;
  for (int i = 0; i < xts_isa_count; i++)
    if ((word & xts_isa[i].mask) == xts_isa[i].match) {
      if (n < max && ids) ids[n] = i;
      n++;
    }
  return n;
}

int xts_decode1(uint32_t word)
{
  int id;
  return xts_decode(word, &id, 1) ? id : -1;
}

int xts_decodes_as(uint32_t word, int insn)
{
  const xts_insn_alias_t *al = alias_of(insn);
  if (al) return xts_decodes_as(word, al->base) && (word & al->fix_mask) == al->set_bits;
  if (insn < 0 || insn >= xts_isa_count) return 0;
  return (word & xts_isa[insn].mask) == xts_isa[insn].match;
}

int64_t xts_extract(int insn, uint32_t word, int opidx)
{
  insn = xts_insn_canonical(insn);
  if (insn < 0 || insn >= xts_isa_count || opidx < 0 || opidx >= xts_isa[insn].nops) return 0;
  return xts_field_get(xts_isa[insn].ops[opidx].field, word);
}

unsigned xts_flags_of(int insn, uint32_t word)
{
  insn = xts_insn_canonical(insn);
  if (insn < 0 || insn >= xts_isa_count) return 0;
  const xts_insn_t *d = &xts_isa[insn];
  unsigned out = 0;
  for (unsigned f = 1; f <= XTS_A; f <<= 1)
    if ((d->flags & f) && (word >> flag_bit(d, f) & 1)) out |= f;
  return out;
}

char *xts_format(int insn, uint32_t word, char *buf, int size)
{
  if (size <= 0) return buf;
  buf[0] = 0;
  const xts_insn_alias_t *al = alias_of(insn);
  if (al) {                                   /* "<alias><operands of the base>" */
    char tmp[128];
    xts_format(al->base, word, tmp, sizeof tmp);
    const char *rest = tmp + strcspn(tmp, " ");
    snprintf(buf, size, "%s%s", al->name, rest);
    return buf;
  }
  if (insn < 0 || insn >= xts_isa_count) { snprintf(buf, size, ".long 0x%08X", word); return buf; }
  const xts_insn_t *d = &xts_isa[insn];
  unsigned fl = xts_flags_of(insn, word);
  int o = 0;
  for (const char *t = d->asm_template; *t && o < size - 1;) {
    if (*t != '[') { buf[o++] = *t++; continue; }
    const char *e = strchr(t, ']');
    if (!e) break;
    char name[16];
    int nl = (int)(e - t - 1);
    if (nl >= (int)sizeof name) nl = sizeof name - 1;
    memcpy(name, t + 1, nl);
    name[nl] = 0;
    t = e + 1;
    const char *rep = NULL;
    char tmp[40];
    if (!strcmp(name, "OE")) rep = (fl & XTS_OE) ? "o" : "";
    else if (!strcmp(name, "Rc")) rep = (fl & XTS_RC) ? "." : "";
    else if (!strcmp(name, "T")) rep = (fl & XTS_T) ? "t" : "";
    else if (!strcmp(name, "LK")) rep = (fl & XTS_LK) ? "l" : "";
    else if (!strcmp(name, "AA")) rep = (fl & XTS_AA) ? "a" : "";
    else if (!strcmp(name, "A")) rep = (fl & XTS_A) ? "all" : "";
    else {
      int idx = xts_op_index(d, name);
      if (idx < 0) rep = "?";
      else {
        int64_t v = xts_field_get(d->ops[idx].field, word);
        if (d->ops[idx].cls == XTS_OC_BRANCH && !(fl & XTS_AA))
          snprintf(tmp, sizeof tmp, v >= 0 ? ".+%lld" : ".%lld", (long long)v);
        else
          snprintf(tmp, sizeof tmp, "%lld", (long long)v);
        rep = tmp;
      }
    }
    while (*rep && o < size - 1) buf[o++] = *rep++;
  }
  buf[o] = 0;
  return buf;
}

char *xts_disasm(uint32_t word, char *buf, int size)
{
  int id = xts_decode1(word);
  for (int i = 0; id >= 0 && i < xts_isa_alias_count; i++)
    if (xts_isa_aliases[i].base == id && (word & xts_isa_aliases[i].fix_mask) == xts_isa_aliases[i].set_bits) {
      id = xts_isa_count + i;
      break;
    }
  return xts_format(id, word, buf, size);
}
