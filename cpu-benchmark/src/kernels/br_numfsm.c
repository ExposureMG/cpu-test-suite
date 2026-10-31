/*
 * br_numfsm.c - CoreMark-style number scanner state machine (BR).
 *
 * 32 KB of text made of tokens separated by ',' or ' ' (sometimes ", ", which
 * leaves an empty token that is skipped): unsigned and signed integers,
 * decimals ("3.25", ".5", "-0.75", "7."), scientific ("1.5e-3", "6E+10"),
 * hex ("0x1F", "0Xbeef") and malformed tokens ("1.2.3", "0x", "--4", "1e+",
 * "12q4", "nan").  run() scans the buffer one character at a time through a
 * switch (state) machine with explicit character range tests (no ctype.h),
 * counts the tokens of each class and the transitions into each state, and
 * folds the counters.  The counters live in the arena and are cleared at the
 * start of run().
 */
#include "bench_kernel.h"

#define FSM_BYTES 32768u
#define FSM_TOKMAX 40u /* longest token init generates, plus slack */

enum fsm_state {
	ST_START,    /* nothing consumed yet                       */
	ST_SIGN,     /* "+" / "-"                                  */
	ST_ZERO,     /* "0": may still become hex                  */
	ST_INT,      /* unsigned digits                            */
	ST_SINT,     /* sign, digits                               */
	ST_DOT,      /* "." with no digit yet (".", "-.")          */
	ST_FRAC,     /* digits "." [digits], or "." digits         */
	ST_EXP_E,    /* mantissa "e"                               */
	ST_EXP_SIGN, /* mantissa "e" sign                          */
	ST_EXP,      /* exponent digits                            */
	ST_HEX_X,    /* "0x"                                       */
	ST_HEX,      /* "0x" hex digits                            */
	ST_INVALID,  /* malformed: skip to the next separator      */
	ST_COUNT
};

enum fsm_class { TK_INT, TK_SINT, TK_DEC, TK_SCI, TK_HEX, TK_BAD, TK_COUNT };

/* Token class of each final state. */
static const uint8_t fsm_class_of[ST_COUNT] = {
	[ST_START] = TK_BAD,    [ST_SIGN] = TK_BAD,     [ST_ZERO] = TK_INT,
	[ST_INT] = TK_INT,      [ST_SINT] = TK_SINT,    [ST_DOT] = TK_BAD,
	[ST_FRAC] = TK_DEC,     [ST_EXP_E] = TK_BAD,    [ST_EXP_SIGN] = TK_BAD,
	[ST_EXP] = TK_SCI,      [ST_HEX_X] = TK_BAD,    [ST_HEX] = TK_HEX,
	[ST_INVALID] = TK_BAD,
};

#define TXT_OFF     0u
#define CNT_OFF     BENCH_ALIGN_UP(TXT_OFF + FSM_BYTES, BENCH_LINE)
#define CNT_WORDS   32u /* class counts at [0..8), transition counts at [8..24) */
#define ARENA_BYTES BENCH_ALIGN_UP(CNT_OFF + CNT_WORDS * 4u, BENCH_LINE)

static inline int is_digit(uint32_t c)
{
	return c >= '0' && c <= '9';
}

static inline int is_hex_digit(uint32_t c)
{
	return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline int is_exp_mark(uint32_t c)
{
	return c == 'e' || c == 'E';
}

static inline int is_sign(uint32_t c)
{
	return c == '+' || c == '-';
}

static double numfsm_run(void *arena)
{
	const uint8_t *txt = (const uint8_t *)bench_at(arena, TXT_OFF);
	uint32_t *cls = (uint32_t *)bench_at(arena, CNT_OFF);
	uint32_t *trans = cls + 8;
	uint32_t state = ST_START, next, h, i;

	bench_fill(cls, 0, CNT_WORDS * 4u);

	for (i = 0; i < FSM_BYTES; i++) {
		uint32_t c = txt[i];

		if (c == ',' || c == ' ') {
			if (state != ST_START) {
				cls[fsm_class_of[state]]++;
				state = ST_START;
			}
			continue;
		}

		switch (state) {
		case ST_START:
			if (c == '0')
				next = ST_ZERO;
			else if (is_digit(c))
				next = ST_INT;
			else if (is_sign(c))
				next = ST_SIGN;
			else if (c == '.')
				next = ST_DOT;
			else
				next = ST_INVALID;
			break;
		case ST_SIGN:
			if (is_digit(c))
				next = ST_SINT;
			else if (c == '.')
				next = ST_DOT;
			else
				next = ST_INVALID;
			break;
		case ST_ZERO:
			if (c == 'x' || c == 'X')
				next = ST_HEX_X;
			else if (is_digit(c))
				next = ST_INT;
			else if (c == '.')
				next = ST_FRAC;
			else if (is_exp_mark(c))
				next = ST_EXP_E;
			else
				next = ST_INVALID;
			break;
		case ST_INT:
		case ST_SINT:
			if (is_digit(c))
				next = state;
			else if (c == '.')
				next = ST_FRAC;
			else if (is_exp_mark(c))
				next = ST_EXP_E;
			else
				next = ST_INVALID;
			break;
		case ST_DOT:
			next = is_digit(c) ? ST_FRAC : ST_INVALID;
			break;
		case ST_FRAC:
			if (is_digit(c))
				next = ST_FRAC;
			else if (is_exp_mark(c))
				next = ST_EXP_E;
			else
				next = ST_INVALID;
			break;
		case ST_EXP_E:
			if (is_sign(c))
				next = ST_EXP_SIGN;
			else if (is_digit(c))
				next = ST_EXP;
			else
				next = ST_INVALID;
			break;
		case ST_EXP_SIGN:
		case ST_EXP:
			next = is_digit(c) ? ST_EXP : ST_INVALID;
			break;
		case ST_HEX_X:
		case ST_HEX:
			next = is_hex_digit(c) ? ST_HEX : ST_INVALID;
			break;
		default:
			next = ST_INVALID;
			break;
		}

		if (next != state) {
			trans[next]++;
			state = next;
		}
	}
	if (state != ST_START)
		cls[fsm_class_of[state]]++;

	h = 0;
	for (i = 0; i < 8u + ST_COUNT; i++)
		h = bench_mix(h, cls[i]);
	return bench_result_u32(h);
}

/* ---- input generation -------------------------------------------------- *
 * init is freestanding code too: stop GCC from turning its byte copy, shift
 * and pad loops into memcpy / memmove / memset calls (the same option
 * bench_copy and bench_fill use).  init is not timed. */
#define INIT_FN __attribute__((optimize("no-tree-loop-distribute-patterns")))

INIT_FN static uint32_t gen_digits(bench_rng *r, uint8_t *t, uint32_t n, int nonzero_first)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (i == 0 && nonzero_first)
			t[i] = (uint8_t)('1' + bench_rng_below(r, 9));
		else
			t[i] = (uint8_t)('0' + bench_rng_below(r, 10));
	}
	return n;
}

INIT_FN static uint32_t gen_sign(bench_rng *r, uint8_t *t)
{
	t[0] = (bench_rng_next(r) & 1u) ? '-' : '+';
	return 1;
}

/* A well-formed token of class cls (TK_INT..TK_HEX); returns its length. */
INIT_FN static uint32_t gen_valid(bench_rng *r, uint8_t *t, uint32_t cls)
{
	static const char hexd[] = "0123456789abcdefABCDEF";
	uint32_t n = 0, i, form;

	switch (cls) {
	case TK_INT:
		if (bench_rng_below(r, 8) == 0)
			n = gen_digits(r, t, 1 + bench_rng_below(r, 3), 0); /* "0", "07" */
		else
			n = gen_digits(r, t, 1 + bench_rng_below(r, 9), 1);
		break;
	case TK_SINT:
		n = gen_sign(r, t);
		n += gen_digits(r, t + n, 1 + bench_rng_below(r, 6), 1);
		break;
	case TK_DEC:
		if (bench_rng_below(r, 3) == 0)
			n = gen_sign(r, t);
		form = bench_rng_below(r, 8);
		if (form == 0) {         /* ".5" */
			t[n++] = '.';
			n += gen_digits(r, t + n, 1 + bench_rng_below(r, 4), 0);
		} else if (form == 1) {  /* "7." */
			n += gen_digits(r, t + n, 1 + bench_rng_below(r, 3), 1);
			t[n++] = '.';
		} else {                 /* "3.25", "0.75" */
			n += gen_digits(r, t + n, 1 + bench_rng_below(r, 4), form != 2);
			t[n++] = '.';
			n += gen_digits(r, t + n, 1 + bench_rng_below(r, 6), 0);
		}
		break;
	case TK_SCI:
		if (bench_rng_below(r, 4) == 0)
			n = gen_sign(r, t);
		n += gen_digits(r, t + n, 1 + bench_rng_below(r, 2), 1);
		if (bench_rng_next(r) & 1u) {
			t[n++] = '.';
			n += gen_digits(r, t + n, 1 + bench_rng_below(r, 5), 0);
		}
		t[n++] = (bench_rng_next(r) & 1u) ? 'e' : 'E';
		if (bench_rng_below(r, 3) != 0)
			n += gen_sign(r, t + n);
		n += gen_digits(r, t + n, 1 + bench_rng_below(r, 3), 0);
		break;
	default: /* TK_HEX */
		t[n++] = '0';
		t[n++] = (bench_rng_below(r, 4) == 0) ? 'X' : 'x';
		for (i = 1 + bench_rng_below(r, 8); i > 0; i--)
			t[n++] = (uint8_t)hexd[bench_rng_below(r, sizeof hexd - 1)];
		break;
	}
	return n;
}

/* A malformed token; returns its length. */
INIT_FN static uint32_t gen_bad(bench_rng *r, uint8_t *t)
{
	static const char junk[] = "ghijklmnopqrstuvwGHQZ#_:$!";
	static const char frag[][6] = {
		"0x", "0X", "1e", "2.5e", "1e+", "7E-", "-", "+", ".", "-.",
		"1.2.3", "--4", "+-3", "nan", "inf", "x1F", "0xg1", "e5",
		".e2", "+0x10", "00x7", "4..2",
	};
	uint32_t n, pos, i, kind = bench_rng_below(r, 3);

	if (kind == 0) {
		/* A fixed malformed fragment. */
		const char *f = frag[bench_rng_below(r, sizeof frag / sizeof frag[0])];

		for (n = 0; f[n]; n++)
			t[n] = (uint8_t)f[n];
		return n;
	}
	/* A valid token with a character that is invalid in every state
	 * inserted at (kind 1) or written over (kind 2) a random position. */
	n = gen_valid(r, t, bench_rng_below(r, TK_BAD));
	pos = bench_rng_below(r, n + 1);
	if (kind == 1) {
		for (i = n; i > pos; i--)
			t[i] = t[i - 1];
		n++;
	} else if (pos == n) {
		pos = n - 1;
	}
	t[pos] = (uint8_t)junk[bench_rng_below(r, sizeof junk - 1)];
	return n;
}

INIT_FN static void numfsm_init(void *arena)
{
	uint8_t *txt = (uint8_t *)bench_at(arena, TXT_OFF);
	uint8_t tok[FSM_TOKMAX];
	bench_rng rng;
	uint32_t pos = 0, n, i, cls, sep;

	bench_rng_seed(&rng, 0xF5A11E55u);
	while (pos + FSM_TOKMAX + 2u <= FSM_BYTES) {
		/* Class mix: INT 25%, SINT 15%, DEC 20%, SCI 15%, HEX 10%, BAD 15%. */
		cls = bench_rng_below(&rng, 20);
		if (cls < 5)
			n = gen_valid(&rng, tok, TK_INT);
		else if (cls < 8)
			n = gen_valid(&rng, tok, TK_SINT);
		else if (cls < 12)
			n = gen_valid(&rng, tok, TK_DEC);
		else if (cls < 15)
			n = gen_valid(&rng, tok, TK_SCI);
		else if (cls < 17)
			n = gen_valid(&rng, tok, TK_HEX);
		else
			n = gen_bad(&rng, tok);
		for (i = 0; i < n; i++)
			txt[pos++] = tok[i];

		sep = bench_rng_below(&rng, 10);
		if (sep < 5) {
			txt[pos++] = ',';
		} else if (sep < 9) {
			txt[pos++] = ' ';
		} else {
			txt[pos++] = ',';
			txt[pos++] = ' ';
		}
	}
	while (pos < FSM_BYTES)
		txt[pos++] = ' ';
}

BENCH_KDEF(numfsm) = {
	.name = "numfsm", .desc = "number lexer FSM, 32 KB text", .unit = "B",
	.category = BENCH_CAT_BR, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = FSM_BYTES, .arena_bytes = ARENA_BYTES,
	.init = numfsm_init, .run = numfsm_run,
};
