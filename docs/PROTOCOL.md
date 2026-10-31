# XTS wire protocol (version 1): UDP messages from the console to a receiver

Everything the run produces leaves as UDP datagrams; nothing travels from the PC to the console (the console's network
stack answers ARP and keeps its DHCP lease, nothing more). The receiver (`tools/xts_recv.py`) rebuilds the run
directory and detects loss.

## Datagram
- **Default (libxenon lwIP):** source = the console's DHCP address (libxenon's static 192.168.1.99 without DHCP),
  destination = the receiver (`make NET_DEST=auto|<ip>`, unicast) or `255.255.255.255` (broadcast). UDP port 36360 on
  both ends; IPv4 and UDP headers and checksums as lwIP builds them.
- **`make NET_DRIVER=own`:** raw Ethernet frames built by `src/core/netfmt.c`: destination MAC/IP from `NET_DEST` or
  broadcast, source IP `169.254.<mac[4]>.<mac[5]>`, TTL 64, DF set, IPv4 header checksum computed, UDP checksum 0,
  frames padded to 60 bytes.

## XTS header (36 bytes, big-endian, at the start of the UDP payload)
| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"XTS1"` |
| 4 | 1 | version = 1 |
| 5 | 1 | type (`XTS_PK_*`, `include/xts/net.h`) |
| 6 | 1 | flags: bit0 = this frame is a repeat copy |
| 7 | 1 | copy index (0..copies-1) |
| 8 | 8 | run_nonce: identifies the run and console (console: MAC in the high 48 bits ^ boot timebase) |
| 16 | 4 | seq: per-run frame sequence number, starting at 1, +1 per frame (not per copy) |
| 20 | 4 | msg: per-run message number, starting at 1 |
| 24 | 2 | frag_idx (0-based) |
| 26 | 2 | frag_cnt (>= 1) |
| 28 | 2 | payload_len (bytes after the header, <= 1400) |
| 30 | 2 | reserved, 0 |
| 32 | 4 | crc32 (IEEE, as zlib.crc32) over header bytes 0..31 followed by the payload |

## Messages
A message is a byte string, UTF-8 JSON for every type, at most 32,768 bytes. It is split into `frag_cnt` frames with
consecutive `seq` values and the same `msg`. Types:

| Type | Name | Payload |
|---|---|---|
| 1 | HELLO | `{"sysinfo":{...}, "profile":"smoke", "plan":{"blocks":N,"cases":M}, "cores":{...}, "build_id":"..."}`. Sent as 3 separate messages at run start (each its own msg/seq). |
| 2 | REC | one `results.jsonl` line (the record schema in CONTRIBUTING.md, unchanged) |
| 3 | LOG | one `run.log` line |
| 4 | CRUMB | `{"block":"...","case_index":k,"case_id":"...","family":"...","mnem":"...","word":"7C642A14","tb":...}` |
| 5 | CRASH | crash report: `{"pir":0,"vector":"0700","srr0":...,"srr1":...,"dar":...,"dsisr":...,"lr":...,"ctr":...,"cr":...,"xer":...,"gpr":[...32],"tb":...,"crumb":{...},"input":{...},"log_tail":[...]}` |
| 6 | HANG | `{"crumb":{...},"timeout_us":...,"heartbeats":{"0":n,"2":n,"4":n}}` |
| 7 | BLOCK | `{"block":"...","records":n}` |
| 8 | END | `{"status":"complete"\|"aborted"\|"crashed"\|"hang","last_seq":S,"messages":{"HELLO":3,"REC":n,"LOG":n,...},"rec_hash":"<16 hex>"}`. Sent as 3 messages; the receiver uses the first intact one. Counts and `last_seq` describe the stream up to, but not including, the END messages. |

Payload objects may carry more members than listed (e.g. BLOCK `family`, `ms`, `seq`, `temps`; END `run_id`,
`records`, `send_failures`, `status_counts`, `net`); receivers ignore members they do not know.

**Multi-pass sessions.** A profile with several passes per boot sends one complete run per pass, one after the other:
each pass has its own `run_nonce` (and `run_id`), its `seq` and `msg` numbers start at 1 again, and END's counts,
`last_seq`, `rec_hash` and `net` cover that pass only. HELLO and END then carry
`"session": {"id": "<16 hex, the first pass's run_id>", "pass": k, "passes": n}` (k 1-based). Single-pass runs have
no `session` member. The console waits `PASS_GAP_S` seconds (default 30) after a pass's END before the next HELLO.

**`rec_hash`:** FNV-1a 64-bit over the payloads of all REC messages, concatenated in `msg` order, with offset basis
0xcbf29ce484222325 and prime 0x100000001b3. Lowercase hex.

## Sending rules (console)
- **Copies:** each frame is sent `XTS_NET_COPIES` times (default 2), about 50 µs apart. The copies are identical except
  for flags bit0 and the copy index.
- **Pacing:** at most `XTS_NET_FPS` frames per second (default 2000), copies included.
- **Wake window:** when no frame has left for `XTS_NET_WAKE_IDLE_MS` (default 50 ms; a measurement pause, the run
  start), the next frames go out at `XTS_NET_WAKE_FPS` (default 200) copies per second for as long as the silence
  lasted, at least `XTS_NET_WAKE_MS` (default 150 ms) and at most `XTS_NET_WAKE_MAX_MS` (default 500 ms; the run start
  gets the longest).
  A receiver on Wi-Fi in power save dozes after ~100 ms of silence and its access point drops what it cannot buffer
  until the receiver wakes at a beacon; the slow start lets it wake first. END `net.wake_windows` counts the windows
  (`make NET_WAKE_MS=0` turns them off).
- **Measurement fence:** no frame is started while the test core is measuring (`xts_xc_measuring()`).
- **Breadcrumbs:** a CRUMB is sent, and `xts_net_wait_sent` confirms it, before its case is released to the test core.

## Receiving rules (`tools/xts_recv.py`)
- **De-duplication:** frames are de-duplicated on `(run_nonce, seq)`. A frame whose CRC fails is dropped and counted.
- **Reassembly:** messages are rebuilt from `(run_nonce, msg)` once all `frag_cnt` fragments have arrived. Incomplete
  messages are reported, never written.
- **A run is complete only when all of these hold:**
  - a HELLO and an END were both received;
  - no seq is missing in `1..END.last_seq`;
  - the message counts per type match END;
  - the recomputed `rec_hash` matches END;
  - END reports no `send_failures` (messages the console could not queue).
- **Report:** everything is written to `receive.json` (`complete`, `missing_seq` ranges, `crc_errors`, `duplicates`,
  `incomplete_messages`, `status`). The receiver prints a one-line verdict.
- **Where loss happened:** `missing_context` gives, per missing range, `t_ms` (arrival of the frame before it, from the
  run's first frame), `hole_ms` (until the frame after it) and `quiet_before_ms` (longest silence in the second before);
  `null` for `--replay`. `kernel_drops` counts datagrams the PC's kernel dropped on the socket because its buffer was full
  (Linux `SO_RXQ_OVFL`; `null` when unknown). Loss with `kernel_drops` 0 shortly after a silence of >= 100 ms points at
  Wi-Fi power save on the receiving PC (`iw dev <wlan> set power_save off`).
- **Sessions:** a run whose HELLO/END has a `session` member is one pass; the receiver keeps listening until all
  `passes` of the session have ended (or one ended `crashed`/`hang`, or `--session-wait` seconds passed without a new
  pass) and writes `<out>/<console_tag>/session-<id>/session.json` plus `merged/` (xts_merge.py over the complete
  passes). The session is complete only when every pass is complete.
- **Output:** the run directory `<out>/<console_tag>/<run_nonce>/` holds `results.jsonl`, `results.csv`,
  `failures.jsonl`, `run.log`, `sysinfo.json`, `summary.json`, `temps.csv` (from BLOCK `temps`), `crash.json` (when a
  CRASH or HANG message arrived), `receive.json` and `capture.bin` (every unique valid frame, for `--replay`).
  This is the same layout the old USB sink produced, so `xts_merge`, `xts_schedmodel` and
  `check_records` work unchanged.
