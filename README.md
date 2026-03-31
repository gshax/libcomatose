# libcomatose

open source C library for controlling the audio subsystem of the Grandstream HT818 VoIP ATA, bypassing the proprietary `gs_ata` and `app_dsp` userspace.

the name is a somewhat morbid pun on **CO**rdless **MA**nager, abbreviated as COMA.

## what this does

the HT818 has a dual-processor architecture: an ARM application processor running Linux, and a DSPG CSS coprocessor handling audio DSP. they communicate over a message-passing framework called COMA.

libcomatose provides:
- **COMA transport** — AF_COMA socket abstraction for talking to CSS services
- **DUA protocol** — digital unit allocation for audio routing and DSP pipeline config
- **TAPI wrappers** — ioctl interface for SLIC hardware control (line power, ringing, hook detect)
- **TDM assignment** — UMT bytecode generation for TDM channel-to-FIFO mapping
- **CSS debug shell** — interactive console for the CSS coprocessor

the eventual goal is to use the HT818 as a general-purpose 8-port ATA with any telephony stack (e.g. Asterisk), without the proprietary SIP stack.

## status

**work in progress.** the full DUA audio routing stack is functional from a clean boot:
shared memory init, DUA service registration, unit allocation (all 8 FXS + VOIP),
UMT DSP pipeline configuration, connection creation and merging, SLIC line power.
the CSS debug shell works for interactive diagnostics.

## building

```
make                    # native build
make CC=arm-none-linux-gnueabi-gcc AR=arm-none-linux-gnueabi-ar  # cross-compile
make tools              # build test/debug tools
cd kmod && make         # build replacement TDM kernel module
```

## tools

- `css_shell` — CSS coprocessor debug console (interactive or `-c "command"`)
- `dua_intercom` — full intercom setup between two FXS ports
- `dua_enumerate` — discover available DUA units and elements
- `tdm_diag` — minimal DUA + TDM setup with clean shutdown
- `tdm_brute` — brute force TDM channel counts
- `bsp_init` — BSP and SLIC initialization
