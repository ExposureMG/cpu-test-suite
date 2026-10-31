| Kernel | Cat | Description | A µs | B µs | A cyc/u | B cyc/u | Unit | Speedup | Check |
|---|---|---|---|---|---|---|---|---|---|
| crc32 | INT | CRC-32 slice-by-4, 64 KB | 136.8 | 140.5 | 6.66 | 6.84 | B | 1.027× | ok |
| murmur3 | INT | MurmurHash3_x86_32, 4096 keys | 677.3 | 672.9 | 16.24 | 16.13 | B | 0.993× | ok |
| expgolmb | INT | Exp-Golomb ue(v) decode, 64K | 1220.3 | 1267.9 | 59.44 | 61.75 | sym | 1.039× | ok |
| divmod | INT | itoa, gcd, const div/mod mix | 1218.1 | 1245.4 | 210.95 | 215.67 | op | 1.022× | ok |
| bitops | INT | popcnt/clz/ctz/rev/rot, 8K u64 | 377.6 | 405.1 | 147.12 | 157.86 | word | 1.073× | ok |
| quicksrt | BR | quicksort 8192 int32, med-of-3 | 1086.1 | 1028.1 | 423.22 | 400.60 | elem | 0.947× | ok |
| bsearch | BR | lower_bound, 64K u32, 4K finds | 1462.4 | 1408.4 | 1139.64 | 1097.59 | find | 0.963× | ok |
| interp | BR | bytecode VM, switch dispatch | 2817.0 | 2723.4 | 60.97 | 58.94 | vmop | 0.967× | ok |
| numfsm | BR | number lexer FSM, 32 KB text | 643.4 | 654.1 | 62.67 | 63.72 | B | 1.017× | ok |
| bswap | MEM | 8192 LE 32 B records → native | 272.1 | 308.9 | 3.31 | 3.76 | B | 1.136× | ok |
| transpos | MEM | 256×256 u32 transpose (16×16) | 311.5 | 310.9 | 15.17 | 15.14 | elem | 0.998× | ok |
| ptrchase | MEM | 4096-line random cycle, 16K hops | 365.8 | 408.0 | 71.28 | 79.49 | hop | 1.115× | ok |
| stream | MEM | STREAM triad, 3 × 2 MB double | 10742.1 | 12282.6 | 5.45 | 6.23 | B | 1.143× | ok |
| matmul | FP | 64×64 double C=A*B, i-k-j | 1406.9 | 1226.4 | 8.57 | 7.47 | flop | 0.872× | ok |
| nbody | FP | 64 bodies, 4 steps, double | 780.4 | 783.1 | 308.93 | 309.98 | pair | 1.003× | ok |
| mandel | FP | float Mandelbrot 128×96, 64 it | 3044.3 | 3118.5 | 790.80 | 810.09 | px | 1.024× | ok |
| mathapx | FP | Cephes-style sinf/cosf/expf | 2628.6 | 2839.3 | 341.40 | 368.78 | eval | 1.080× | ok |
| vxform | VEC | vec4 × mat4 + vec_re persp div | 118.7 | 124.8 | 46.27 | 48.62 | vert | 1.051× | ok |
| vblend | VEC | 256×256 RGBA8 src-over, per-px α | 1049.3 | 1145.1 | 51.11 | 55.77 | px | 1.091× | ok |
| saxpy | VEC | saxpy + 4-lane sdot, 16K floats | 85.8 | 83.4 | 16.71 | 16.25 | elem | 0.973× | ok |
| pcmconv | VEC | s16 stereo 2×2 gain, clamp, s16 | 6855.2 | 7359.1 | 667.78 | 716.86 | frm | 1.074× | ok |
| **sad16** | **VEC** | **16×16 SAD motion search, ±3 px** | **2344.4** | **141.9** | **18.64** | **1.13** | **px** | **0.061×** | **WRONG B** |
| sha256 | MIX | SHA-256 compress, 256 blk 16 KB | 269.6 | 276.0 | 52.53 | 53.78 | B | 1.024× | ok |
| aes128 | MIX | AES-128 CTR, 4 T-tables, 16 KB | 206.3 | 199.5 | 40.19 | 38.88 | B | 0.967× | ok |
| lz4dec | MIX | LZ4 block decode, 128 KB text | 941.9 | 969.7 | 22.94 | 23.62 | B | 1.030× | ok |
| idct8 | MIX | JPEG islow IDCT 8×8, 1024 blk | 1023.3 | 1042.3 | 3189.72 | 3248.94 | blk | 1.019× | ok |
| raytrace | MIX | float ray tracer 64×48, 6 sph | 3436.0 | 3512.7 | 3570.21 | 3649.87 | ray | 1.022× | ok |
| memcpy | LIB | 4096 × 1..256 B + 4 × 32 KB | 615.0 | 1636.5 | 2.99 | 7.95 | B | 2.661× | ok |
| memset | LIB | 4096 × 1..512 B + 4 × 64 KB | 546.5 | 618.0 | 1.33 | 1.50 | B | 1.131× | ok |
| strops | LIB | strlen/strcmp/strchr/memcmp | 846.0 | 979.9 | 405.74 | 469.93 | call | 1.158× | ok |
| qsortlib | LIB | newlib qsort, 4096 u32 | 1077.3 | 987.4 | 839.57 | 769.50 | elem | 0.917× | ok |
| mathf | LIB | newlib sinf cosf expf logf sqrtf | 3173.5 | 3252.9 | 989.22 | 1013.98 | call | 1.025× | ok |
| mathd | LIB | newlib sin cos exp log sqrt | 4062.4 | 4082.5 | 1266.33 | 1272.58 | call | 1.005× | ok |