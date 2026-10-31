# testxenon3 vs test16old: per-kernel comparison

Real Xbox 360, five boots of `build/bench-ab.elf32` (results1-5.md). Speedup = test16old time / testxenon3 time: above 1 means testxenon3 is faster. Each boot lands in one of two states (X: runs 1, 2, 5; Y: runs 3, 4); cycles per unit are the state-X medians.

| Kernel | Cat | What it exercises | Unit | testxenon3 cyc/unit | test16old cyc/unit | Speedup X | Speedup Y | Result check | Why |
|---|---|---|---|---|---|---|---|---|---|
| crc32 | INT | Table lookups, shifts and XORs (zlib slice-by-4) | B | 6.66 | 6.84 | 1.028× | 1.029× | both ok | Not traced |
| murmur3 | INT | 32-bit multiply and rotate chains | B | 16.24 | 16.13 | 0.994× | 0.992× | both ok | On par |
| expgolmb | INT | Bit-stream decode: count-leading-zeros, variable shifts | sym | 59.43 | 61.75 | **1.039×** | **1.039×** | both ok | Not traced |
| divmod | INT | Integer divide, divide-by-constant, itoa, GCD | op | 210.96 | 215.67 | 1.022× | 1.023× | both ok | Not traced |
| bitops | INT | 64-bit popcount/clz/ctz/reverse/rotate | word | 147.11 | 157.86 | **1.073×** | **1.073×** | both ok | test16old has a 14-cycle `andi.` in the per-word loop |
| quicksrt | BR | Data-dependent branches, compare and swap | elem | 423.17 | 400.62 | **0.947×** | **0.945×** | both ok | Same loop structure; differs in loop-top padding |
| bsearch | BR | Unpredictable branch plus L2-latency loads | find | 1139.59 | 1097.59 | **0.963×** | **0.960×** | both ok | Identical inner loop; layout alone |
| interp | BR | switch() dispatch through an indirect branch | vmop | 60.97 | 58.94 | **0.967×** | **0.967×** | both ok | Not traced; small enough to be code layout |
| numfsm | BR | Character state machine, byte loads, branches | B | 62.67 | 63.72 | 1.017× | 1.017× | both ok | On par |
| bswap | MEM | Byte-reversed loads/stores (`lwbrx`, `ldbrx`) | B | 3.31 | 3.76 | **1.135×** | **1.149×** | both ok | Better load-to-store spacing in testxenon3 |
| transpos | MEM | Strided 2-D access, cache-set conflicts | elem | 15.17 | 15.14 | 0.998× | 1.002× | both ok | On par |
| ptrchase | MEM | Dependent-load latency (pointer chase in L2) | hop | 71.28 | 79.50 | **1.115×** | **1.106×** | both ok | More distance between loads and uses (likely) |
| stream | MEM | Memory bandwidth, double FMA streaming | B | 5.45 | 6.24 | **1.145×** | **1.140×** | both ok | All loads issued before the first FMA (likely) |
| matmul | FP | Double FMA, FP load/store issue | flop | 8.57 | 7.47 | **0.872×** | **0.869×** | both ok | Same instructions, different schedule, padded loop top |
| nbody | FP | Double sqrt/divide, FP latency | pair | 308.93 | 309.98 | 1.003× | 1.003× | both ok | Same FP code |
| mandel | FP | Float dependency chains with an exit branch | px | 790.80 | 810.09 | 1.024× | 1.024× | both ok | Same FP code |
| mathapx | FP | Inline sinf/cosf/expf polynomials, range reduction | eval | 341.40 | 368.77 | **1.080×** | **1.080×** | both ok | test16old has two 14-cycle `andi.` per sin/cos |
| vxform | VEC | AltiVec float multiply-add, `vec_re` divide | vert | 46.25 | 48.61 | **1.051×** | **1.037×** | both ok | VMX128 registers; varies by boot state |
| vblend | VEC | AltiVec byte/halfword math, pack/unpack | px | 51.10 | 55.77 | **1.091×** | **1.072×** | both ok | VMX128 registers; varies by boot state |
| saxpy | VEC | Auto-vectoriser: saxpy and a 4-lane dot product | elem | 16.72 | 16.25 | 0.972× | **0.879×** | both ok | Varies most by boot state |
| pcmconv | VEC | int16 to float gain, clamp, back to int16 | frm | 667.78 | 716.86 | **1.074×** | **1.066×** | both ok | Not traced |
| sad16 | VEC | Vectorising a widening sum (motion search SAD) | px | 18.64 | 1.13 | n/a | n/a | test16old WRONG | test16old miscompiles it (wrong result) |
| sha256 | MIX | 32-bit rotates and adds | B | 52.53 | 53.78 | 1.024× | 1.024× | both ok | On par |
| aes128 | MIX | T-table lookups, byte extraction | B | 40.19 | 38.88 | **0.967×** | **0.967×** | both ok | Not traced; small enough to be code layout |
| lz4dec | MIX | Byte copies, unaligned access, branchy decode | B | 22.94 | 23.62 | 1.030× | 1.029× | both ok | Not traced |
| idct8 | MIX | 16-bit integer multiply-accumulate | blk | 3189.72 | 3248.81 | 1.018× | 1.016× | both ok | On par |
| raytrace | MIX | Float sqrt/divide, branches, mixed work | ray | 3564.92 | 3649.46 | 1.024× | 1.022× | both ok | On par |
| memcpy | LIB | newlib memcpy, 1-256 B unaligned + 32 KB aligned | B | 2.99 | 7.95 | **2.661×** | **2.675×** | both ok | Unaligned byte loop vectorised with `lvlx/lvrx` |
| memset | LIB | newlib memset, 1-512 B + 64 KB | B | 1.33 | 1.50 | **1.131×** | **1.130×** | both ok | test16old's memset uses slow `andi.` tests |
| strops | LIB | newlib strlen/strcmp/strchr/memcmp | call | 405.74 | 469.88 | **1.158×** | **1.157×** | both ok | test16old's word loops use 14-cycle `and.` |
| qsortlib | LIB | newlib qsort with an indirect comparator | elem | 839.59 | 769.50 | **0.916×** | **0.917×** | both ok | testxenon3's qsort is 1.8x the code (likely) |
| mathf | LIB | newlib sinf cosf expf logf sqrtf | call | 989.15 | 1013.86 | 1.025× | 1.022× | both ok | On par |
| mathd | LIB | newlib sin cos exp log sqrt | call | 1266.30 | 1272.30 | 1.005× | 1.004× | both ok | On par |

Bold: testxenon3 at least 3% faster or slower. sad16 is left out of every geomean.

| Category | Kernels | Geomean X | Geomean Y |
|---|---|---|---|
| INT | 5 | 1.031× | 1.031× |
| BR | 4 | 0.973× | 0.972× |
| MEM | 4 | 1.097× | 1.097× |
| FP | 4 | 0.992× | 0.991× |
| VEC | 4 | 1.046× | 1.010× |
| MIX | 5 | 1.012× | 1.011× |
| LIB | 6 | 1.220× | 1.220× |
| **All** | 32 | **1.058×** | **1.053×** |
| All except LIB | 26 | 1.024× | 1.018× |
