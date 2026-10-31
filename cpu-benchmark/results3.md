| Kernel | Cat | Description | A µs | B µs | A cyc/u | B cyc/u | Unit | Speedup | Check |
|---|---|---|---|---|---|---|---|---|---|
| crc32 | INT | CRC-32 slice-by-4, 64 KB | 136.7 | 140.7 | 6.66 | 6.85 | B | 1.029× | ok |
| murmur3 | INT | MurmurHash3_x86_32, 4096 keys | 677.7 | 672.2 | 16.25 | 16.12 | B | 0.992× | ok |
| expgolmb | INT | Exp-Golomb ue(v) decode, 64K | 1220.0 | 1267.9 | 59.42 | 61.75 | sym | 1.039× | ok |
| divmod | INT | itoa, gcd, const div/mod mix | 1218.0 | 1245.7 | 210.93 | 215.72 | op | 1.023× | ok |
| bitops | INT | popcnt/clz/ctz/rev/rot, 8K u64 | 377.6 | 405.1 | 147.14 | 157.86 | word | 1.073× | ok |
| quicksrt | BR | quicksort 8192 int32, med-of-3 | 1088.7 | 1028.8 | 424.20 | 400.85 | elem | 0.945× | ok |
| bsearch | BR | lower_bound, 64K u32, 4K finds | 1468.4 | 1409.4 | 1144.31 | 1098.33 | find | 0.960× | ok |
| interp | BR | bytecode VM, switch dispatch | 2816.9 | 2723.4 | 60.97 | 58.94 | vmop | 0.967× | ok |
| numfsm | BR | number lexer FSM, 32 KB text | 643.3 | 653.9 | 62.66 | 63.70 | B | 1.016× | ok |
| bswap | MEM | 8192 LE 32 B records → native | 274.7 | 315.7 | 3.34 | 3.84 | B | 1.149× | ok |
| transpos | MEM | 256×256 u32 transpose (16×16) | 310.8 | 311.2 | 15.14 | 15.16 | elem | 1.002× | ok |
| ptrchase | MEM | 4096-line random cycle, 16K hops | 369.4 | 408.4 | 71.96 | 79.57 | hop | 1.106× | ok |
| stream | MEM | STREAM triad, 3 × 2 MB double | 10755.6 | 12227.6 | 5.46 | 6.20 | B | 1.137× | ok |
| matmul | FP | 64×64 double C=A*B, i-k-j | 1411.1 | 1226.3 | 8.59 | 7.47 | flop | 0.869× | ok |
| nbody | FP | 64 bodies, 4 steps, double | 780.4 | 783.1 | 308.93 | 309.98 | pair | 1.003× | ok |
| mandel | FP | float Mandelbrot 128×96, 64 it | 3044.3 | 3118.5 | 790.80 | 810.09 | px | 1.024× | ok |
| mathapx | FP | Cephes-style sinf/cosf/expf | 2628.4 | 2838.0 | 341.38 | 368.60 | eval | 1.080× | ok |
| vxform | VEC | vec4 × mat4 + vec_re persp div | 119.5 | 123.9 | 46.56 | 48.28 | vert | 1.037× | ok |
| vblend | VEC | 256×256 RGBA8 src-over, per-px α | 1039.6 | 1114.9 | 50.63 | 54.30 | px | 1.072× | ok |
| saxpy | VEC | saxpy + 4-lane sdot, 16K floats | 88.7 | 77.9 | 17.27 | 15.18 | elem | 0.879× | ok |
| pcmconv | VEC | s16 stereo 2×2 gain, clamp, s16 | 6852.6 | 7268.6 | 667.53 | 708.05 | frm | 1.061× | ok |
| **sad16** | **VEC** | **16×16 SAD motion search, ±3 px** | **2344.4** | **141.9** | **18.64** | **1.13** | **px** | **0.061×** | **WRONG B** |
| sha256 | MIX | SHA-256 compress, 256 blk 16 KB | 269.6 | 276.1 | 52.53 | 53.78 | B | 1.024× | ok |
| aes128 | MIX | AES-128 CTR, 4 T-tables, 16 KB | 206.3 | 199.5 | 40.19 | 38.88 | B | 0.967× | ok |
| lz4dec | MIX | LZ4 block decode, 128 KB text | 942.0 | 969.6 | 22.94 | 23.61 | B | 1.029× | ok |
| idct8 | MIX | JPEG islow IDCT 8×8, 1024 blk | 1022.6 | 1038.8 | 3187.76 | 3238.04 | blk | 1.016× | ok |
| raytrace | MIX | float ray tracer 64×48, 6 sph | 3435.5 | 3512.1 | 3569.72 | 3649.28 | ray | 1.022× | ok |
| memcpy | LIB | 4096 × 1..256 B + 4 × 32 KB | 611.4 | 1635.4 | 2.97 | 7.94 | B | 2.675× | ok |
| memset | LIB | 4096 × 1..512 B + 4 × 64 KB | 546.7 | 618.0 | 1.33 | 1.50 | B | 1.130× | ok |
| strops | LIB | strlen/strcmp/strchr/memcmp | 845.5 | 978.6 | 405.50 | 469.28 | call | 1.157× | ok |
| qsortlib | LIB | newlib qsort, 4096 u32 | 1077.4 | 987.7 | 839.61 | 769.70 | elem | 0.917× | ok |
| mathf | LIB | newlib sinf cosf expf logf sqrtf | 3175.7 | 3245.3 | 989.92 | 1011.61 | call | 1.022× | ok |
| mathd | LIB | newlib sin cos exp log sqrt | 4059.0 | 4076.0 | 1265.28 | 1270.57 | call | 1.004× | ok |