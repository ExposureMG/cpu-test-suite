/*
 * br_interp.c - bytecode VM with "for (;;) switch (*pc++)" dispatch (BR).
 *
 * A small stack machine of the kind found in game scripting engines and
 * homebrew emulators: 24 one-byte opcodes, operands in the following bytes
 * (immediates are assembled from bytes, LSB first, so the bytecode is the same
 * on any host), an operand stack indexed by a uint8_t (it wraps like the 6502
 * stack page, so it can never leave its 256 slots), 16 registers, and 4096
 * words of data memory with masked addresses.  Registers, stack and memory
 * live in the arena and are cleared at the start of run(); the VM counts the
 * ops it executes, like an emulator's cycle counter.
 *
 * init assembles a fixed program into the arena:
 *   1. sieve of Eratosthenes over N = 2048 flag words;
 *   2. a pass over the flags counting the primes and hashing the prime gaps;
 *   3. nested LCG loops (K x J = 16 x 160) mixing a hash register with
 *      variable shifts and scattering it into a 256-word histogram.
 * Every loop bound is an immediate in the bytecode, so the C compiler cannot
 * fold anything.  At HALT, run() folds the op count, the registers, the
 * stack and the data memory.
 *
 * A taken jump after more than OPS_BUDGET ops also halts the VM.  The
 * program never gets near it; it only turns corrupt bytecode (say, an init
 * miscompiled on the console, where nothing else could catch a loop) into a
 * wrong result instead of a hang.
 */
#include "bench_kernel.h"

enum {
	OP_HALT,  /*                 stop                                    */
	OP_PUSHB, /* u8              push zero-extended byte                 */
	OP_PUSHW, /* u32 (LSB first) push word                               */
	OP_LOAD,  /* r               push R[r]                               */
	OP_STORE, /* r               R[r] = pop                              */
	OP_INC,   /* r               R[r] += 1                               */
	OP_LDM,   /*                 a = pop; push M[a]                      */
	OP_STM,   /*                 v = pop; a = pop; M[a] = v              */
	OP_ADD,   /* binary ops:     b = pop; a = pop; push a <op> b         */
	OP_SUB,
	OP_MUL,
	OP_AND,
	OP_OR,
	OP_XOR,
	OP_SHL,   /*                 a << (b & 31)                           */
	OP_SHR,   /*                 a >> (b & 31), logical                  */
	OP_LT,    /*                 a < b, unsigned, 0 or 1                 */
	OP_EQ,    /*                 a == b, 0 or 1                          */
	OP_DUP,
	OP_SWAP,
	OP_DROP,
	OP_JMP,   /* u16 (LSB first) absolute code address                   */
	OP_JZ,    /* u16             pop; jump if zero                       */
	OP_JNZ,   /* u16             pop; jump if not zero                   */
	OP_COUNT
};

#define VM_NREG    16u
#define VM_STACK   256u   /* indexed by uint8_t: wraps, never out of bounds */
#define VM_MEMW    4096u
#define VM_MEMMASK (VM_MEMW - 1u)
#define VM_CODE    1024u  /* code region; the program is far smaller        */
#define VM_JMASK   511u   /* jump targets stay in the first 512 bytes       */

/* Program constants (immediates in the bytecode). */
#define P_N      2048u    /* sieve size (flag words M[0..N))               */
#define P_K      16u      /* outer LCG loop                                 */
#define P_J      160u     /* inner LCG loop; histogram at M[N..N+256)       */
#define P_SEED   0x2545F491u

/* Ops executed by one run of the program, counted on the host. */
#define INTERP_OPS 147486u
#define OPS_BUDGET (4u * INTERP_OPS)

#define CODE_OFF    0u
#define REG_OFF     BENCH_ALIGN_UP(CODE_OFF + VM_CODE, BENCH_LINE)
#define STK_OFF     BENCH_ALIGN_UP(REG_OFF + VM_NREG * 4u, BENCH_LINE)
#define MEM_OFF     BENCH_ALIGN_UP(STK_OFF + VM_STACK * 4u, BENCH_LINE)
#define ARENA_BYTES BENCH_ALIGN_UP(MEM_OFF + VM_MEMW * 4u, BENCH_LINE)

static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static inline uint32_t rd16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/* ---- the VM ------------------------------------------------------------ */

static double interp_run(void *arena)
{
	const uint8_t *code = (const uint8_t *)bench_at(arena, CODE_OFF);
	uint32_t *R = (uint32_t *)bench_at(arena, REG_OFF);
	uint32_t *S = (uint32_t *)bench_at(arena, STK_OFF);
	uint32_t *M = (uint32_t *)bench_at(arena, MEM_OFF);
	const uint8_t *pc = code;
	uint32_t ops = 0, a, b, h, i;
	uint8_t sp = 0;

	bench_fill(R, 0, VM_NREG * 4u);
	bench_fill(S, 0, VM_STACK * 4u);
	bench_fill(M, 0, VM_MEMW * 4u);

#define BINOP(expr)                          \
	b = S[--sp];                         \
	a = S[(uint8_t)(sp - 1)];            \
	S[(uint8_t)(sp - 1)] = (expr);       \
	break

	for (;;) {
		ops++;
		switch (*pc++) {
		case OP_HALT:
			goto halt;
		case OP_PUSHB:
			S[sp++] = *pc++;
			break;
		case OP_PUSHW:
			S[sp++] = rd32(pc);
			pc += 4;
			break;
		case OP_LOAD:
			S[sp++] = R[*pc++ & 15u];
			break;
		case OP_STORE:
			R[*pc++ & 15u] = S[--sp];
			break;
		case OP_INC:
			R[*pc++ & 15u]++;
			break;
		case OP_LDM:
			a = S[(uint8_t)(sp - 1)];
			S[(uint8_t)(sp - 1)] = M[a & VM_MEMMASK];
			break;
		case OP_STM:
			b = S[--sp];
			a = S[--sp];
			M[a & VM_MEMMASK] = b;
			break;
		case OP_ADD: BINOP(a + b);
		case OP_SUB: BINOP(a - b);
		case OP_MUL: BINOP(a * b);
		case OP_AND: BINOP(a & b);
		case OP_OR:  BINOP(a | b);
		case OP_XOR: BINOP(a ^ b);
		case OP_SHL: BINOP(a << (b & 31u));
		case OP_SHR: BINOP(a >> (b & 31u));
		case OP_LT:  BINOP(a < b);
		case OP_EQ:  BINOP(a == b);
		case OP_DUP:
			a = S[(uint8_t)(sp - 1)];
			S[sp++] = a;
			break;
		case OP_SWAP:
			a = S[(uint8_t)(sp - 1)];
			S[(uint8_t)(sp - 1)] = S[(uint8_t)(sp - 2)];
			S[(uint8_t)(sp - 2)] = a;
			break;
		case OP_DROP:
			sp--;
			break;
		case OP_JMP:
			if (ops > OPS_BUDGET)
				goto halt;
			pc = code + (rd16(pc) & VM_JMASK);
			break;
		case OP_JZ:
			if (S[--sp] == 0) {
				if (ops > OPS_BUDGET)
					goto halt;
				pc = code + (rd16(pc) & VM_JMASK);
			} else {
				pc += 2;
			}
			break;
		case OP_JNZ:
			if (S[--sp] != 0) {
				if (ops > OPS_BUDGET)
					goto halt;
				pc = code + (rd16(pc) & VM_JMASK);
			} else {
				pc += 2;
			}
			break;
		default: /* illegal opcode */
			goto halt;
		}
	}
#undef BINOP

halt:
	h = bench_mix(ops, sp);
	for (i = 0; i < VM_NREG; i++)
		h = bench_mix(h, R[i]);
	for (i = 0; i < VM_STACK; i++)
		h = bench_mix(h, S[i]);
	for (i = 0; i < VM_MEMW; i++)
		h = bench_mix(h, M[i]);
	return bench_result_u32(h);
}

/* ---- a tiny assembler for init ----------------------------------------- */

typedef struct vm_asm {
	uint8_t *code;
	uint32_t pos;
} vm_asm;

static void as_op(vm_asm *as, uint32_t op)
{
	as->code[as->pos++] = (uint8_t)op;
}

static void as_opb(vm_asm *as, uint32_t op, uint32_t v)
{
	as_op(as, op);
	as_op(as, v & 0xFFu);
}

static void as_pushw(vm_asm *as, uint32_t v)
{
	as_op(as, OP_PUSHW);
	as_op(as, v & 0xFFu);
	as_op(as, (v >> 8) & 0xFFu);
	as_op(as, (v >> 16) & 0xFFu);
	as_op(as, v >> 24);
}

/* Emits a jump; returns the operand position for as_patch(). */
static uint32_t as_jump(vm_asm *as, uint32_t op, uint32_t target)
{
	uint32_t at;

	as_op(as, op);
	at = as->pos;
	as_op(as, target & 0xFFu);
	as_op(as, target >> 8);
	return at;
}

static void as_patch(vm_asm *as, uint32_t at, uint32_t target)
{
	as->code[at] = (uint8_t)(target & 0xFFu);
	as->code[at + 1] = (uint8_t)(target >> 8);
}

enum { R_I, R_J, R_CNT, R_N, R_H, R_K, R_X, R_KN, R_JN, R_PH, R_LAST };

static void interp_init(void *arena)
{
	vm_asm as = { (uint8_t *)bench_at(arena, CODE_OFF), 0 };
	uint32_t outer, inner, cloop, kloop, jloop;
	uint32_t j_count, j_next, j_inner, j_cdone, j_cnext, j_end, j_knext;
	uint32_t next, next_drop, count_drop, cnext, cdone, knext, end;

	/* Pad the whole code region with HALT (opcode 0, so a zero fill). */
	bench_fill(as.code, 0, VM_CODE);

	/* 1. sieve: M[j] = 1 for every composite j < N */
	as_pushw(&as, P_N);
	as_opb(&as, OP_STORE, R_N);
	as_opb(&as, OP_PUSHB, 2);
	as_opb(&as, OP_STORE, R_I);
	outer = as.pos;                           /* sq = i * i; while (sq < N) */
	as_opb(&as, OP_LOAD, R_I);
	as_op(&as, OP_DUP);
	as_op(&as, OP_MUL);
	as_op(&as, OP_DUP);
	as_opb(&as, OP_LOAD, R_N);
	as_op(&as, OP_LT);
	j_count = as_jump(&as, OP_JZ, 0);
	as_opb(&as, OP_LOAD, R_I);                /* if (M[i] == 0)             */
	as_op(&as, OP_LDM);
	j_next = as_jump(&as, OP_JNZ, 0);
	as_opb(&as, OP_STORE, R_J);               /*   j = sq                   */
	inner = as.pos;                           /*   while (j < N)            */
	as_opb(&as, OP_LOAD, R_J);
	as_opb(&as, OP_LOAD, R_N);
	as_op(&as, OP_LT);
	j_inner = as_jump(&as, OP_JZ, 0);
	as_opb(&as, OP_LOAD, R_J);                /*     M[j] = 1              */
	as_opb(&as, OP_PUSHB, 1);
	as_op(&as, OP_STM);
	as_opb(&as, OP_LOAD, R_J);                /*     j += i                */
	as_opb(&as, OP_LOAD, R_I);
	as_op(&as, OP_ADD);
	as_opb(&as, OP_STORE, R_J);
	as_jump(&as, OP_JMP, inner);
	next_drop = as.pos;                       /* (drop sq) i++              */
	as_op(&as, OP_DROP);
	next = as.pos;
	as_opb(&as, OP_INC, R_I);
	as_jump(&as, OP_JMP, outer);
	count_drop = as.pos;                      /* (drop sq)                  */
	as_op(&as, OP_DROP);

	/* 2. count primes in [2, N), hash the gaps: ph = ph * 33 ^ (i - last) */
	as_opb(&as, OP_PUSHB, 2);
	as_opb(&as, OP_STORE, R_I);
	cloop = as.pos;
	as_opb(&as, OP_LOAD, R_I);
	as_opb(&as, OP_LOAD, R_N);
	as_op(&as, OP_LT);
	j_cdone = as_jump(&as, OP_JZ, 0);
	as_opb(&as, OP_LOAD, R_I);
	as_op(&as, OP_LDM);
	j_cnext = as_jump(&as, OP_JNZ, 0);
	as_opb(&as, OP_INC, R_CNT);
	as_opb(&as, OP_LOAD, R_I);
	as_opb(&as, OP_LOAD, R_LAST);
	as_op(&as, OP_SUB);
	as_opb(&as, OP_LOAD, R_PH);
	as_opb(&as, OP_PUSHB, 5);
	as_op(&as, OP_SHL);
	as_opb(&as, OP_LOAD, R_PH);
	as_op(&as, OP_ADD);
	as_op(&as, OP_XOR);
	as_opb(&as, OP_STORE, R_PH);
	as_opb(&as, OP_LOAD, R_I);
	as_opb(&as, OP_STORE, R_LAST);
	cnext = as.pos;
	as_opb(&as, OP_INC, R_I);
	as_jump(&as, OP_JMP, cloop);
	cdone = as.pos;

	/* 3. for (k = 0; k != K; k++) { for (j = 0; j < J; j++) {
	 *        x = x * 1103515245 + 12345;
	 *        h ^= (x >> 16) << ((j + k) & 7);
	 *        M[N + (x >> 24)] += h; }
	 *      h = rotl(h, 3); } */
	as_pushw(&as, P_SEED);
	as_opb(&as, OP_STORE, R_X);
	as_opb(&as, OP_PUSHB, P_K);
	as_opb(&as, OP_STORE, R_KN);
	as_opb(&as, OP_PUSHB, P_J);
	as_opb(&as, OP_STORE, R_JN);
	as_opb(&as, OP_PUSHB, 0);
	as_opb(&as, OP_STORE, R_K);
	kloop = as.pos;
	as_opb(&as, OP_LOAD, R_K);
	as_opb(&as, OP_LOAD, R_KN);
	as_op(&as, OP_EQ);
	j_end = as_jump(&as, OP_JNZ, 0);
	as_opb(&as, OP_PUSHB, 0);
	as_opb(&as, OP_STORE, R_J);
	jloop = as.pos;
	as_opb(&as, OP_LOAD, R_J);
	as_opb(&as, OP_LOAD, R_JN);
	as_op(&as, OP_LT);
	j_knext = as_jump(&as, OP_JZ, 0);
	as_opb(&as, OP_LOAD, R_X);
	as_pushw(&as, 1103515245u);
	as_op(&as, OP_MUL);
	as_pushw(&as, 12345u);
	as_op(&as, OP_ADD);
	as_op(&as, OP_DUP);
	as_opb(&as, OP_STORE, R_X);
	as_opb(&as, OP_PUSHB, 16);
	as_op(&as, OP_SHR);
	as_opb(&as, OP_LOAD, R_J);
	as_opb(&as, OP_LOAD, R_K);
	as_op(&as, OP_ADD);
	as_opb(&as, OP_PUSHB, 7);
	as_op(&as, OP_AND);
	as_op(&as, OP_SHL);
	as_opb(&as, OP_LOAD, R_H);
	as_op(&as, OP_XOR);
	as_opb(&as, OP_STORE, R_H);
	as_opb(&as, OP_LOAD, R_X);
	as_opb(&as, OP_PUSHB, 24);
	as_op(&as, OP_SHR);
	as_opb(&as, OP_LOAD, R_N);
	as_op(&as, OP_ADD);
	as_op(&as, OP_DUP);
	as_op(&as, OP_LDM);
	as_opb(&as, OP_LOAD, R_H);
	as_op(&as, OP_ADD);
	as_op(&as, OP_STM);
	as_opb(&as, OP_INC, R_J);
	as_jump(&as, OP_JMP, jloop);
	knext = as.pos;
	as_opb(&as, OP_LOAD, R_H);
	as_op(&as, OP_DUP);
	as_opb(&as, OP_PUSHB, 3);
	as_op(&as, OP_SHL);
	as_op(&as, OP_SWAP);
	as_opb(&as, OP_PUSHB, 29);
	as_op(&as, OP_SHR);
	as_op(&as, OP_OR);
	as_opb(&as, OP_STORE, R_H);
	as_opb(&as, OP_INC, R_K);
	as_jump(&as, OP_JMP, kloop);
	end = as.pos;
	as_op(&as, OP_HALT);

	as_patch(&as, j_count, count_drop);
	as_patch(&as, j_next, next_drop);
	as_patch(&as, j_inner, next);
	as_patch(&as, j_cdone, cdone);
	as_patch(&as, j_cnext, cnext);
	as_patch(&as, j_end, end);
	as_patch(&as, j_knext, knext);
}

BENCH_KDEF(interp) = {
	.name = "interp", .desc = "bytecode VM, switch dispatch", .unit = "vmop",
	.category = BENCH_CAT_BR, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = INTERP_OPS, .arena_bytes = ARENA_BYTES,
	.init = interp_init, .run = interp_run,
};
