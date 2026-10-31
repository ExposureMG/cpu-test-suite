/*
 * mem_ptrchase.c - dependent-load latency: a random pointer chase (MEM).
 *
 * 4096 nodes, one per 128-byte cache line (512 KB: well past the 32 KB L1,
 * inside the 1 MB L2), linked into ONE random cycle with Sattolo's
 * algorithm, the way a linked list of game objects ends up scattered over
 * the heap.  run() follows 16384 links from node 0 (four laps of the cycle)
 * and folds the payloads in visiting order (rotate-add, so the order
 * matters, and the fold stays off the critical path).
 *
 * Every load address comes from the previous load (lwz rN,0(rN)), so the
 * kernel measures load-to-use latency of the memory hierarchy and should
 * come out about even between the two compilers: it is the control kernel.
 *
 * init stores absolute pointers into the arena; both variants run on the
 * same arena, so the B variant follows the same links.
 */
#include "bench_kernel.h"

#define PC_NODES    4096u
#define PC_HOPS     16384u	/* 4 laps of the 4096-node cycle */
#define PC_OFF      0u
#define ARENA_BYTES (PC_OFF + PC_NODES * BENCH_LINE)

typedef struct pc_node {
	struct pc_node *next;
	uint32_t payload;
	unsigned char pad[BENCH_LINE - sizeof(struct pc_node *) - sizeof(uint32_t)];
} pc_node;

_Static_assert(sizeof(pc_node) == BENCH_LINE, "one node per cache line");

static void ptrchase_init(void *arena)
{
	pc_node *n = (pc_node *)bench_at(arena, PC_OFF);
	bench_rng rng;
	uint32_t i;

	bench_rng_seed(&rng, 0x9C4A5E01u);

	/* Sattolo's algorithm, with the payload fields as the permutation
	 * array: afterwards n[i].payload is the successor of node i, and the
	 * successor map is a single cycle through all PC_NODES nodes. */
	for (i = 0; i < PC_NODES; i++)
		n[i].payload = i;
	for (i = PC_NODES - 1u; i > 0; i--) {
		uint32_t j = bench_rng_below(&rng, i);	/* 0 <= j < i */
		uint32_t t = n[i].payload;

		n[i].payload = n[j].payload;
		n[j].payload = t;
	}
	for (i = 0; i < PC_NODES; i++)
		n[i].next = &n[n[i].payload];

	/* The real payloads. */
	for (i = 0; i < PC_NODES; i++)
		n[i].payload = bench_rng_next(&rng);
}

static double ptrchase_run(void *arena)
{
	const pc_node *p = (const pc_node *)bench_at(arena, PC_OFF);
	uint32_t h = 0, i;

	for (i = 0; i < PC_HOPS; i++) {
		h = ((h << 3) | (h >> 29)) + p->payload;
		p = p->next;
	}
	/* After exactly four laps p is node 0 again. */
	return bench_result_u32(bench_mix(h, p->payload));
}

BENCH_KDEF(ptrchase) = {
	.name = "ptrchase", .desc = "4096-line random cycle, 16K hops", .unit = "hop",
	.category = BENCH_CAT_MEM, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = PC_HOPS, .arena_bytes = ARENA_BYTES,
	.init = ptrchase_init, .run = ptrchase_run,
};
