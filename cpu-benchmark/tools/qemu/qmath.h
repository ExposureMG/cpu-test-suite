/*
 * qmath.h - math routines of the qemu runner runtime (see qmath.c).
 *
 * In the PPC build qm_sqrt/qm_sqrtf are also exported as sqrt/sqrtf, the
 * names GCC calls when it cannot use a hardware square root (-mcpu=7400 has
 * no fsqrt).  With -DQLG_HOST (host unit test) only the qm_ names exist.
 */
#ifndef QMATH_H
#define QMATH_H

double qm_sqrt(double x);  /* correctly rounded (IEEE round to nearest) */
float qm_sqrtf(float x);   /* correctly rounded */

/* Deterministic stand-ins with "reasonable" accuracy (about 1e-15
 * relative for double over the usual ranges, and the float versions are
 * the double result rounded once).  Arguments of sin/cos beyond 2^30 are
 * reduced with less care. */
double qm_sin(double x);
double qm_cos(double x);
double qm_exp(double x);
double qm_log(double x);
float qm_sinf(float x);
float qm_cosf(float x);
float qm_expf(float x);
float qm_logf(float x);

#endif /* QMATH_H */
