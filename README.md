# Fern-RTLTCP

An rtl_tcp input module for [FernSDR](https://github.com/Steven9101/FernSDR):
listen to an RTL-SDR, or any SDR exposed through an `rtl_tcp`-compatible
server, local or over a tunnel, with a read-only mode for OpenWebRX
`rtltcp_compat` feeds.

SPDX-License-Identifier: GPL-2.0-or-later

## What it does

- Connects to an `rtl_tcp` endpoint (`host`/`port`), reads the 12-byte header,
  and relays the `cu8` samples to FernSDR as `u8` IQ.
- `control = true` (default): sends tuning commands — sample rate, centre,
  gain, ppm and bias tee — on `open`, and applies live `gain`/`ppm`/`bias_tee`
  on `set`.
- `control = false`: sends nothing, for read-only feeds such as OpenWebRX
  `rtltcp_compat`, where the centre and rate are fixed by the owner.
- Reconnects for up to 30 seconds when the stream drops.

## Settings

| key | type | default | live | meaning |
|---|---|---|---|---|
| host | string | 127.0.0.1 | no | rtl_tcp host, e.g. 127.0.0.1 through a tunnel |
| device | string | "" | no | a name for this endpoint, to tell bands apart |
| port | number | 1234 | no | rtl_tcp port |
| gain | string | auto | yes | `auto`, or dB |
| ppm | number | 0 | yes | frequency correction in ppm |
| bias_tee | boolean | false | yes | bias tee on |
| control | boolean | true | no | send tuning commands; off for read-only feeds |

## Build

```sh
make              # native build, build/rtltcp
make static       # fully static, build/static-x86_64/rtltcp
make package      # dist/rtltcp-<version>-linux-<arch>.fernmod
make test         # unit + fake rtl_tcp integration + cli/package checks
```

`make package` uses the `tools/mkfernmod.py` and `tools/check_fernmod.py`
helpers included here, kept in sync with the other FernSDR modules
(originally from [Fern-RTLSDR](https://github.com/Steven9101/Fern-RTLSDR)).

The module is a single C++17 program with no dependencies, linked statically.
It talks to FernSDR over the module contract in FernSDR's `docs/MODULES.md`
(API 1): commands on fd 0, samples on fd 1, log on fd 2, events on fd 3.
