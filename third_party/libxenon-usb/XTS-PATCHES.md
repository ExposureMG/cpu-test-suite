# Patched libxenon USB mass-storage driver (tinyehci)

## What this is
A copy of libxenon's `drivers/usb/tinyehci/` translation unit: `tinyehci.c`, which `#include`s `ehci.c`, which
`#include`s `ehci-mem.c`, `usb2.c` and `usbstorage.c` (plus the headers). libxenon compiles exactly this into
`libxenon.a(tinyehci.o)`; XTS compiles the patched copy into `$(BUILD)/console/usbfix/tinyehci.o` and links it ahead of
`-lxenon` when `USB_FIX=1` (the default). `usb_os.c` (DMA helpers, `USB_Alloc`) stays in libxenon.

- Source: `~/Projects/libxenon/libxenon` at `f345a2b` ("gcc 16 and binutils 2.32 vmx128 patches").
- `upstream/` is the untouched copy. Compiled with libxenon's flags (`ports/xenon/Makefile`) it gives the same 80
  defined symbols and the same section sizes (text 14502, data 148, bss 2382) as `/usr/local/xenon/usr/lib/libxenon.a
  (tinyehci.o)`, i.e. the installed library was built from this source.
- `xts-usb.patch` is the whole difference (`ehci.c`, `usbstorage.c`; `usbstorage.c` keeps its CRLF line ends).
- Licenses: `ehci.c`/`ehci-mem.c` (derived from Linux ehci-hcd) and `tinyehci.c`/`usb2.c` are GPL v2 or later (see
  their headers); `usbstorage.c` is zlib-style (header); libxenon's own `LICENSE` is copied as `LICENSE-libxenon`.
  XTS links libxenon already, so nothing changes license-wise by vendoring.

## Link
`Makefile`: `USB_FIX ?= 1`. With 1, `$(CON)/usbfix/tinyehci.o` is an input object before `--start-group -lxenon ...`;
it defines every symbol of `libxenon.a(tinyehci.o)`, so the archive member is never pulled in. If something ever
needed a symbol only the archive member had, the member would be pulled in and every one of its 80 symbols would
collide: `ld` fails with "multiple definition" (checked: linking both objects together fails). After every link the
Makefile also reads the map and fails the build if `libxenon.a(tinyehci.o)` was linked with `USB_FIX=1` (or not linked
with `USB_FIX=0`), if a libfat other than the vendored one or `libxenon.a(except.o)` got in. The input sections of
`ehci_do_urb`, `__cycle` and `USBStorage_Write_Sectors` in `xts-<profile>.map` name `usbfix/tinyehci.o`.
`sysinfo.json` records the driver in `"usb_driver"`. `USB_FIX=0` links libxenon's unpatched driver (the disc guard is
active either way).

## The bugs and the patches

### usb-0: every halted transfer executed `trap` (ehci.c `qh_end_transfer`) — the likely crash
```c
for (qtd = qh->qtd_head; qtd; qtd = qtd->next) { ... if (halted) error = -1; }
if (error) qtd->urb->actual_length = error;      /* qtd is NULL here */
```
The loop ends with `qtd == NULL`. gcc 16 sees the NULL dereference as undefined behaviour and compiles the error path
to `lwz r9,60(r9)` (r9 = 0) followed by **`trap`** (disassembly of the installed `libxenon.a(tinyehci.o)`, function
`ehci_do_urb` +0x294/+0x298; the only `trap` in the object). So every qTD that halts — a STALL handshake on a bulk
endpoint, babble, a transaction error after the controller's 3 retries — raises a program interrupt inside
`ehci_do_urb` instead of returning an error. In XTS that goes through libxenon's handler to the crash path ("XTS CRASH
vector 0700", SRR0 inside `ehci_do_urb`), in the middle of whatever libfat was writing: exactly the shape of the
incident (the `xts` directory entry on the stick, its cluster still free in the FAT). The patch keeps the urb pointer
from the list head and sets `urb->actual_length = -1` as intended; `ehci_do_urb` then returns -1 and `__cycle` does reset
recovery. The patched object contains no `trap`.

### usb-1: `__cycle` kept transferring with stale state after an error or short transfer (usbstorage.c)
Bulk-only transport is CBW -> data phase -> CSW. In the original data loops:
- write: `if (retval < 0) continue;` and, for a short transfer, `retval = USBSTORAGE_EDATARESIDUE; continue;` — the
  `continue` restarts the inner `while (len > 0)` loop, not the outer retry loop. The same chunk is sent again without
  reset and without a new CBW, although the device already took part of it. The device's notion of where the data
  phase ends and the host's drift apart; the device can take the NEXT CBW (31 bytes, "USBC") as sector data and write
  it to the medium — the incident's "sector 4 of the root directory starts with a CBW (SCSI WRITE(10))".
- read: `len -= retval; buffer += retval; if (retval != thisLen) { retval = -1; continue; }` — after a short packet the
  loop asks for the rest; the next transfer can return the device's CSW (13 bytes, "USBS") or the start of the next
  phase as data, `retval` becomes positive again and the command "succeeds" with the buffer partly unfilled (stale
  contents of libfat's cache page) or filled with protocol bytes.
- `if (retval < 0) continue;` re-issues a failing transfer forever unless it is -ENODEV/-ETIMEDOUT.

Patch: one attempt = CBW, data, CSW. Any data-phase error, any short OUT transfer and (for READ(10)/WRITE(10)) any short
IN transfer `break`s out and `continue`s the OUTER loop, which does the bulk-only reset recovery (class reset + clear
halt on both endpoints) and starts again with a fresh CBW and `buffer`/`len` rewound; at most
`USBSTORAGE_CYCLE_RETRIES` (10) times, then the existing failure path (-ENODEV / unplug). READ(10)/WRITE(10) return
success only if every byte moved, the CSW is valid (signature, tag — `__read_csw` already checked those), CSW status is
0 and the data residue is 0. A phase error (status 2) always gets reset recovery. Other commands (INQUIRY, READ
CAPACITY, REQUEST SENSE, TEST UNIT READY) may end the IN phase early with a short packet, as bulk-only allows: the CSW is
read and its status returned, as before.

### usb-2: CSW status of READ(10)/WRITE(10) was ignored
`if (retval > 0 && status != 0) retval = USBSTORAGE_ESTATUS;` — `__cycle` returns 0 on success, so this never fired: a
command the device reported as FAILED returned success. Now `retval >= 0` (and usb-1 already refuses status != 0).

### usb-3: endless retry loops in `USBStorage_Read_Sectors` / `USBStorage_Write_Sectors`
`for (retry = 0; retry < 16; retry++) { if (retry > 12) retry = 12; ... }` never ends, and once `retval < 0` without the
device dropping off, `if (retval >= 0) retval = USBStorage_Write(...)` never retries the transfer: a persistent error
hung the caller (libfat, with XTS's journal open) forever. Now at most `USBSTORAGE_SECTOR_ATTEMPTS` (4) attempts, each a
full `__cycle` with its own reset-recovery retries; between attempts a dropped device is re-enumerated by
`unplug_procedure()` as before; then FALSE, so libfat (and the disc guard) see the error.

Also: `USBStorage_Inserted` (unused) marked `__attribute__((unused))` so the copy builds without warnings.

## What is not verified
Nothing here has run on hardware yet. Unknown: whether the user's stick ever STALLs or short-transfers (usb-0/usb-1
explain the incident, but which one fired is not proven — a crash screen with SRR0 inside `ehci_do_urb` would confirm
usb-0); whether reset recovery after a short read is fast enough on that stick; and whether some device relies on the
old behaviour (e.g. returns fewer INQUIRY bytes in a way the new code handles differently). The host replay
(`make host-fatreplay`) models the driver's failure modes at the DISC_INTERFACE level, not the USB protocol itself.

## Upstreaming
The four patches are independent and small (`xts-usb.patch`, ~200 changed lines). usb-0 and usb-2 are plain bug fixes;
usb-1 changes the retry structure (worth testing with several sticks); usb-3 changes "hang forever" into "return an
error after ~4 x 10 reset attempts".
