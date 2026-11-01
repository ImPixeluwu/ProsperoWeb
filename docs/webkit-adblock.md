<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# WebKit ad blocking: prior art and a YouTube recipe

Researched 2026-11-01. This note studies how the two relevant WebKit/Safari-class blockers work — uBlock Origin Lite (uBOL) and wBlock — and translates their YouTube logic into something ProsperoWeb can actually ship on its WebKit port. It is research, not a design sign-off.

Pinned sources (shallow, read-only clones):

- uBOL: `github.com/uBlockOrigin/uBOL-home` @ `ade83cb83f4b9dd238726668912664c81ade90e6`
- uAssets (the filters + scriptlets uBO/uBOL consume): `github.com/uBlockOrigin/uAssets` @ `c98eed42249992fa339ade8952b6d24e52af46a6`
- wBlock (Safari blocker): `github.com/0xCUB3/wBlock` @ `9dd89fd6dc922d2823140533abbd7525bc04a81e`

## 0. Our starting point (what the port actually gives us)

Two facts constrain everything below, and they are already recorded in our own converter header `src/core/include/prosperoweb/core/adblock.h`:

- `ENABLE(CONTENT_EXTENSIONS)` is **off** in our WebKit build, so `WKContentRuleList` / `WKUserContentExtensionStore` is unusable on the console (header lines 59-65, the "Userscript filter set" section). The converter's `convert()` path still emits WebKit content-rule-list JSON, but only the build-time staging tool can compile it; on device we fall back to `build_filter_set()`, which extracts whole-host blocks + generic/domain CSS for an **injected userscript** to enforce.
- We own the **NetworkProcess**. That is the lever Safari blockers do not have: we can make per-request decisions (block/allow/rewrite) that a declarative rule list cannot.

So our real mechanisms are: (a) host/URL blocking in NetworkProcess, (b) `WKUserScript` injection for CSS element-hiding and for JS that proxies page APIs. We are not limited to the declarative model — which matters a lot for YouTube.

## 1. uBlock Origin Lite (uBOL) — the declarative model

uBOL "operates entirely declaratively, meaning no permanent process is required for filtering. The browser handles CSS/JS injection" (uBOL `README.md`). It is MV3 on Chromium and a Safari App Extension on Apple platforms (`dist/safari/xcode/...`). The default set is uBO's built-in lists + EasyList + EasyPrivacy + Peter Lowe's list (`README.md`).

How a filter list becomes rules (two output channels):

- **Network rules → declarative rules.** `||host^`, path/regex patterns, `$`-options and `@@` exceptions compile to `declarativeNetRequest` rules on Chromium and to the equivalent Safari content-blocker JSON (`block` / `ignore-previous-rules`, `if-domain`/`unless-domain`, `resource-type`). The generated sets live in `chromium/rulesets/main/*.json` (one file per list: `easylist.json`, `easyprivacy.json`, `ublock-filters.json`, regional `*-0.json`, etc.). This is a 1:1 analogue of what our `convert()` already emits.
- **Cosmetic `##selector` → CSS injection**, and **`##+js(...)` scriptlets → injected JS content scripts**. These are NOT declarative-network rules. uBOL bundles them as content scripts under `chromium/rulesets/scripting/scriptlet/main/*.js` (e.g. `ublock-filters.js`) plus a per-domain map in `chromium/rulesets/scriptlet-details.json`. That map tags each ruleset with the execution world: `ublock-filters` runs `"MAIN": ["*"]` and `"ISOLATED": ["*"]` (scriptlet-details.json head). The **MAIN world** tag is the important part — see the YouTube recipe.

What it CANNOT do declaratively: scriptlets (they are JS, injected as content scripts, not DNR), procedural/extended cosmetics (`:has`, `:matches-css`, `:xpath`, `:style`), and anything that must rewrite a response body or make a per-request decision. Apple's content-blocker JSON only offers `block`, `block-cookies`, `css-display-none`, `ignore-previous-rules`, `make-https` (+ `redirect` in newer WebKit), with triggers `url-filter`, `if/unless-domain`, `resource-type`, `load-type`, `if/unless-top-url`. No body rewriting.

**YouTube coverage, honestly:**

- Page/feed ads (home grid, sidebar, masthead promos): partly declarative (network blocks for some endpoints) but the reliable removal is a **scriptlet** that prunes the JSON response, not a network rule. In uBOL this prune ships in the MAIN-world `ublock-filters.js` bundle (grep for `adPlacements`, `playerResponse.adPlacements`, `adSlots` confirms the json-prune args are present there).
- Pre-roll / in-stream video ads: the declarative network set does **not** remove them, because YouTube serves ad and content video from the same `*.googlevideo.com` origin (often now server-stitched into one stream). uBOL kills classic pre-roll only via the json-prune scriptlet running in the page. A pure-declarative install does not stop YouTube video ads.

Net: uBOL's YouTube result is carried by its scriptlet, which on Safari rides in the App Extension's MAIN-world injection — exactly the capability we must reimplement ourselves.

## 2. wBlock — content blockers + a scripts Web Extension

wBlock is a Safari blocker for macOS/iOS/iPadOS/visionOS (`README.md`). Architecture (`README.md` "How it works" + `Adblock_Comparison.md`):

- It converts AdGuard-compatible lists with **AdGuard SafariConverterLib 4.3.0** into **five content blockers** (Ads, Privacy, Security, Foreign, Custom), each capped at Safari's 150,000 compiled rules; "Safari compiles and applies them independently of the app" (`README.md`). This is the same `WKContentRuleList` ceiling our converter notes (150k, `adblock.h` line 25-28).
- "A Safari blocker is not uBlock Origin on Firefox... the blocker can't make a call on each request. Cosmetic filtering, scriptlets, element picking, YouTube fixes, and per-site switches all need a separate Safari Web Extension" (`Adblock_Comparison.md:5`). That extension, **wBlock Scripts**, "runs AdGuard scriptlets and cosmetic CSS... and hosts the userscript engine" (`Adblock_Comparison.md:46`). Site-disable is a single coarse `ignore-previous-rules` with `url-filter ".*"` + `if-domain` (`Adblock_Comparison.md:148-160`). "Safari's content blocker API can block a request but can't rewrite its URL" (`Adblock_Comparison.md:165`).

**How wBlock handles YouTube video ads specifically:** not through network rules. Two layers:

1. The AdGuard/uBO json-prune scriptlet delivered through its lists and run by wBlock Scripts (same mechanism as uBOL).
2. Optional **userscripts** from its catalog (`wBlockCoreService/BuiltInUserScripts.swift`): "Tube Cleaner" (`tubeCleanerURL`, from `0xCUB3/wBlock-userscripts`) which "gives YouTube Safari-native controls, chapters, SponsorBlock skipping, picture-in-picture, background playback, quality selection, and audio-only mode" (lines 42-50), plus "Player Cleaner", Return YouTube Dislike, DeArrow, and AdGuard Extra. Note `retiredYouTubeAdBlockURL` (line 39-40) — a standalone YT ad script they shipped and then retired, a sign that pure YT ad scripts are high-maintenance.

Takeaway: wBlock does not have a secret network endpoint that kills pre-roll. Its YouTube ad removal is the same page-world JSON-pruning scriptlet logic, optionally augmented by a native-player userscript (Tube Cleaner) that side-steps the ad-serving player entirely. The native-player idea is interesting for us because our port already does hardware video decode in the native title.

## 3. The YouTube anti-ad scriptlets (what they actually do)

The authoritative filters are in uAssets. There are two generations.

**(a) Stable, production — `json-prune` of the player response.** `filters/legacy.txt:165`:

```
youtube.com,youtubekids.com,youtube-nocookie.com##+js(json-prune, playerResponse.adPlacements playerResponse.playerAds playerResponse.adSlots adPlacements playerAds adSlots legacyImportant)
```

What `json-prune` does (uBOL `chromium/js/resources/json-prune.js`): it installs a proxy on `JSON.parse` (`proxyApplyFn('JSON.parse', ...)`), and on every parse it deletes the listed property paths from the result before returning it (`objectPruneFn`). There is a sibling `json-prune-fetch-response` / `json-prune-xhr-response` that does the same to `fetch`/`XHR` response bodies. For YouTube: when the player config is parsed, `adPlacements`, `playerAds`, `adSlots` are stripped, so the player never schedules the pre-roll/mid-roll it would have read from those fields. Page-feed ad variants prune `...richItemRenderer.content.adSlotRenderer` (`experimental.txt:25`, `legacy.txt`/`annoyances-others.txt`). This is the whole trick for classic (client-scheduled) YouTube ads, and it is simple.

**(b) Experimental — server-stitched ads (SSAP).** `filters/experimental.txt:11-30`. YouTube increasingly stitches the ad into the same media stream (`debug_info` starts with `"SSAP, AD"`), so pruning placements is not enough. The experimental scriptlet (`rpnt, script, /serverContract.../`) rewrites the player bootstrap and does, in essence:

- Spoof `ytcfg.data_.INNERTUBE_CONTEXT.client.userAgent` with marker tokens (`"instream"`, `"channel"`, `"inline"`, `"adunit"`, `"eafg"`), and via `trusted-edit-inbound-object` on `JSON.stringify` inject fields like `isInlinePlaybackNoAd:true` / `clientScreen:"ADUNIT"` into the outgoing `/youtubei` request body so the server returns an ad-free variant (`experimental.txt:19-21`).
- Detect the fake "ad is buffering" state (`buffer_health_seconds === "0.00 s"` && `resolution === "0x0"`) via `getStatsForNerds()` and, when an ad is detected, call `player.seekTo(duration)` to jump to the end of the stitched ad and `loadVideoById(...)` to re-request clean (`experimental.txt:11/18`).
- Neutralise anti-adblock: proxy `Map.prototype.has`, `Array.prototype.push`, and `Promise.prototype.then` to suppress `onAbnormalityDetected` / `onSnackbarMessage` (the "ad blockers are not allowed" nag).
- Block the ad-telemetry XHRs (`no-xhr-if` on `/api/stats/atr`, `/youtubei/v1/log_event`, `/api/stats/qoe`) — `privacy.txt:1176`, `annoyances-others.txt:3520`.

We cannot run these as uBO "scriptlets", but every one of them is a plain page-world JS operation (proxy a global, edit a request body, call a player method), so they are reimplementable as a `WKUserScript`.

## 4. Mapping to the ProsperoWeb stack

Rulesets to adopt (unchanged from what our converter already targets): uBO built-in filters, **EasyList**, **EasyPrivacy**, **Peter Lowe's list** as the declarative/host baseline — the uBOL default set. Feed them through `build_filter_set()` for the on-device userscript path (host blocks + CSS) and, where the staging tool can compile it, through `convert()`.

The decisive architectural point: **split the work by mechanism, and lean on NetworkProcess where Safari blockers cannot.**

| Target | Mechanism we use | Realistic outcome |
|---|---|---|
| General web network ads/trackers | NetworkProcess host/URL block from EasyList/EasyPrivacy/Peter Lowe (our `hosts` + converted network rules) | Strong. This is the part that works like a normal blocker and does not depend on the broken `WKContentRuleList`. |
| General web cosmetic ads (banners, slots) | `WKUserScript` injecting `css-display-none` selectors (generic + domain-scoped) from `build_filter_set()` | Good for generic and domain CSS. No procedural cosmetics (`:has`, `:matches-css`) — those are dropped and counted, same wall as uBOL/Safari. |
| YouTube **page/feed** ads (grid, sidebar, masthead, promoted) | `WKUserScript` at document-start that (i) hides `ytd-ad-slot-renderer`, `ytd-promoted-*`, `#player-ads`, `.ytp-ad-module` via CSS and (ii) json-prunes `adSlotRenderer`/`adPlacements` from `ytInitialData` / XHR responses | Achievable and fairly stable. |
| YouTube **classic pre-roll / mid-roll** (client-scheduled) | `WKUserScript` reimplementing `json-prune` of `playerResponse.adPlacements/playerAds/adSlots` (see recipe) | Achievable. This is exactly what uBOL/wBlock ship. |
| YouTube **server-stitched (SSAP)** pre/mid-roll | `WKUserScript` reimplementing the experimental logic (request-body edit + seekTo-to-skip + anti-adblock proxies), optionally a NetworkProcess hook on `/youtubei/v1/player` | Partly achievable but fragile and high-maintenance; this is an arms race uAssets itself keeps in `experimental.txt`. Expect breakage on YouTube changes. |
| YouTube — robust fallback | Native player: extract the stream and play it through our hardware decoder (the "Tube Cleaner" idea), bypassing the ad-serving web player | Most robust long-term, biggest build effort; separate track. |

### Concrete YouTube recipe (what to inject, not "use a scriptlet")

Injection contract: register the script as a `WKUserScript` with `injectionTime = .atDocumentStart`, `forMainFrameOnly = false`, and — critically — in the **page / MAIN world** (`WKContentWorld.pageWorld`), not the default isolated world. The hook must replace the page's own `JSON.parse` *before* YouTube's base.js parses the player config. This mirrors uBOL tagging its YouTube scriptlets `MAIN` (scriptlet-details.json). An isolated-world injection cannot see or proxy the page's `JSON.parse`, so it will not work.

Step 1 — prune client-scheduled ads (covers classic pre/mid-roll + feed):

```js
// document-start, MAIN world
const PRUNE = ['adPlacements','playerAds','adSlots'];              // on playerResponse + root
const prune = o => {
  if (!o || typeof o !== 'object') return o;
  for (const k of PRUNE) delete o[k];
  if (o.playerResponse) for (const k of PRUNE) delete o.playerResponse[k];
  // feed ads:
  // walk contents[].richItemRenderer.content.adSlotRenderer and drop those entries
  return o;
};
const _parse = JSON.parse;
JSON.parse = function(...a){ return prune(_parse.apply(this,a)); };
// also wrap Response.prototype.json / XHR for /youtubei/v1/player + /browse responses
```

Step 2 — strip `ytInitialPlayerResponse` / `ytInitialData` that YouTube writes inline (not via JSON.parse): intercept via a property setter (`Object.defineProperty(window,'ytInitialPlayerResponse',{set/get})`) and prune on set — equivalent to uAssets `edit-object-on-setter` (`experimental.txt:27`).

Step 3 — kill ad telemetry so YouTube does not detect the skip: in NetworkProcess (preferred) or via a `fetch`/`XHR` wrapper, drop requests matching `/api/stats/ats?`, `/api/stats/qoe?`, `/youtubei/v1/log_event`, `/pagead/`, `/ptracking` (uAssets `privacy.txt:1176`, `annoyances-others.txt:3520`). NetworkProcess is the clean place for this and is our advantage over Safari.

Step 4 — SSAP fallback (only if Step 1 leaves stitched ads): port the `experimental.txt` logic — on each player tick read `getStatsForNerds()`; if `debug_info` lacks `"SSAP, AD"` is false and the fake-buffer signature shows (`buffer_health_seconds === "0.00 s"`, `resolution === "0x0"`), call `movie_player.seekTo(getProgressState().duration)` and `loadVideoById(videoId, startSeconds)` to re-fetch clean; and inject the `instream`/`adunit` userAgent markers + `isInlinePlaybackNoAd` into the outgoing `/youtubei/v1/player` body. Guard it behind a feature flag and expect to chase YouTube changes.

Robustness to YouTube's SPA: YouTube navigates without full reloads (`yt-navigate-finish`). The JSON.parse/XHR hooks installed at document-start survive SPA navigations automatically (they are global); the DOM-cosmetic layer must re-run on `yt-navigate-finish` and under a `MutationObserver`, as the uAssets scriptlet does (`new MutationObserver(...).observe(document,{childList:true,subtree:true})`).

## 5. Bottom line

The declarative `WKContentRuleList` path is a red herring for us twice over: it is disabled in our build, and even where Safari blockers have it, it does not stop YouTube video ads — those are killed by page-world JS. Our NetworkProcess ownership makes the host/URL and telemetry-blocking half stronger than any Safari blocker, and `WKUserScript` MAIN-world injection gives us the exact same page-world hook uBOL and wBlock rely on. So the YouTube job is a userscript-engineering job, not a rule-list job.
