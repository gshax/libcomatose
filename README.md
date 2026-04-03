# libcomatose

open source C library for interfacing with the hardware of Grandstream ATAs, with the goal of bypassing the proprietary userspace stack.

## what this does

the DSPG DVF99 series and DVF101 SoCs have a dual-processor architecture: an application processor (APP) running Linux, and a communication subsystem coprocessor (CSS) handling audio routing and DSP. they communicate over a message-passing framework called Cordless Manager (COMA).

libcomatose provides:
- **COMA transport** — AF_COMA socket abstraction for talking to CSS services
- **DUA protocol** — digital unit allocation for audio routing and DSP pipeline config
- **TDM assignment** — UMT bytecode generation for TDM channel-to-FIFO mapping
- **CSS debug shell** — access to the interactive debug console on the CSS coprocessor
- **TAPI wrappers** — ioctl interface (separate from COMA proper) for SLIC hardware control (line power, ringing, hook detect)

the eventual goal is to use the HT818 (and its siblings) as a general-purpose ATA without the proprietary SIP stack.

## status

**work in progress.** the stack is in a more or less functional state for exploratory and research purposes. experimental replacement of the CSS BGSC daemon is incomplete.

blobs that are currently still required:
- `css-loader` — CSS firmware
- TAPI stack kernel modules
    - `bsp_ht.ko` — platform support
    - `drv_silabs.ko` — low-level SLIC driver
    - `drv_tapi.ko` — TAPI interface
- `app_dsp` — CSS BGSC daemon, handles early DUA init and transcoding
    - annoyingly, dynamically linked against glibc

## building

```
# cross-compile library and tools
make CC=arm-none-linux-gnueabi-gcc AR=arm-none-linux-gnueabi-ar
# build replacement TDM kernel module
cd kmod && make
```

## tools

- `comatosed` — **main hardware support daemon** (runs alongside stock `app_dsp` by default; `--full-stack` for experimental BGSC reimplementation)
- diagnostics utilities
    - `css_shell` — CSS debug console client (interactive or `-c "command"`)
    - `voice_tap` — dump packets from RTP stream device nodes
    - `tapi_test` — interact with SLIC control device nodes
- research tools
    - `shm_dump` — dump CSS shared memory regions
    - `shm_decode` — decode CSS shared memory element descriptors
    - `shm_watch` — monitor shared memory element state changes in real time
    - `css_crash` — read and decode CSS crash dump from physical memory
    - `dua_enumerate` — connect to the DUA service and discover available units
- old proof of concepts
    - `dua_intercom` — CSS-only intercom between two FXS ports
