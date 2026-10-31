| Kernel | Cat | Description | A µs | B µs | A cyc/u | B cyc/u | Unit | Speedup | Check |
|---|---|---|---|---|---|---|---|---|---|
| crc32 | INT | CRC-32 slice-by-4, 64 KB | 136.7 | 140.5 | 6.66 | 6.84 | B | 1.027× | ok |
| murmur3 | INT | MurmurHash3_x86_32, 4096 keys | 677.3 | 672.9 | 16.24 | 16.13 | B | 0.993× | ok |
| expgolmb | INT | Exp-Golomb ue(v) decode, 64K | 1220.2 | 1267.9 | 59.43 | 61.75 | sym | 1.039× | ok |
| divmod | INT | itoa, gcd, const div/mod mix | 1218.1 | 1245.4 | 210.96 | 215.68 | op | 1.022× | ok |
| bitops | INT | popcnt/clz/ctz/rev/rot, 8K u64 | 377.5 | 405.1 | 147.11 | 157.86 | word | 1.073× | ok |
| quicksrt | BR | quicksort 8192 int32, med-of-3 | 1086.0 | 1028.2 | 423.16 | 400.62 | elem | 0.947× | ok |
| bsearch | BR | lower_bound, 64K u32, 4K finds | 1462.3 | 1408.4 | 1139.59 | 1097.59 | find | 0.963× | ok |
| interp | BR | bytecode VM, switch dispatch | 2817.0 | 2723.4 | 60.97 | 58.94 | vmop | 0.967× | ok |
| numfsm | BR | number lexer FSM, 32 KB text | 643.3 | 654.2 | 62.67 | 63.72 | B | 1.017× | ok |
| bswap | MEM | 8192 LE 32 B records → native | 272.1 | 308.9 | 3.31 | 3.76 | B | 1.135× | ok |
| transpos | MEM | 256×256 u32 transpose (16×16) | 311.4 | 310.9 | 15.17 | 15.14 | elem | 0.998× | ok |
| ptrchase | MEM | 4096-line random cycle, 16K hops | 365.8 | 408.0 | 71.28 | 79.50 | hop | 1.115× | ok |
| stream | MEM | STREAM triad, 3 × 2 MB double | 10738.7 | 12303.2 | 5.45 | 6.24 | B | 1.146× | ok |
| matmul | FP | 64×64 double C=A*B, i-k-j | 1406.9 | 1226.4 | 8.57 | 7.47 | flop | 0.872× | ok |
| nbody | FP | 64 bodies, 4 steps, double | 780.4 | 783.1 | 308.93 | 309.98 | pair | 1.003× | ok |
| mandel | FP | float Mandelbrot 128×96, 64 it | 3044.3 | 3118.5 | 790.80 | 810.09 | px | 1.024× | ok |
| mathapx | FP | Cephes-style sinf/cosf/expf | 2628.5 | 2839.2 | 341.40 | 368.76 | eval | 1.080× | ok |
| vxform | VEC | vec4 × mat4 + vec_re persp div | 118.7 | 124.8 | 46.25 | 48.61 | vert | 1.051× | ok |
| vblend | VEC | 256×256 RGBA8 src-over, per-px α | 1049.2 | 1145.1 | 51.10 | 55.77 | px | 1.091× | ok |
| saxpy | VEC | saxpy + 4-lane sdot, 16K floats | 85.8 | 83.4 | 16.72 | 16.25 | elem | 0.972× | ok |
| pcmconv | VEC | s16 stereo 2×2 gain, clamp, s16 | 6855.2 | 7358.9 | 667.78 | 716.85 | frm | 1.073× | ok |
| **sad16** | **VEC** | **16×16 SAD motion search, ±3 px** | **2344.4** | **141.9** | **18.64** | **1.13** | **px** | **0.061×** | **WRONG B** |
| sha256 | MIX | SHA-256 compress, 256 blk 16 KB | 269.6 | 276.0 | 52.53 | 53.78 | B | 1.024× | ok |
| aes128 | MIX | AES-128 CTR, 4 T-tables, 16 KB | 206.3 | 199.5 | 40.19 | 38.88 | B | 0.967× | ok |
| lz4dec | MIX | LZ4 block decode, 128 KB text | 942.0 | 969.8 | 22.94 | 23.62 | B | 1.030× | ok |
| idct8 | MIX | JPEG islow IDCT 8×8, 1024 blk | 1023.3 | 1042.2 | 3189.70 | 3248.81 | blk | 1.019× | ok |
| raytrace | MIX | float ray tracer 64×48, 6 sph | 3430.0 | 3512.0 | 3563.96 | 3649.15 | ray | 1.024× | ok |
| memcpy | LIB | 4096 × 1..256 B + 4 × 32 KB | 614.8 | 1636.6 | 2.99 | 7.95 | B | 2.662× | ok |
| memset | LIB | 4096 × 1..512 B + 4 × 64 KB | 546.4 | 618.0 | 1.33 | 1.50 | B | 1.131× | ok |
| strops | LIB | strlen/strcmp/strchr/memcmp | 846.1 | 979.8 | 405.74 | 469.87 | call | 1.158× | ok |
| qsortlib | LIB | newlib qsort, 4096 u32 | 1077.4 | 987.4 | 839.59 | 769.50 | elem | 0.917× | ok |
| mathf | LIB | newlib sinf cosf expf logf sqrtf | 3173.1 | 3252.5 | 989.10 | 1013.86 | call | 1.025× | ok |
| mathd | LIB | newlib sin cos exp log sqrt | 4062.2 | 4081.6 | 1266.27 | 1272.30 | call | 1.005× | ok |