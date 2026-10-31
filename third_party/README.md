# Third-party code

Files vendored verbatim into this repository. Do not modify them in place; update by replacing the file with a newer upstream release and updating this table.

| Path | Project | Version | Source URL | SHA-256 | License |
|------|---------|---------|------------|---------|---------|
| `nlohmann/json.hpp` | [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` | MIT |
| `doctest/doctest.h` | [doctest](https://github.com/doctest/doctest) | 2.4.12 | https://raw.githubusercontent.com/doctest/doctest/v2.4.12/doctest/doctest.h | `94029a7d32da24a56249658147dbd2b33ff0b9ed665295cbbaf19aafff5b0ced` | MIT |
| `stb/stb_image_write.h` | [stb](https://github.com/nothings/stb) | v1.16 (commit `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`) | https://raw.githubusercontent.com/nothings/stb/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5/stb_image_write.h | `cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05` | MIT or public domain |

`webkit/` is an upstream WebKit checkout fetched separately; it is ignored by git and not vendored.
