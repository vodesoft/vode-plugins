# PLAN: Headless UI screenshot capture in vst3testhost (L4)

Goal: test the Sectra Scope UI without launching Studio One or any external
host. Extend `vst3testhost` so that during an offline run it can open the
plugin's real editor, feed it live spectrum data, and capture PNG screenshots
of the rendered UI at chosen points in time.

## Background / research findings

- VSTGUI ships the exact primitive we need: `COffscreenContext` +
  `frame->draw(offscreen)` +
  `getPlatformFactory().createBitmapMemoryPNGRepresentation(...)` produces raw
  PNG bytes. Steinberg uses this in `VST3Editor::saveScreenshot()`
  (vstgui4/plugin-bindings/vst3editor.cpp) and in their gfxtest suite.
- The view tree is only built inside `VST3Editor::open()`, which on Windows
  requires attaching to a native HWND (`IPlugView::attach(hwnd, kWin32HWND)`).
  Solution: the testhost registers a window class and creates a hidden popup
  window positioned off-screen; the editor opens against it exactly like in a
  real host. No pixels are ever shown.
- Live data path already exists: processor → `DataExchangeHandler` → (no host
  `IDataExchangeHandler` available) → fallback `IMessage` via
  `IConnectionPoint::notify()`. If the testhost instantiates BOTH component and
  controller from the module factory and connects them as connection points
  (pattern used by the SDK hostchecker), scope snapshots flow into the
  controller's `onDataExchangeBlocksReceived` with zero new protocol work.
- Rendering must happen INSIDE the plugin image (option A below): linking
  VSTGUI into the host exe too would create two copies of VSTGUI straddling
  the DLL boundary — same version today, ODR/layout hazard tomorrow.

## Design decisions

### D1 — Screenshot trigger: message-based, plugin-side render (chosen)

New debug message ID `"vdplg.debug.screenshot"` with attributes:
- `path` (string, UTF-8) — output file path for the PNG.

Host side: build an `IMessage`, send via `controller->notify(msg)`.
Plugin side: `Controller::notify()` intercepts the ID before falling through to
`EditController::notify()`:
1. Get the current frame (the VST3Editor created by `createView("editor")`).
   If no frame is open → return `kResultFalse` (host reports failure).
2. Render: `COffscreenContext::create(frameSize, 1.)`, `beginDraw()`,
   `frame->draw(offscreen)`, `endDraw()`.
3. `createBitmapMemoryPNGRepresentation(platformBitmap)` → write bytes with
   `CFileStream` (binary/truncate/write).
4. Return `kResultTrue` on success.

Rationale: keeps all VSTGUI usage inside the plugin process; identical code
path works later if we want screenshots from a real cross-process host;
reuses Steinberg's own screenshot implementation verbatim.

Rejected alternative: link `vstgui_support` into vst3testhost and drive
`VST3Editor` directly from the host (duplicate-VSTGUI-symbol risk across the
DLL boundary).

### D2 — Editor lifecycle in the testhost

When UI capture is requested (`--ui` CLI flag or `"ui": true` in case JSON):
1. After loading the module, find the editor class UID via
   `component->getControllerClassId(...)`.
2. Create the controller instance, `initialize(nullptr)`.
3. Wire connection points both ways:
   `component->connect(controllerAsConnectionPoint)` and
   `controller->connect(componentAsConnectionPoint)` so data-exchange fallback
   messages reach the controller during `process()`.
4. `IPlugView* view = controller->createView("editor")`; query `IPlugFrame`;
   register a hidden window class + `CreateWindowEx(WS_POPUP | WS_CHILD?...)`
   off-screen (e.g. at (-20000,-20000), size 720x560); call
   `plugView->attach(hwnd, kWin32HWND)` then `plugView->open(hwnd, kWin32HWND)`.
   The editor builds its view tree, binds knobs, calls our `didOpen`.
5. Destroy order at teardown: close plugview, destroy HWND, release
   controller/component.

Note: parameter automation already applied to the component must also be
mirrored onto the controller for knob visuals — simplest correct approach:
when scheduling an automation event for block N, ALSO call
`controller->setParamNormalized(id, value)` before that block is processed
(hosts do exactly this when they forward param changes to the edit controller).

### D3 — Screenshot timing / API surface

CLI:
- `--ui` — open the hidden editor for the whole run.
- `--screenshot <path.png>` — capture once, after the audio pump finishes
  (steady-state spectra).
Case JSON fields (mirror of CLI):
- `"ui": true`
- `"screenshots": [ {"at": "1.5s"|"Nsamples", "file": "out.png"}, ... ]` —
  time-stamped captures taken between blocks during the pump (lets us capture
  mid-transition states, e.g. right after a mode switch).

Testhost verifies each captured file exists and is non-empty; failures are
reported in the JSON report (`"screenshots": [{"file":..., "ok":true/false}]`)
and flip the exit code.

## Testable features & acceptance criteria (TDD phases)

### F1 — Controller renders its frame to PNG on debug message (unit level)
Red test first, in `tests/l1_sectra_scope.cpp` (already links VSTGUI + the
Controller sources):
- Construct `Controller`, initialize, create two bare ScopeViews via
  `createCustomView` (existing helper pattern), attach them to a headless
  `CFrame` created directly with `new CFrame(rect, nullptr)` (no native
  window needed at unit level — we exercise the render function itself).
- Feed a known spectrum dataset A, send the screenshot message → assert:
  returns ok, file exists, valid PNG signature, IHDR width/height == frame
  size, pixel content differs from an all-background render.
- Feed dataset B (different values) → second PNG differs from first (byte or
  sampled-pixel comparison).
Acceptance: both screenshots written correctly; message returns false when no
frame is attached.

Implementation note: the render-to-PNG logic lives in a small free function
(e.g. `bool renderFrameToPng(CFrame*, const std::string& path)`) inside the
plugin so it is callable from both `notify()` and the unit test without a full
VST3Editor.

### F2 — Testhost opens the real editor headlessly and captures steady state
- New case `testdata/cases/sectra_ui_screenshot.json`:
  sine input, `ui: true`, one screenshot at end of run.
- Acceptance: ctest `l3_sectra_ui_screenshot` passes; PNG exists at expected
  path, dimensions 720x560, non-trivial file size (> ~5 KB, i.e. not blank);
  JSON report shows `"screenshots":[{"ok":true}]`.
- Negative check: running the same case WITHOUT the plugin supporting the
  message (not easily simulatable) — instead assert that a bogus output dir
  makes the test FAIL (exit code 1, report shows ok:false).

### F3 — Time-stamped capture during automation (mid-run states)
- Case variant: mode switch at t=1s via automation, screenshots at 0.5s
  (L/R labels) and 2s (M/S labels).
- Acceptance: both files exist and DIFFER from each other (labels/layout
  changed), proving the UI actually reacted to parameter changes while audio
  was pumping.

### F3 — DONE (green, full suite 9/9 incl. l3_sectra_ui_mode_switch)

- Case `testdata/cases/sectra_ui_mode_switch.json`: sine input (1 s @ 44.1 kHz),
  `"automation": "Channel Mode=0.5@0.5s"` (switches to kMS at sample 22050),
  screenshots at `"0.25s"` (pre-switch, L/R labels) and `"end"` (post-switch,
  M/S labels); `"mustDiffer": ["out/ui_mode_lr.png", "out/ui_mode_ms.png"]`.
- Testhost additions (`testhost/src/main.cpp`):
  - `TestCase::mustDiffer` vector parsed from case JSON; paths resolved relative
    to case dir like screenshot files.
  - `pngFilesDiffer(a, b, err)` free function: reads both files fully, returns
    true when byte vectors differ.
  - `RunResult::Diff {a, b, differ}` struct; evaluated after shot validation in
    runOffline (all pairs where a < b lexicographically).
  - JSON report emits `"mustDiffer":[{"a","b","ok"}]`; text report prints
    PASS/FAIL per pair; allOk folds diffs[].differ into exit code.
- Observation: scope curves are empty (-120 dB floor) in offline captures because
  the AnalysisWorker is async (SPSC queue + separate thread); by the time of
  mid-run capture it hasn't delivered spectra yet. The byte difference comes
  from label text ("L"/"R" → "M"/"S") and balance-mode grid change on scope B.
  This still proves the param change reached the controller's setParamNormalized
  → updateScopeLabels path. For richer future tests, a brief yield/sleep before
  capture or a synchronous worker mode would show actual spectrum curves.

### F4 — Golden-image regression compare (DONE, see Implementation notes)
- Case-file form: `"goldenUi": {"file", "ref", "maxFraction", "tolerance"}` —
  decodes both PNGs, counts per-pixel channel diffs above `tolerance`, fails
  when diff fraction exceeds `maxFraction`.
- CLI form: `--compare-golden-ui ref.png` compares the last scheduled capture
  against the baseline.
- Baselines committed under `testdata/golden/`; L3 case
  `testdata/cases/sectra_ui_golden.json` auto-registered via cases glob.
- Decoder: miniz tinfl (`third_party/miniz`, tag 3.1.2), compiled into `common`
  (only `miniz_tinfl.c`); API in `common/include/vdplg/png.h`, unit tests in
  `tests/l0_png.cpp` `[ui][l0]`.

## Files touched

| File | Change |
|---|---|
| `plugins/sectra-scope/Source/Controller.h/.cpp` | handle `"vdplg.debug.screenshot"` in `notify()`; expose frame access for tests; new helper `renderFrameToPng` |
| `tests/l1_sectra_scope.cpp` | F1 Red/Green tests |
| `testhost/src/main.cpp` | hidden window + controller wiring (`--ui`), screenshot scheduling (`--screenshot`, `screenshots[]`), report fields |
| `testhost/CMakeLists.txt` | link `user32`/`gdi32`; register new L3 cases automatically (glob already does this) |
| `testdata/cases/sectra_ui_screenshot.json` (+ F3 variant) | new cases |
| `CMakeLists.txt` | `enable_language(C)` so miniz compiles |
| `common/{png.h,png.cpp,CMakeLists.txt}` | tinfl-based PNG decode + pixel-diff golden compare |
| `tests/l0_png.cpp` | 5 `[ui][l0]` test cases (72 assertions) with in-memory encoder fixture |
| `testdata/golden/sectra_scope_steady.png` | committed steady-state baseline |
| `testdata/cases/sectra_ui_golden.json` | F4 L3 case |

No changes to the processor or analysis worker. No changes to published
plugin behavior beyond the additive debug message handler (guarded by an
exact message-ID match; inert in real hosts).

## Risks / open questions

1. **Font rendering determinism**: knob titles use SystemFont; pixel-exact
   golden compares (F4) may be machine/DPI dependent. Mitigation: F4 uses
   tolerant pixel-diff; keep goldens per-machine if needed. Does not affect
   F1–F3 (existence/dimension/difference checks only).
2. **Hidden window on headless CI**: CreateWindowEx works without a visible
   desktop session on Windows as long as there is an interactive or service
   session with a window station; fine locally, revisit if we move to CI.
3. **VSTGUI idle loop**: `open()` starts `IdleUpdateHandler`; ensure
   `close()` stops it so the testhost exits cleanly (it does — verified in
   vst3editor.cpp close()).
4. Data-exchange fallback delivers messages synchronously inside
   `process()` → controller's `onDataExchangeBlocksReceived` runs on the audio
   pump thread of the testhost. That matches our existing unit-test usage and
   is acceptable offline (no realtime constraints); screenshots themselves are
   taken from main flow between blocks.

## Implementation notes

### F1 — DONE (green, full suite 7/7)

- `renderFrameToPng(CFrame*, const std::string&)` free function in
  Controller.cpp; `Controller::notify()` intercepts `"vdplg.debug.screenshot"`
  via `std::strcmp` (**FIDString is `const char*` — never compare with `==`**).
- `attachDebugFrame()/detachDebugFrame()` added for tests that have no
  VST3Editor; ownership stays with the caller.
- Three `[ui]` TEST_CASEs in `tests/l1_sectra_scope.cpp`: render without
  attached frame (graceful no-op), render with attached frame (valid PNG
  magic + IHDR dimensions), notify round-trip through a real Controller.

### Windows headless-VSTGUI prerequisites (hard-won, all required)

1. **COM apartment BEFORE `VSTGUI::init(nullptr)`**:
  `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED /* =2 */)`. Without it
  Win32Factory's ctor leaves the WIC imaging factory null (unchecked
  CoCreateInstance) and bitmap creation crashes.
2. **A D2D device must be registered before any offscreen draw**: create a
  temporary hidden WS_POPUP window at (-20000,-20000), call
  `asWin32Factory()->createGraphicsDeviceContext(hwnd)`, release the GDC and
  `DestroyWindow(hwnd)` immediately — the device lives independently in the
  platform factory afterwards. No long-lived HWND needed.
3. **Explicit shutdown from `main()`, not static destructors**: after all
  tests run, call `extern "C" vdplg_tests_shutdown_ui()` which does
  `VSTGUI::exit(); CoUninitialize();`. Relying on cross-TU static destructor
  order crashes at exit (null-deref `mov rax,[rax]` observed in Debug;
  fault located via WER ReportArchive + dumpbin disassembly). This mirrors
  what a real host does when unloading the plugin DLL.
4. `WIN32_LEAN_AND_MEAN` excludes objbase.h → explicit `#include <objbase.h>`
  for CoInitializeEx/CoUninitialize.
5. CRect constructor is **(left, top, RIGHT, BOTTOM)**, not (x, y, w, h).
6. The test file has NO `using namespace VSTGUI` — qualify everything.

The same recipe will apply to F2 if the testhost needs offscreen rendering
(it shouldn't — screenshots are rendered inside the plugin image per D1).

### F2 — DONE (green, full suite 8/8 incl. l3_sectra_ui_screenshot)

- Case `testdata/cases/sectra_ui_screenshot.json`: sine input, `"ui": true`,
  one screenshot at `"end"` → `out/ui_steady.png`. Auto-registered by the
  existing cases glob in `testhost/CMakeLists.txt` (`sectra_*` prefix routes
  to sectrascope.vst3); reconfigure once so ctest picks it up.
- Testhost side (`testhost/src/main.cpp`): `UiCapture` struct creates a
  hidden WS_POPUP window (WS_EX_TOOLWINDOW, -20000,-20000, 720x560),
  instantiates controller from module factory, QIs IConnectionPoint both
  sides and connects them, `createView("editor")`, implements a no-op
  `HostPlugFrame : IPlugFrame` and calls `view->setFrame(...)` BEFORE
  `attached(hwnd, kPlatformTypeHWND)` (missing frame → null-deref crash in
  VSTGUIEditor::open). Screenshots fire between pump blocks via
  `ScreenshotMessage` + `ctrlCp_->notify`; automation events are mirrored to
  `controller->setParamNormalized`. Validation checks PNG signature, IHDR
  dims (big-endian) == 720x560, size >= 5 KB; failures append `[err]` to the
  reported filename and flip exit code.
- **Exit-time crash root cause**: calling `DestroyWindow` on the hidden HWND
  or `CoUninitialize()` during shutdown while the plugin DLL still holds
  live VSTGUI state → access violation at process exit (0xC0000005 /
  EXITCODE=-1073741819). Fix: skip BOTH in `UiCapture::shutdown()`; let the
  OS reclaim the window and COM apartment when the process ends. Verified
  EXITCODE=0 repeatedly.
- Other hard-won SDK facts (see repo memory for details): COM must be
  initialized before `Module::create()` (plugin static initializers call
  CoCreateInstance in DllMain); `TChar` is char16_t not wchar_t;
  `queryInterface` takes TUID by value; `IMessage::getMessageID()` is
  non-const; real `IAttributeList` has setInt/getInt/setFloat/getFloat/
  setString(AttrID,const TChar*)/getString — no count()/getKey(); VST
  interfaces live in `Steinberg::Vst`.
- Negative check verified: screenshot path into nonexistent dir → exit 1,
  report `"screenshots":[{"ok":false}]` with reason. Note: temp case files
  must sit under testdata/cases/ because input WAV paths resolve relative to
  the case file's directory.

### F4 — DONE (green, full suite 10/10 incl. l3_sectra_ui_golden)

- **Decoder** (`common/src/png.cpp`, API `common/include/vdplg/png.h`):
  miniz tinfl from `third_party/miniz` (git clone tag 3.1.2, module-split
  layout). Only `miniz_tinfl.c` is compiled into `common`; we define
  MINIZ_NO_ZLIB_APIS / NO_ARCHIVE_APIS / NO_DEFLATE_APIS before including
  miniz.h via extern "C". The repo ships no `miniz_export.h` — CMake writes an
  empty one to `${CMAKE_BINARY_DIR}/generated/`. Root CMakeLists needed
  `enable_language(C)` or the .c source was silently dropped from the target.
  Inflation uses the high-level helper `tinfl_decompress_mem_to_mem(...,
  TINFL_FLAG_PARSE_ZLIB_HEADER)`; output buffer sized by worst-case deflate
  expansion `(idat/65536+1)*65536*1033 + 64K` (a fixed multiple of compressed
  size is NOT safe for stored blocks). Supports bit depths 8/16, color types
  0/2/4/6, all five filters, non-interlaced; normalizes to 8-bit RGBA.
- **Unit tests** (`tests/l0_png.cpp`, `[ui][l0]`, 72 assertions / 5 cases):
  in-memory PNG encoder fixture (zlib stored-block framing: `78 01` header,
  BFINAL/BTYPE byte 0x01, len LE, ~len LE, data, adler32 BE); covers exact
  pixel decode RGB/gray+alpha/16-bit, rejection of missing/non-PNG files,
  tolerance semantics of countPixelDiff, and compareGoldenUi fraction math
  including the boundary (diff == maxFraction passes).
- **Testhost wiring** (`testhost/src/main.cpp`): `goldenUi` object parsed
  from case JSON (file/ref/maxFraction/tolerance), paths resolved against the
  case dir; CLI form `--compare-golden-ui ref.png` defaults file to the last
  scheduled screenshot. Evaluation runs after mustDiffer in runOffline:
  `ok = cmpOk && withinTolerance` (both flags matter — a hard decode failure
  AND exceeding the limit both fail). Text report prints per-compare lines;
  JSON report emits `"goldenUi":[...]`; goldens fold into every allOk
  computation (text branch, json branch, final exit code).
- **Baseline**: `testdata/golden/sectra_scope_steady.png` captured from the
  steady-state shot of `sectra_ui_screenshot.json` on this machine. Scope
  curves are empty offline (async AnalysisWorker), so goldens protect static
  chrome (labels, grid, knobs) — deterministic enough locally; keep goldens
  per-machine if font/DPI differences appear elsewhere (Risk #1).
- **L3 case** `testdata/cases/sectra_ui_golden.json`: sine input, peak
  assertion, one end-of-run capture compared vs baseline at maxFraction 0.01,
  tolerance 8. Negative check verified: pointing ref at a different UI state
  with maxFraction 0.001 → FAIL + exit 1 ("pixel diff fraction ... exceeds
  limit").
