# libcomatose

open source C library wrapping the COMA/DUA/TAPI interfaces of the Grandstream HT818 VoIP ATA.

## building

native: `make`
cross-compile for ht818: `make CC=arm-none-linux-gnueabi-gcc AR=arm-none-linux-gnueabi-ar`
tools: `make tools` (after building the library)
kernel module: `cd kmod && make` (needs kernel source tree at materials/linux-4.9.0)

tools are statically linked for easy deployment to the device.

## deploying to the ht818

the device is netbooted into alpine linux (`root@10.70.3.112`, password `alpine`).

deploy via scp:
```
scp tools/<binary> root@10.70.3.112:/tmp/
scp kmod/comatose_tdm.ko root@10.70.3.112:/tmp/
```

use `coma_tapi_full_reset` on the device to reload SLIC modules + CSS firmware.

## architecture

three independent layers:
- `coma.c` — AF_COMA socket transport (PF_COMA=43, SOCK_SEQPACKET)
- `dua.c` — DUA protocol: shared memory init, message building, request/reply correlation, TDM assignment UMT bytecode
- `tapi.c` — TAPI ioctls for SLIC hardware control (/dev/fxsXX, /dev/slic_bsp)
- `kmod/comatose_tdm.c` — replacement TDM kernel module (deregisters stock BUG_ON handler)

header-only definition files (`*_defs.h`) can be used independently.

## tools

- `dua_intercom` — full intercom setup: allocate all units, connect two FXS ports
- `dua_enumerate` — enumerate DUA unit types and elements
- `dua_probe` — step-by-step DUA init with diagnostics
- `tdm_diag` — minimal DUA setup + TDM assignment (exits cleanly, doesn't hang CSS)
- `tdm_brute` — TDM channel count brute forcer
- `css_shell` — CSS debug console client (`-c "command"` or interactive)
- `coma_test` — low-level COMA socket connectivity test
- `bsp_init` — BSP and SLIC initialization
- `ht818-exec.sh` — remote command execution via telnet (legacy, use ssh instead)

## critical implementation notes

### recv() vs recvmsg()
the COMA kernel socket returns EOPNOTSUPP on plain `recv()` but works correctly
with `recvmsg()`. always use `coma_recv()` which wraps `recvmsg()` internally.

### DUA sync vs async responses (IMPORTANT)
the DUA sync response (cmd=0x81) only means "message received by CSS." the actual
operation result arrives later as an async callback (cmd=0x7f). our current code
returns "OK" from the sync response and ignores the async callbacks, which masks
real errors. the stock `libcordless.so` has `p_duasync_*` wrappers that use a
mutex + `duasync_coma_wait()` to block until the async result arrives.

**current workaround:** generous `usleep()` delays between operations. the async
callback logging (stderr) shows real results: `p[3] = result * 0x100 + type`,
where negative results are DUA error codes.

**TODO:** implement proper sync wrappers that wait for the matching async callback
before returning, similar to stock's `p_duasync_UnitSetReq` pattern.

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

### line feed enum values
`IFX_TAPI_LINE_FEED_ACTIVE = 0` (not 1!), `STANDBY = 2`. the grandstream
fork swapped these compared to what you might expect.

### BSP major number
dynamically allocated on alpine (246), not hardcoded (122). the library
reads it from `/proc/devices` at runtime.

### TDM grant (CURRENT BLOCKER)
the CSS TDM instance has channel count = 0 and rejects all grants with -7.
the stock kernel module has BUG_ON in the nack handler — use our replacement
`comatose_tdm.ko` instead. the TDM channel count initialization path in the
kernel needs further investigation. see `research/claudes_notes/tdm_grant_investigation.md`.

## key source references

- CSS firmware analysis: `../research/claudes_notes/`
- kernel COMA source: `../materials/linux-4.9.0/drivers/staging/dspg/coma/`
- kernel TDM driver: `../materials/linux-4.9.0/drivers/staging/grandstream/tdm/gs-tdm.c`
- stock libcordless: `../materials/mtdblock/ht818base/app/lib/libcordless.so`
- stock app_dsp: `../materials/mtdblock/ht818base/usr/bin/app_dsp`
