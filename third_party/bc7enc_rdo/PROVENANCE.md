# bc7enc_rdo (vendored)

CPU BC7 encoder used to produce the 3840x2160 BC7 DDS home-screen backgrounds
(`sce_sys/pic0.dds`, `pic1.dds`) a native PS5 title shows.

## Provenance

- Source: <https://github.com/richgel999/bc7enc_rdo>.
- Pinned commit: `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`.
- Licence: MIT / public-domain dual licence (see `LICENSE`); LodePNG is
  zlib-licensed (notice in `lodepng.cpp`). The `bc7e.ispc` encoder (Apache-2.0)
  is **not** vendored and is compiled out (`-DSUPPORT_BC7E=0`), so only the
  CPU `bc7enc.cpp` path (`-C`) is used; no ISPC toolchain is needed.

Only the files required for the CPU encoder are vendored (no `.git`, no ISPC
sources, no test images).

## Build and use

`scripts/ps5/native/make-title.sh` builds the `bc7enc` tool with the host
`g++` and OpenMP (`libgomp`), then runs it as:

```
bc7enc -C -u4 -g -f -o <background-3840x2160.png> <out.dds>
```

`-C` CPU BC7, `-u4` quality, `-g` no unpacked PNG, `-f` DX10 DDS. bc7enc writes
`dwMipMapCount = 0`; the script patches the DDS header dword at offset 28 to
`1`, which is what the PS5 shell and ProsperoStore's `tools/validate-assets.sh`
require (a single 3840x2160 `BC7_UNORM` DX10 2D surface without mipmaps).
