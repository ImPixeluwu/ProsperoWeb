# ps5-native-tool (vendored)

The open-source PS5 native-title toolchain used to turn a native-app ELF into a
signed development FSELF `eboot.bin` and to regenerate the clean-room
`sce_module/libc.prx` runtime companion.

## Provenance

Vendored from BlackBearReloaded's `ps5-native-app-boilerplate`, as shipped
inside the ProsperoStore repository:

- Source: <https://github.com/darkxex/ProsperoStore>, directory `tooling/` and
  `runtime/libc.prx.sha256`.
- Pinned commit: `22d5a44a053b4dad4f3a5867fe39d262a80a1b8c` (2026-10-29).
- Licence: GPL-3.0-or-later (see `LICENSE`).

Only the host tooling needed to build a title is vendored (the converter, the
clean-room `libc.prx` emitter, the native application CRT, the PS5 PIE linker
script, the symbol-visibility map and the two runtime manifests). No Sony SDK,
firmware or proprietary runtime is included: the `libc.prx` is generated from
the two text manifests under `tooling/native/runtime/` by a clean-room emitter,
and `tools/rebuild-libc.sh` upstream gates it against a byte-for-byte digest.

## What it produces

`scripts/ps5/native/make-title.sh` builds two host binaries from this source
with the system `clang++`/`clang-18` and the host `zlib`:

- `ps5-native-tool` — converts an LLVM PIE (linked with `ps5-pie.ld`) to the
  PS5 ELF layout (`e_type=0xFE10`, process-parameter block, RELRO/16 KiB
  congruence), then wraps and signs it as a development FSELF
  (magic `0x1D3D154F`, authority `0x3100000000000002`, program type `1`,
  module SDK `0x02000009`, companion SDK `0x08050001`).
- `libc-builder` — emits the clean-room `libc.prx` from the manifests.

Reproducibility check performed when vendored (both digests match the pinned
ProsperoStore release exactly):

```
raw    libc.raw.elf  8ee6e124993e1af26420cb455890fd002f5d6c7e78883c860ce45734e7d002bb
signed libc.prx      e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036 (1284674 bytes)
```
