# libcomatose

open source C library wrapping the COMA/DUA/TAPI interfaces of Grandstream ATAs.

## building

native: `make`
cross-compile for ARMv7 by setting `CC` and `AR` (verify correct toolchain with user if unclear)
tools: `make tools` (after building the library)
kernel module: `cd kmod && make` (needs kernel source tree at materials/linux-4.9.0)

tools are statically linked for easy deployment to the device.

## architecture

three independent layers:
- `coma.c` — AF_COMA socket transport (PF_COMA=43, SOCK_SEQPACKET)
- `dua.c` — DUA protocol: shared memory init, message building, request/reply correlation, TDM assignment UMT bytecode
- `tapi.c` — TAPI ioctls for SLIC hardware control (/dev/fxsXX, /dev/slic_bsp)
- `kmod/comatose_tdm.c` — replacement TDM kernel module (only necessary for debugging TDM grant issues, deregisters stock BUG_ON handler)

header-only definition files (`*_defs.h`) can be used independently.

## critical implementation notes

### recv() vs recvmsg()
the COMA kernel socket returns EOPNOTSUPP on plain `recv()` but works correctly
with `recvmsg()`. always use `coma_recv()` which wraps `recvmsg()` internally.

### DUA sync vs async responses
the DUA sync response (cmd=0x81) only means "message received by CSS." the actual
operation result arrives later as an async callback (cmd=0x7f).

### DUA initialization sequence
the CSS firmware requires a specific init order before DUA commands work:
1. open `/dev/sharedmem`, ioctl `0xc0045302` to trigger CSS MMU mapping
2. mmap shared memory, zero it, set header (size at +4, offset 0x30 at +8)
3. connect AF_COMA socket to "dua" service
4. send `DUA_CMD_INIT_REQ` (cmd 0x01) with 5 params including shared memory pointer
5. send `DUA_CMD_APPL_INIT` (cmd 0x13)
6. now DUA is ready for unit operations

skipping steps 1-4 causes CSS panic. `dua_init_hw()` handles all of this.

### DUA response format
- `cmd=0x81`: synchronous response, matched by `sender_id`
- `cmd=0x7f`: async callback (event notification), has `0xdeadbeef` as params[0]
- responses are correlated to requests via `sender_id` field
- async callbacks must be drained between operations

### DUA variable-length data
large data blobs (e.g. TDM assignment UMT) use length-prefixed packing:
`[params...] [pad to 4-byte align] [uint32 data_size] [data bytes] [pad]`

### DUA session cleanup
the CSS hangs if a DUA session is left open with pending operations.
always close the session with `dua_close()`. for diagnostic runs, use
`tdm_diag` which closes cleanly instead of `dua_intercom` which blocks.

### CSS debug shell
the shell only works after DUA init (the I/O switch from UART to COMA
happens during InitReq/ApplInit). send characters one at a time as
2-byte messages: `{0x00, char}`.

### TAPI ioctl encoding
GS uses two encoding styles: simple `0x71xx` for plain int args, and full
`_IOW` encoding (`0x4001xxxx`/`0x4004xxxx`) for struct pointer args.
all ioctl numbers validated against `Get_IOCTL_Str()` in drv_tapi.ko.
see `tapi_defs.h` for the complete verified table.

### line feed enum values
`IFX_TAPI_LINE_FEED_ACTIVE = 0`, `STANDBY = 2`. both map to the same
ProSLIC register value (FWD_ACTIVE). standby is electrically a no-op.
use `DISABLED = 4` to actually cut power (ProSLIC OPEN).

### ringing
always set `RING_CADENCE_HR_SET` before `RING_START`. do NOT set line feed
around ringing — the driver manages it internally. touching line state
confuses the internal ring state machine and octuple_ring_sema.

### BSP major number
dynamically allocated on alpine (246), not hardcoded (122). the library
reads it from `/proc/devices` at runtime.
