/* Case lists (xts/test.h). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>

xts_case_t *xts_case_add(xts_caselist_t *cl, const char *id)
{
  if (cl->n >= cl->cap) {
    int nc = cl->cap ? cl->cap * 2 : 64;
    xts_case_t *nv = realloc(cl->v, sizeof *nv * nc);
    if (!nv) return NULL;
    cl->v = nv;
    cl->cap = nc;
  }
  xts_case_t *c = &cl->v[cl->n++];
  memset(c, 0, sizeof *c);
  c->insn = -1;
  snprintf(c->id, sizeof c->id, "%s", id ? id : "");
  return c;
}

xts_case_t *xts_case_addf(xts_caselist_t *cl, const char *fmt, ...)
{
  char id[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(id, sizeof id, fmt, ap);
  va_end(ap);
  return xts_case_add(cl, id);
}
