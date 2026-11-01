# Spike: WebKit auxiliary processes as threads of the native title

Question: can the native PS5 title run the WebProcess and the NetworkProcess as **threads inside the UI process** (launcher starts `AuxiliaryProcessMain` on a new thread, IPC over a `socketpair` in the same address space), so the title needs no second process, no broker and no elfldr?

Short answer: technically possible, but only by teaching WTF that one address space holds several "processes" (three logical main threads, three main run loops, three process types and identifiers) and by serialising them with a global lock, the way iOS WebKitLegacy shares the main thread with its WebThread. WebKit has no such mode today; the one it once had (WebKit2's `ThreadLauncher`, 2010-2012) was removed as broken. The cost is a large permanent fork of WTF/WebKit, no crash isolation, no JIT for web content, and UI jank while content runs. **Recommendation: keep the broker design; do not switch.** In-process mode is worth a prototype only if elfldr-less launch becomes a hard requirement.

Scope: analysis of `third_party/webkit` at `e049cc7f923ea377af4d92877f9eedaf4ed5a1c0` with this repository's patches applied, as `scripts/ps5/fetch-webkit.sh` leaves it, so line numbers in `Connection.cpp` and `ProcessLauncherPlayStation.cpp` include patches 0005/0018. Nothing was built, and nothing was run on a console. The grep counts come from the script in the appendix. Paths are relative to `third_party/webkit/Source/`.

## 1. Main-thread identity

### How WTF defines "main"

| What | Where | Behaviour |
|---|---|---|
| `initializeMainThread()` | `WTF/wtf/MainThread.cpp:40-48` | `std::call_once` for the whole process: `initialize()`, `initializeMainThreadPlatform()`, `RunLoop::initializeMain()`, then `RELEASE_ASSERT(!isMainThread() \|\| Thread::currentSingleton().uid() == 1)` |
| `isMainThread()` (generic, used by PlayStation) | `WTF/wtf/generic/MainThreadGeneric.cpp:40-55` | one `static pthread_t s_mainThread`, set to the thread that called `initializeMainThread()`; `isMainThread()` is `pthread_equal(pthread_self(), s_mainThread)` |
| main run loop | `WTF/wtf/RunLoop.cpp:38, 63-87` | one `static RunLoop* s_mainRunLoop`; `initializeMain()` does `RELEASE_ASSERT(!s_mainRunLoop)`; `mainSingleton()` returns it |
| `RunLoop::isMain()` / `isMainRunLoop()` | `WTF/wtf/RunLoop.h:107`, `MainThread.cpp:63-66` | `mainSingleton().isCurrent()` |
| `callOnMainThread` / `callOnMainRunLoop` / `ensureOnMain*` | `WTF/wtf/MainThread.cpp:68-99` | all post to `RunLoop::mainSingleton()` (or to the WebThread run loop under `USE(WEB_THREAD)`) |
| process type | `WTF/wtf/RuntimeApplicationChecks.cpp:71-98` | one `static std::optional<AuxiliaryProcessType>`; `isInWebProcess()`/`isInNetworkProcess()` read it (56 call sites in WebCore/WebKit/WTF/JSC) |

So "the main thread" is one process-wide value set once, and every subsystem (UI, WebProcess, NetworkProcess) would see the **UI's** thread as main. If the auxiliary processes ran on new threads unchanged:
- `InitializeWebKit2()` (`WebKit/Shared/WebKit2Initialize.cpp:42-55`) on the WebProcess thread is a no-op for the main thread (the `call_once` already ran in the UI), so every `isMainThread()` check on the WebProcess thread is false;
- `RunLoop::run()` at the end of `AuxiliaryProcessMainBase::run()` (`WebKit/Shared/AuxiliaryProcessMain.h:63-88`) runs the thread's own run loop, but every `callOnMainThread`/`callOnMainRunLoop` from WebProcess or NetworkProcess code lands on the **UI** thread, where it runs WebCore code on the wrong thread;
- `setAuxiliaryProcessType()` (`AuxiliaryProcessMain.h:68-69`, `AuxiliaryProcess.cpp:89`) overwrites the UI's "I am the UI process" state, and the last of the WebProcess or NetworkProcess threads to start wins.

### Size of the dependency (grep counts)

`releaseAssert` = `RELEASE_ASSERT[_WITH_MESSAGE](isMainThread()|RunLoop::isMain()|isMainRunLoop())`; `debugAssert` = `ASSERT(...)` of the same or `assertIsMain{Thread,RunLoop}()` (compiled out in our release build); `allChecks` = every call of the three predicates (asserts plus branches); `dispatch` = `callOnMainThread|callOnMainRunLoop|ensureOnMainThread|ensureOnMainRunLoop|mainSingleton().dispatch`.

| Directory | releaseAssert | debugAssert | allChecks | dispatch | `RunLoop::mainSingleton()` |
|---|---:|---:|---:|---:|---:|
| `WTF/wtf` | 1 | 27 | 55 | 47 | 21 |
| `JavaScriptCore` | 0 | 7 | 11 | 1 | 1 |
| `WebCore` | 56 | 1221 | 1487 | 701 | 208 |
| `WebKit/WebProcess` | 6 | 187 | 205 | 132 | 75 |
| `WebKit/NetworkProcess` | 16 | 225 | 448 | 119 | 84 |
| `WebKit/Shared` | 6 | 15 | 26 | 40 | 19 |
| `WebKit/Platform` (IPC) | 0 | 20 | 25 | 17 | 16 |
| `WebKit/UIProcess` | 34 | 326 | 446 | 350 | 255 |

Raw per-pattern counts (from `build-spike/count.sh`): `isMainThread()` in WebCore 1428, `callOnMainThread` in WebCore 409, `ensureOnMainThread` in WebCore 148, `RunLoop::isMain()` in NetworkProcess 415. More objects that are tied to "the" main thread: `ThreadSafeRefCounted<..., DestructionThread::Main|MainRunLoop>` 150 uses, `MainThreadNeverDestroyed`/`MainRunLoopNeverDestroyed` 340 in WebCore + 26 in WebKit, `static (Lazy)NeverDestroyed` 1132 in WebCore + WebProcess + NetworkProcess.

The release asserts (about 85 in the code that would run on auxiliary threads) are the least of it. The **~1000 dispatch sites** decide which thread runs the work, and with one global main they all go to the UI thread. Patching call sites one by one is out of the question. The only workable approach is to change what "main" means inside WTF.

### Existing non-main "web thread" mechanism

- `USE(WEB_THREAD)` (iOS WebKitLegacy, 48 `#if` sites): `isMainThread()` is `(isWebThread() || pthread_main_np()) && webThreadIsUninitializedOrLockedOrDisabled()` (`WTF/wtf/cocoa/MainThreadCocoa.mm:101-104`). Two threads *share* the main identity, and **mutual exclusion comes from the WebThread lock** (`WebCore/platform/ios/wak/WebCoreThread.mm:787-810`). The WebThread's run loop takes and drops that lock from run-loop observers (`WebRunLoopLock`, `:521-545`). Both threads also share one `AtomStringTable` (`WTF/wtf/Threading.cpp:217-224`) and one `ThreadGlobalData` (`WebCore/platform/ThreadGlobalData.cpp:64-95`). `callOnMainThread` goes to the web run loop when there is one (`MainThread.cpp:81-91`).
- `isWebThread()`/`isUIThread()` fall back to `isMainThread()` without `WEB_THREAD` (`MainThread.h:70-73`). `isMainThreadOrGCThread()` (`MainThread.cpp:101-107`) only adds GC helper threads.

This is the only in-tree precedent for "two threads are main", and it works only because the two threads never run WebKit code at the same time.

### Can it be generalised? Design options

| Option | Idea | Verdict |
|---|---|---|
| A. One thread for everything | Run WebProcess/NetworkProcess message handlers on the UI's main run loop | **Not viable.** Synchronous IPC deadlocks: 94 `Synchronous` messages to UIProcess receivers, 14 to WebProcess, 21 to NetworkProcess (`*.messages.in`), e.g. `CookiesForDOM` (`NetworkProcess/NetworkConnectionToWebProcess.messages.in:44`); the UI waits for the WebProcess in `DrawingAreaProxyCoordinatedGraphics.cpp:131` (`waitForAndDispatchImmediately`, 500 ms). A sender blocked in `waitForSyncReply` (`Platform/IPC/Connection.cpp:190-192`) would wait for itself. |
| B. Per-thread "process domain", fully concurrent | A thread-local `Domain*` (main `pthread_t`, main `RunLoop*`, `AuxiliaryProcessType`, `ProcessIdentifier`). `isMainThread()`, `RunLoop::mainSingleton()`, `processType()`, `Process::identifier()` read the current thread's domain. `Thread::create` and `WorkQueue`/`ConcurrentWorkQueue` dispatch capture the creator's domain. | Routes the 1000+ dispatch sites correctly, but **process-global WebCore state is then touched concurrently by up to three main threads with no lock**: `StringImpl` refcounts are not atomic (static `String`s/`AtomString`s copied by WebCore code in both WebProcess and NetworkProcess, e.g. `MIMETypeRegistry` tables); `ObjectIdentifierMainThreadAccessTraits` uses a plain `++currentMainThreadIdentifier` (`WTF/wtf/ObjectIdentifier.cpp:49-53`, 392 identifier types); 1132 `static NeverDestroyed` objects. These are rare, silent data races that would never be found by testing. |
| C. B plus a global "WebKit lock" (GIL) | Like iOS: at most one logical main thread runs WebKit code at a time. The lock is taken when a main run loop dispatches work and released where it would block: the run-loop idle wait (`WTF/wtf/generic/RunLoopGeneric.cpp:136, 148`), the IPC sync/`waitForMessage` waits (`Platform/IPC/Connection.cpp:190-192, 880`), and the shell's own frame/SDL wait. Logical mains share one `AtomStringTable` (as `Threading.cpp:219-224` does for iOS). Background threads (GC, curl scheduler, IPC queues, workers) never take it and keep their own tables, as they do today. | **The only option that is plausibly correct.** Costs: content and UI no longer run in parallel. A long script or layout in the "WebProcess" freezes UI painting and input, as WebKit1 did. One more WTF fork point. |

Why the shared `AtomStringTable` matters even for B: `initializeCommonAtomStrings()` runs once per process (`std::call_once`, `WebCore/platform/CommonAtomStrings.cpp:38-47`) from `InitializeWebKit2()`, i.e. on the UI thread. The common atoms (`starAtom`, `xmlnsAtom`, ...) would therefore live in the UI thread's per-thread table (`Threading.cpp:217`), while the WebProcess thread's own `AtomString("*")` would be a different `StringImpl`. AtomString equality is pointer equality, so comparisons fail silently. The `HTMLNames::init()` family has the same once-per-process guard (`WebCore/dom/make_names.pl:698-702`).

`RunLoop::mainSingleton()` is one object, so yes: each domain needs its own main `RunLoop` (just `RunLoop::currentSingleton()` of the domain's thread, which the generic RunLoop already supports), and `mainSingleton()`/`isMain()` must become domain lookups. `callOnMainThread` then routes correctly through the domain captured by the calling thread. The hole is in shared thread pools that run tasks from several domains. For those the domain must travel with each dispatched task (wrap the function in `WorkQueueBase::dispatch*`, `ConcurrentWorkQueue`, `Thread::create`), not with the thread.

## 2. Singletons and process-global state

| State | Where | Thread-local? | Conflict in one address space |
|---|---|---|---|
| `WebProcess::singleton()` | `WebKit/WebProcess/WebProcess.cpp:337-341` (`static NeverDestroyed`) | no | No clash with the UI (it has `WebProcessPool`, not `WebProcess`), but **only one WebProcess per address space, and it can never be restarted** (static, `call_once` initialisers). Force `usesSingleWebProcess` (`UIProcess/API/APIProcessPoolConfiguration.h:55-56`), turn off prewarming (`:58-64`), process swap on navigation (`:122-127`) and the WebProcess cache. The chrome page and the content then share one WebProcess (today the title spawns two). |
| `NetworkProcess::singleton()` | `WebKit/NetworkProcess/NetworkProcess.cpp:178-182` | no | same: one instance, never restartable |
| `AuxiliaryProcessType` | `WTF/wtf/RuntimeApplicationChecks.cpp:71-98` | no | overwritten by each auxiliary start (`AuxiliaryProcess.cpp:89`); must become per domain |
| `WebCore::Process::identifier()` | `WebCore/platform/ProcessIdentifier.cpp:34-57` (`static std::optional<ProcessIdentifier> globalIdentifier`) | no | `AuxiliaryProcess::initialize` calls `Process::setIdentifier` (`AuxiliaryProcess.cpp:94`); the UI's own identifier would change under it, which corrupts `ProcessQualified<>` identifiers (48 uses). Must become per domain. |
| `JSC::Options` / `g_jscConfig` | `JavaScriptCore/runtime/InitializeThreading.cpp:100-115`, `Options.cpp:1158-1186` | no | **The UI does initialise JSC on PlayStation**: `InitializeWebKit2()` calls `JSC::initialize()` (`WebKit2Initialize.cpp:44`), and the UI creates VMs lazily (`SharedJSContextWK`, `Shared/API/APISerializedScriptValue.cpp:40-64`). Options are read from the environment once per process, so the UI's `JSC_useJIT=false` (`PROSPEROWEB_UI_ONLY_ENV`) applies to web content too. The first `VM` construction calls `Config::finalize()` → `permanentlyFreeze()` (`JavaScriptCore/runtime/VM.cpp:568`, `WTF/wtf/WTFConfig.cpp:190-211`), which makes the config page read-only. After that the WebProcess's late option writes (`WebProcess.cpp:453-455`, `:741-748`, guarded only by `!commonVMOrNull()`) write to a read-only page and crash, unless the UI never creates a VM first. |
| `MemoryPressureHandler::singleton()` | used by UI (5), WebProcess (7), NetworkProcess (3), WebCore (28) | no | one handler per address space: each process's `setLowMemoryHandler`/configuration overwrites the previous one (UI: `UIProcess/WebMemoryPressureHandler.cpp:40`) |
| `CurlContext::singleton()` | `WebCore/platform/network/curl/CurlContext.h:64` (`curl_global_init`) | no | NetworkProcess only (6 uses) plus WebCore curl code (22); the shell does not call libcurl itself (no `curl_*` in `src/`). No clash, but `curl_global_init` is not thread-safe if anything else initialises curl. |
| `NetworkStorageSession` / cookie permission | `WebCore/platform/network/CookieStorageSession.cpp:37-47` (static bool) | no | UI sets it (`UIProcess/WebProcessPool.cpp:311`), NetworkProcess sets it (`NetworkProcess.cpp:331`); harmless (same value), but WebProcess code that asserts the opposite would now see `true` |
| `FontCache::forCurrentThread()` | `WebCore/platform/graphics/FontCache.cpp:126-134` → `ThreadGlobalData` | **yes** (per WTF `Thread`, `ThreadGlobalData.cpp:97-110`) | none |
| `eventNames()`, `ThreadTimers` | `ThreadGlobalData` | yes | none |
| `AtomStringTable` | `WTF/wtf/Threading.cpp:217` | yes | see §1: the common atoms and HTML names are global objects created in one table |
| `commonVM()` | `WebCore/bindings/js/CommonVM.cpp` (global) | no | WebProcess only; UI uses `SharedJSContextWK`, its own VM, so no clash, except the config freeze above |
| `GCController` | WebCore singleton | no | WebProcess only |
| `PlatformDisplay::sharedDisplay()` | WebCore (82), WebProcess (35) | no | WebProcess only on our port (no GL) |
| `DeprecatedGlobalSettings`, `LegacySchemeRegistry`, `SecurityPolicy` lists | WebCore statics (195 uses of `DeprecatedGlobalSettings`) | no | written by both WebProcess and NetworkProcess initialisation, same values in practice; data race without the lock |
| `ObjectIdentifier` generator | `WTF/wtf/ObjectIdentifier.cpp:49-53` | no | one counter for all. IDs stay unique (better than multi-process), but the counter is non-atomic → needs the lock |
| ICU | process-global, initialised once, thread-safe | n/a | none |
| `setlocale` | only GTK `WebProcessMainGtk.cpp:79` | n/a | none on PlayStation |
| Signals | WTF thread suspend/resume `SIGUSR1` (`WTF/wtf/posix/ThreadingPOSIX.cpp:198`), `SIGPIPE` ignored (`Shared/unix/AuxiliaryProcessMain.cpp:103-106`) | process-wide | same handlers for everyone; GC thread suspension targets specific threads, so fine |
| `atexit` | NetworkProcess 1, WebCore 2 | process-wide | harmless |
| **Process exit paths** | `AuxiliaryProcess::didClose` → `terminateProcess(EXIT_SUCCESS)` on non-GTK/WPE (`Shared/AuxiliaryProcess.cpp:69-77`); WebProcess `callExit` on connection close (`WebProcess.cpp:319-321, 467-478`); `setShouldExitOnSyncMessageSendFailure(true)` → `exitProcess(0)` (`WebProcess.cpp:480-482`, `Platform/IPC/Connection.cpp:1505-1510`); NetworkProcess `callExitSoon` (`NetworkProcess.cpp:392-394`), `terminateProcess(EXIT_FAILURE)` (`:172`); `AuxiliaryProcess::platformStopRunLoop` stops `RunLoop::mainSingleton()` (`AuxiliaryProcess.cpp:229-233`) | process-wide | each of these would **kill or stop the whole title** (the UI included) when a page closes or the UI terminates an unresponsive WebProcess. All must become "stop this domain's run loop and end the thread". |

`ps5_terminate_process()` cannot kill a thread. `AuxiliaryProcessProxy` wants a `ProcessID` (logs, `terminate`), so the launcher has to invent a positive token. "Terminate" can at best ask the domain's run loop to stop. A hung WebProcess thread cannot be reclaimed.

## 3. Precedent

| Where | What | Status |
|---|---|---|
| WebKit2 `UIProcess/Launcher/ThreadLauncher.{h,cpp}` + `ThreadLauncherMac.mm`, `ThreadLauncherQt.cpp`, `ThreadLauncherWin.cpp` (2010-2012) | ran `WebProcess` on a thread of the UI process ("WebKit2: WebThread"), process model `ProcessModelSharedSecondaryThread`, used by MiniBrowser's threaded mode | **removed**: [bug 93652](https://bugs.webkit.org/show_bug.cgi?id=93652) "Get rid of ProcessModelSharedSecondaryThread", r125363 (2012-08-11), because the code had "been broken for too long"; MiniBrowser's threaded mode went first (bug 93774). Sources: [ThreadLauncher.h](https://webkit.googlesource.com/WebKit/+/a259a13f74fd8e66f1f243b22aed5c3b5ae96e5d/WebKit2/UIProcess/Launcher/ThreadLauncher.h); GTK never implemented it ([webkit-dev, May 2011](https://lists.webkit.org/pipermail/webkit-dev/2011-May/016878.html)). Note: in 2010 there was no NetworkProcess and far less main-thread machinery. |
| `WKContextSetProcessModel` / `kWKProcessModel*` | `UIProcess/API/C/WKContext.h:142-147` ("FIXME: Remove these once support for Mavericks has been dropped"); the function is an empty stub (`Shared/API/c/WKDeprecatedFunctions.cpp:47-49`) | dead API |
| `usesSingleWebProcess` | `APIProcessPoolConfiguration.h:55-56` | one WebProcess for all pages, **still a separate process** |
| WebKitLegacy (WebKit1) | genuinely in-process, `USE(WEB_THREAD)` on iOS | `Source/WebKitLegacy` builds only for Cocoa (`PlatformCocoa.cmake`, no PlayStation/Win sources left); porting it means writing all WebCore clients again |
| `ProcessLauncher` thread variants, `launchProcessAsThread`, `UseThreads`, `singleProcess` in `WebKit`, `WebKitTestRunner`, `TestWebKitAPI` | — | **nothing** at this commit (only an unrelated `ServerRunInThread` for the GLib test HTTP server) |

Chromium's `--single-process` runs the renderer (and the GPU service) on threads of the browser process (`RenderProcessHost::run_renderer_in_process()` with an in-process renderer thread). Chromium treats it as an unsupported debugging mode, and it works there because Blink's per-thread state was designed for it, which WebCore's "one main thread per process" model is not.

## 4. Partial variants

| Variant | Saves the broker? | Notes |
|---|---|---|
| NetworkProcess as a thread, WebProcess still a process | **No**: the WebProcess still has to be spawned, which a foreground title cannot do itself (`docs/native-title.md`: `rfork`+`ptrace`+`execve` from the big app panics the console). | Saves one process and one image mapping. **Makes things worse on the console**: libcurl would then run inside the title, which refuses `fcntl` on sockets and may not make TCP sockets non-blocking at all (runs 22 and 34, commits `79d7985` and `ef9dfe1`). curl's multi interface needs non-blocking connects. Today the NetworkProcess runs as a payload, where this works. |
| WebProcess as a thread, NetworkProcess still a process | **No**: the NetworkProcess still needs spawning. | Gets most of the complexity of §1 (WebCore is the bulk of the main-thread code) for none of the benefit. |

Both have to become threads to reach the operator's goal (no second process, no broker, no elfldr). Doing only one of them gains nothing.

## 5. JIT, sandbox and resource consequences

- **JIT.** A title has no executable memory: `docs/native-title.md` ("the UI runs JSC without JIT (no executable memory for a title), while the children keep their JIT (`PROSPEROWEB_UI_ONLY_ENV`)"). `sceKernelJitCreateSharedMemory`/`JitMapSharedMemory` exist, but third-party titles report they "need the process jailbroken first" (`docs/research.md:222`), and the title deliberately never raises its own privileges and has no kernel R/W (`docs/native-title.md:24, 67`). `JSC::Options` are one per process (§2), so web content in the title runs on the LLInt/IPInt interpreters. Measured cost on the console: `chrome/bench.html` 645 ms with JIT+Wasm vs 1107 ms without, **about 1.7x slower** (`docs/ps5-runtime.md:830-834`). The JSC config-freeze ordering (§2) has to be solved either way.
- **Memory.** With `USE_SYSTEM_MALLOC=ON` (`build-wk-ps5/CMakeCache.txt`) every `fastMalloc` goes to the title's dlmalloc heap in direct memory (≤ 4 GiB, `docs/native-title.md:68`). That heap is generous, and one image mapping instead of three saves roughly 2 × 130 MiB. But JSC heap blocks and other anonymous `mmap`s draw on the title's **512 MiB of flexible memory** before falling back to direct memory (`docs/native-title.md:69, 71`). UI + content + network then share that one budget, and nobody has measured the fallback under JSC's allocation pattern.
- **Crash isolation: none.** Any `RELEASE_ASSERT`/`CRASH()` in web content or network code (5811 sites in WebCore, WebProcess, NetworkProcess and JSC), any renderer bug in a bad page, and the ffmpeg media engine all take down the whole title. The UI's "WebProcess crashed, reload" path cannot work, because the WebProcess singleton cannot be re-created (§2), so recovery means restarting the title. Patch 0014 shows that a crashed WebProcess is a real event on this console.
- **Responsiveness.** With option C, content and UI never run WebKit code in parallel. Heavy pages stall the address bar, scrolling and painting.
- **Sandbox gains.** No broker, no elfldr, no `rdup` of descriptors, no cross-process `SCM_RIGHTS`, no payload/title path translation (`/app0` vs install folder). `socketpair` + `SCM_RIGHTS` inside one process works (self-test, `docs/native-title.md:156`), and `fcntl` is already emulated with `SO_NBIO`. But all remaining title restrictions now also hit WebCore and curl: `access()` EPERM, file permission quirks, fontconfig cache writes, and non-blocking TCP for curl (the open question in §4).

## 6. Estimate, patch sites, risk, recommendation

### Effort (one engineer who knows this tree; console iteration included)

| Deliverable | Engineer-days | Contents |
|---|---:|---|
| (a) hacky prototype: one page renders in-process on the PS5 | **8-15** | WTF domains (main thread, main run loop, process type, process identifier, domain propagation through `Thread::create`/`WorkQueue`) 2-3; GIL in `RunLoopGeneric` + IPC waits + shell loop 1-2; thread launcher + entry points 0.5; exit paths 0.5; JSC freeze order + shared atom table 0.5-1; single-WebProcess configuration, chrome sharing the content process 0.5; curl in the title (non-blocking TCP) 1-5, **unknown, may block**; console debugging 3-5 |
| (b) production-quality mode | **35-60**, plus permanent rebase cost | everything in (a), plus: audit of `MainThreadNeverDestroyed` (366), `DestructionThread::Main*` (150) and shared statics (1132) under the lock; WebProcess/NetworkProcess teardown and relaunch without restarting the title (re-initialisable singletons: not supported anywhere in WebKit); memory-pressure handler multiplexing; hang watchdog; regression testing against the broker build; keeping a WTF/WebCore fork in sync on every WebKit update |

### Top 10 patch sites

1. `WTF/wtf/generic/MainThreadGeneric.cpp:40-55`: `s_mainThread` → per-domain main thread; `isMainThread()` per domain (and "holds the GIL" as in `MainThreadCocoa.mm:101-104`).
2. `WTF/wtf/RunLoop.cpp:38, 63-87` and `RunLoop.h:107`: `s_mainRunLoop`, `initializeMain()` `RELEASE_ASSERT`, `mainSingleton()`/`isMain()` → per domain.
3. `WTF/wtf/MainThread.cpp:40-48, 68-99`: `initializeMainThread()` once per domain (and the `uid() == 1` `RELEASE_ASSERT`), `callOnMainThread`/`callOnMainRunLoop` routing.
4. `WTF/wtf/Threading.cpp:217-224` plus `WorkQueue`/`ConcurrentWorkQueue` dispatch: domain inheritance for new threads and pooled tasks; one shared `AtomStringTable` for the logical main threads.
5. `WTF/wtf/RuntimeApplicationChecks.cpp:71-98` and `WebCore/platform/ProcessIdentifier.cpp:34-57`: process type and process identifier per domain.
6. `WTF/wtf/generic/RunLoopGeneric.cpp:136, 148`: release/re-acquire the GIL around the idle wait (iOS `WebRunLoopLock`, `WebCoreThread.mm:521-545`).
7. `WebKit/Platform/IPC/Connection.cpp:190-192` (sync-reply semaphore) and `:880` (`waitForMessage`): release the GIL while blocked.
8. Exit paths: `WebKit/Shared/AuxiliaryProcess.cpp:69-77, 229-233`, `WebKit/WebProcess/WebProcess.cpp:319-321, 467-482`, `WebKit/NetworkProcess/NetworkProcess.cpp:172, 392-394`, `WebKit/Platform/IPC/Connection.cpp:1505-1510` → stop the domain's run loop and end the thread.
9. `WebKit/UIProcess/Launcher/playstation/ProcessLauncherPlayStation.cpp:124-150` (patch 0005), `terminateProcess` `:184-196`, and `WebKit/Shared/AuxiliaryProcessMain.h:63-88` / `Shared/unix/AuxiliaryProcessMain.cpp:63-99`: start `ps5_webkit_*_process_main` on a `Thread` with a large stack (title default 2 MiB), hand over the client fd, return a fake PID; `InitializeWebKit2()` per domain.
10. JSC/WebCore once-only initialisation: `WebKit/WebProcess/WebProcess.cpp:453-455, 741-748` vs `JavaScriptCore/runtime/VM.cpp:568` / `WTF/wtf/WTFConfig.cpp:190-211` (option writes after the freeze), and `WebCore/platform/CommonAtomStrings.cpp:38-47` (`RELEASE_ASSERT(isUIThread())`, atoms created in the UI's table).

Shell side (outside this spike's ownership, `src/frontend/ps5`): release the GIL around the SDL/present wait, dispatch `WebProcess`/`NetworkProcess` to threads instead of `ps5_spawn_self`, and give up the second WebProcess for the chrome page.

### Risks

| Risk | Likelihood | Impact |
|---|---|---|
| Silent data races in shared WebCore statics (if the GIL is skipped or leaky) | high | memory corruption that cannot be reproduced |
| curl cannot get non-blocking TCP sockets inside the title | medium (runs 34/35 still open) | NetworkProcess-in-title impossible → whole approach blocked |
| One content crash takes down the title; no WebProcess relaunch | certain | worse UX than today's broker design, which survives a WebProcess crash |
| Interpreter-only JS (~1.7x slower), UI stalls during content work | certain | noticeably slower browsing |
| Permanent fork of WTF threading core, re-checked on every WebKit bump | certain | ongoing cost; upstream deleted the last such mode as unmaintainable |
| Flexible-memory exhaustion (512 MiB shared by everyone's anonymous `mmap`) | medium | OOM; fallback to direct memory untested under JSC |

### Recommendation

**Continue the broker design.** It already works in the native title after the sandbox fixes, keeps the JIT and crash isolation in the children, and the outstanding problems (elfldr dependency, loopback socket modes) are small and local. In-process mode trades one external dependency (elfldr, or websrv as fallback) for a deep fork of WebKit's threading model, slower JS, and a title that dies with any page.

Keep in-process mode only as a **documented fallback idea**. Spend the prototype budget, (a) 8-15 days, only if elfldr-less launch becomes a hard requirement (e.g. distribution to users without a loader). Before that, run one cheap gating test in the title: a libcurl multi transfer over TCP (non-blocking connect) inside the native title. If that fails, NetworkProcess-in-title, and with it the whole in-process idea, is dead without any WebKit work.

## Appendix: host experiment (counting only)

`build-spike/count.sh` (not committed; `build-*/` is ignored) produced the counts above. It is a plain `grep -rEo ... | wc -l` per directory over `*.cpp`, `*.h`, `*.mm`, `*.c`:

```sh
#!/usr/bin/env bash
set -euo pipefail
S=${1:-third_party/webkit/Source}
declare -A DIRS=([WTF]="$S/WTF/wtf" [JSC]="$S/JavaScriptCore" [WebCore]="$S/WebCore"
  [WP]="$S/WebKit/WebProcess" [NP]="$S/WebKit/NetworkProcess" [Shared]="$S/WebKit/Shared"
  [Platform]="$S/WebKit/Platform" [UI]="$S/WebKit/UIProcess")
PATS=('RELEASE_ASSERT\(isMainThread\(\)\)' '[^_]ASSERT\(isMainThread\(\)\)' 'assertIsMainThread\(\)'
  'isMainThread\(\)' 'isMainRunLoop\(\)' 'assertIsMainRunLoop\(\)' 'RunLoop::isMain\(\)'
  'RunLoop::mainSingleton\(\)' 'callOnMainThread' 'callOnMainRunLoop' 'ensureOnMainThread'
  'ensureOnMainRunLoop' '::singleton\(\)' 'NeverDestroyed<')
for p in "${PATS[@]}"; do printf '%-36s' "$p"
  for d in WTF JSC WebCore WP NP Shared Platform UI; do
    printf '%8s' "$( (grep -rEo --include='*.cpp' --include='*.h' --include='*.mm' --include='*.c' "$p" "${DIRS[$d]}" || true) | wc -l)"
  done; echo; done
```

Output at the pinned commit (columns WTF, JSC, WebCore, WebProcess, NetworkProcess, Shared, Platform, UIProcess):

```text
RELEASE_ASSERT(isMainThread())      1    0    54    0    0    0    0    0
ASSERT(isMainThread())             12    7   857    0    0    0    0    0
assertIsMainThread()                8    0   341    2    7    0    0    0
isMainThread()                     42   11  1428    1    0    0    0    0
isMainRunLoop()                     5    0     5  146   33    9    3  171
assertIsMainRunLoop()               1    0     0   48    0    1    0    1
RunLoop::isMain()                   8    0    54   58  415   17   22  275
RunLoop::mainSingleton()           21    1   208   75   84   19   16  255
callOnMainThread                   16    0   409    0    0    0    0    1
callOnMainRunLoop                   7    0    31   74   27    7    3   81
ensureOnMainThread                  9    0   148    8    4    1    0    3
ensureOnMainRunLoop                 7    0    10    9   10    0    0   45
::singleton()                      27  219  1384 1366  162   23    8  364
NeverDestroyed<                    67  168  1471   64   32   46   25  209
```

(`isMainRunLoop()` includes `assertIsMainRunLoop()`; `isMainThread()` includes the assert forms.)
