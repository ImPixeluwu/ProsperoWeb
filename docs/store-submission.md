# Store submission (ProsperoStore)

Preparation for listing ProsperoWeb in the ProsperoStore homebrew catalog. Nothing has been submitted: no pull request, reservation or message has been sent anywhere. Everything below was read from public sources on 2026-10-31; links to GitHub files are pinned to the commit that was read.

## What ProsperoStore is

- **ProsperoStore** is a native PS5 homebrew app store (title `PPSA99000`, GPL-3.0) by AngelXex (`darkxex` on GitHub), formerly `blackbearreloaded/ProsperoStore`: <https://github.com/darkxex/ProsperoStore>. It installs, updates and removes apps from a catalog. Latest release at the time of writing: `v1.000.116` (2026-10-29), shipped as `PPSA99000.zip`.
- **The catalog** it reads is a separate repository, <https://github.com/darkxex/ps5-homebrew-catalog> (GPL-3.0), published as the website and store API at <https://prosperostore.net>. 164 records at the time of writing; none is a web browser.
- Submission is a **GitHub pull request** to the catalog that adds one file, `apps/<TITLEID>.json`, opened from the account that owns the app's repository (a PR from another account waits for manual approval) ([CONTRIBUTING.md](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/CONTRIBUTING.md)). The [submit page](https://prosperostore.net/submit/) can draft the record from a GitHub release ([docs/submitting.md:109](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/submitting.md#L109)). CI checks the record, a maintainer reviews it against [docs/review-policy.md](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/review-policy.md) and merges.

### Requirements (catalog `docs/submitting.md`, `docs/metadata.md`)

| Requirement | Rule | Source |
| --- | --- | --- |
| App type | A **native PS5 app** installed as a title with its own title ID (`eboot.bin` and `sce_sys/param.json`). **ELF payloads**, PS4 packages, backports, ROMs and web pages are not listed. | [submitting.md:9-11](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/submitting.md#L9-L11) |
| Artifact | One `.zip` of the `<TITLEID>/` folder (with `eboot.bin` and `sce_sys/`) at the top of the archive, attached to a published GitHub release (pre-releases accepted). `.ffpkg`/`.ffpfsc` images and fPKGs are not accepted. Installed to `/data/homebrew/<TITLEID>/` and registered by ShadowMountPlus. | [artifact-formats.md:8](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/artifact-formats.md#L8) |
| Record | Exactly twelve fields: `titleid`, `name`, `kind` (`app`/`game`/`tool`), `category`, `description` (one plain sentence, max 200 chars), `license` (SPDX, must agree with what GitHub detects for the repo), `author`, `version` (= the release tag, optional leading `v` dropped), `source_repo`, `artifact_url` (pinned release asset), `sha256`, `icon_url`. | [metadata.md:13-24](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/metadata.md#L13-L24) |
| Category | One of eleven keys; no browser shelf. `utilities` ("Files, launchers, backups and system helpers") is the closest. | [metadata.md:54](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/metadata.md#L54) |
| Icon | Square PNG/JPEG/WebP at a pinned HTTPS URL, 512x512 recommended (256x256 minimum), max 2 MiB; "exactly the PS5's own `sce_sys/icon0.png`". | [submitting.md:33](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/submitting.md#L33) |
| Title ID | Four uppercase letters + five digits, first come first served. `PPSA` is usual; custom prefixes are accepted except `CUSA` and `NPXS`; `PPSA99999` (template default) is rejected. | [submitting.md:50-66](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/submitting.md#L50-L66) |
| Version | `contentVersion` in `sce_sys/param.json`, format `NN.NNN.NNN`, raised in every release; the catalog reads it from the repo at the tag, or from the ZIP. | [versioning.md:21](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/versioning.md#L21) |
| Screenshots | None: the record has no screenshot field. Release notes come from the GitHub release description. | [metadata.md](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/metadata.md) |
| Reservation | A record with `artifact_url`, `source_repo`, `icon_url`, `sha256` set to `null` reserves the title ID ("Coming soon"); max 5 per account, refresh every 180 days. | [submitting.md:163](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/submitting.md#L163) |

### Home screen assets of a native app

From ProsperoStore's own build ([sce_sys/param.json](https://github.com/darkxex/ProsperoStore/blob/22d5a44a053b4dad4f3a5867fe39d262a80a1b8c/sce_sys/param.json), [tools/validate-assets.sh:34,52](https://github.com/darkxex/ProsperoStore/blob/22d5a44a053b4dad4f3a5867fe39d262a80a1b8c/tools/validate-assets.sh#L34), [tools/init-project.sh:33-55](https://github.com/darkxex/ProsperoStore/blob/22d5a44a053b4dad4f3a5867fe39d262a80a1b8c/tools/init-project.sh#L33-L55)):

- `sce_sys/icon0.png`: 512x512 PNG, required.
- `sce_sys/pic0.dds` (selection background) and `sce_sys/pic1.dds` (launch background): optional, both or neither, **3840x2160 BC7_UNORM DX10 DDS without mipmaps** (converted with Microsoft's `texconv`). PNG backgrounds are not what the system reads in a native app folder.
- `param.json`: `titleId`, `contentId` (`XX0000-<TITLEID>_00-<16 uppercase letters or digits>`), `conceptId` (the title ID's digits), `contentVersion`, `masterVersion` (`01.00`), `applicationCategoryType` (`0`, as ProsperoStore and websrv's own loader title use; `65536` is a media app), `localizedParameters`.

### How websrv uses the tile today

websrv ([ps5-payload-dev/websrv](https://github.com/ps5-payload-dev/websrv) at `7748df9d`) lists `/data/homebrew/<dir>/` entries that contain `homebrew.js` or `eboot.elf` (`assets/localStore.js:25`) and shows `<dir>/sce_sys/icon0.png` as the tile picture (`assets/main.js:41`). It reads nothing else from `sce_sys/`: no `param.json`, no backgrounds. Its own loader title uses `FAKE00000` / `IV9999-FAKE00000_00-HOMEBREWLOADER00` (`src/ps5/hbldr.c:58-59`).

### ShadowMountPlus

[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) (at `30c5a2b7`), which the catalog relies on to register installed folders, treats a title ID with any four-letter prefix other than `CUSA`, `PPSA` and `NPXS` as a **homebrew ID** (README.md:40-41), and registers a folder only when it holds `eboot.bin` (or `eboot2.bin`) and `sce_sys/param.json` (README.md:435).

## Chosen identifiers

All in [`assets/app.json`](../assets/app.json), the single source; `scripts/ps5/package.sh` generates `sce_sys/param.json` from it.

| Field | Value | Why |
| --- | --- | --- |
| Title ID | `PRWB00001` | Unique by construction: no catalog record uses the `PRWB` prefix (164 records checked at catalog commit `0bd485a`: 162 `PPSA`, one `CWEN`, one `FAKE`), a GitHub code search for `PRWB00001` finds nothing and one for `PRWB0` only unrelated hashes and tokens. A custom prefix cannot collide with a retail PS5 title (`PPSA` is Sony's retail namespace, and the catalog already holds 120+ `PPSA9xxxx` homebrew IDs). ShadowMountPlus classifies it as a homebrew ID. Development builds under the earlier name used `PRSB00001`; it was never published. |
| Content ID | `IV9999-PRWB00001_00-PROSPEROWEB00000` | Standard layout. `IV9999` is the homebrew publisher code websrv uses; ProsperoStore's `UP9000` is Sony's own first-party publisher code and is avoided. Suffix: `PROSPEROWEB` padded with `0` to 16 characters. |
| Concept ID | `00001` | The title ID's digits, as in ProsperoStore's `init-project.sh`. |
| Version | `0.1.0` (release tag `v0.1.0`) | |
| contentVersion | `01.001.000` | Mapping `0.M.P` -> `01.MMM.PPP` (0.1.1 -> `01.001.001`, 0.2.0 -> `01.002.000`; 1.0.0 -> `02.000.000`), monotonic as the catalog requires. |
| masterVersion | `01.00` | |
| Store kind / category | `app` / `utilities` | No browser category exists; the operator's "Utilities/Web Browser" maps to `utilities`. |
| License | `GPL-3.0-or-later` | The catalog accepts it when GitHub detects `GPL-3.0` (its check matches `GPL-3.0-` as a prefix). |

Trade-off of the custom prefix, for the operator to confirm before the first public release (a title ID is permanent: changing it later makes the console treat the app as a new title with separate data): the store API lists non-`PPSA` apps in a separate `shared` array, which ProsperoStore before 1.000.110 does not show and 1.000.101-1.000.109 fails on ([api.md:75](https://github.com/darkxex/ps5-homebrew-catalog/blob/0bd485acc5e17d9ed11248652d5f1e447435c82a/docs/api.md#L75)). Current releases (1.000.116) show it. The alternative is a free `PPSA9xxxx` ID, at the cost of sharing Sony's retail namespace.

## Assets

| File | Size | Format | Used by |
| --- | --- | --- | --- |
| `assets/icon0.svg` | 512x512 | SVG source | |
| `assets/background.svg` | 1920x1080 | SVG source (wordmark in Noto Sans Bold, OFL) | |
| `assets/sce_sys/icon0.png` | 512x512 | PNG, RGB | websrv tile, catalog `icon_url`, native app |
| `assets/sce_sys/pic0.png`, `pic1.png` | 1920x1080 | PNG, RGB | packaged in the tile for completeness; nothing reads them yet |
| `build-dist/assets/background-3840x2160.png` | 3840x2160 | PNG (not committed) | source for `pic0.dds`/`pic1.dds` of the native title (`make-title.sh`) |
| `docs/screenshots/store-icon-preview.png` | 800x560 | PNG | icon at 512 and 128 |
| `docs/screenshots/store-background-preview.png` | 1280x720 | PNG | background preview |

`scripts/assets/render.sh` renders all of them reproducibly (byte-identical reruns) with `rsvg-convert` and ImageMagick, with fontconfig limited to the checksum-verified Noto Sans from `third_party/dl/fonts/`. The artwork is original: a globe with a moon on a tilted orbit (Prospero is a moon of Uranus) in the chrome palette from `src/chrome-ui/chrome.css`; no Sony marks, logos or controller-button shapes.

Screenshots for the README or the GitHub release (the catalog takes none). Without third-party marks: `docs/screenshots/host-demo-chrome.png`, `host-chrome-events.png`, `host-osk-page-field.png`, `host-video-mp4.png` (Big Buck Bunny, CC BY), `host-video-hls.png` (Mux test stream). Avoid `host-page-wikipedia.png`, `host-osk.png`, `host-menu-bookmarks.png`, `host-fullscreen.png` (a Wikipedia article about the PS5, with its product imagery) and `host-drm-notice.png` (names Netflix). These are host (WebKitGTK) shots; real console shots can be taken later with the browser's screenshot feature.

## Draft record

[`docs/store/PRWB00001.json`](store/PRWB00001.json) is a valid catalog **reservation** record (release fields `null`), ready to be proposed as `apps/PRWB00001.json` if the operator decides to reserve the title ID. For the first release it becomes:

```json
{
  "titleid": "PRWB00001",
  "name": "ProsperoWeb",
  "kind": "app",
  "category": "utilities",
  "description": "A WebKit web browser for the PS5, driven by the controller, with tabs, bookmarks and video playback.",
  "license": "GPL-3.0-or-later",
  "author": "kvaps",
  "version": "0.1.0",
  "source_repo": "https://github.com/kvaps/ProsperoWeb",
  "artifact_url": "https://github.com/kvaps/ProsperoWeb/releases/download/v0.1.0/PRWB00001.zip",
  "sha256": "<sha256 of PRWB00001.zip, lowercase hex>",
  "icon_url": "https://raw.githubusercontent.com/kvaps/ProsperoWeb/v0.1.0/assets/sce_sys/icon0.png"
}
```

## What still needs the operator

1. **A native app build (blocker).** The catalog lists only native titles (`eboot.bin` + `sce_sys/param.json` in a `PRWB00001/` folder); ProsperoWeb today is an ELF payload started by websrv's loader title (`FAKE00000`) through `homebrew.js`, which is exactly what the catalog excludes. Listing needs a `PRWB00001/eboot.bin` that runs the browser as its own title, plus a replacement for the websrv-hosted spawn broker the browser uses to start its WebProcess and NetworkProcess (docs/ps5-runtime.md). This is engineering work, not packaging.
2. **Public repository state**: as of 2026-10-31 `kvaps/ProsperoWeb` shows no license to GitHub's detector and has no releases. The `license` check needs `LICENSE` on the default branch (`main`); the record needs a published release `v0.1.0` with `PRWB00001.zip` attached (the release workflow attaches it, see the README).
3. **Confirm the title ID** (`PRWB00001` vs a `PPSA` ID, see above) and whether to reserve it now.
4. **Real console screenshots** for the README and release notes.
5. **The pull request itself**, from the `kvaps` account, after review of the record text.

`pic0.dds`/`pic1.dds` are no longer open: `scripts/ps5/native/make-title.sh` encodes them with the vendored `bc7enc_rdo` (docs/native-title.md).
