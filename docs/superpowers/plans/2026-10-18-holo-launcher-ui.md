# Holo-era launcher redesign — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the developer-style launcher with a Holo-era player app: a library of installed and added 32-bit
games with their own icons, tap to convert then tap to play, and a long-press menu (art header + Settings, Mods,
Patches, Delete).

**Architecture:** Pure-Java parts (`ApkScan`, `ArtPicker`, `GameSettings`) are host-tested on the PC; Android parts
(`GameCatalog`, `ArtCache`, `ConvertJob`, `HoloTheme`, the screens, `SettingsProvider`) are plain framework views
built in code, as today. `LauncherActivity` becomes a router. Converted games read their per-game settings at start
through the repack's existing shim provider, which asks the launcher's provider; the launcher answers only callers
signed with this device's repack key.

**Tech Stack:** Java 8 language level, Android framework only (no AndroidX), aapt2/d8 via `tools/build_launcher_apk.sh`,
host tests via `tools/run_java_tests.sh`, C++ runtime in `android/jni/runtime_boot.cpp`.

**Spec:** `docs/superpowers/specs/2026-10-18-holo-launcher-ui-design.md`

## Global Constraints

- No new libraries; framework APIs only; minSdk 26, the launcher's targetSdk 34.
- Every scripted launcher extra keeps working unchanged (`-e repack`, `-e stage`, `-e target`, `-e rename`,
  `-e install`, `-e native`, `-e debuggable`, `-e bootcheck`, `-e stubcheck`, `-e run`, `-e shimcheck`): the device
  scripts in `tools/` and `$TEMP` depend on them.
- The UI thread never opens an APK, reads a zip entry or decodes an image.
- The original installed game is never modified or uninstalled; only `<pkg>.rb` copies are.
- Player-flow conversions use rename `.rb` and target 35 (Play Protect installs it on both devices), unless the
  game's Settings tab says otherwise.
- Copy follows the spec's wording; sentence case; no "please", no "successfully".
- Colours: Holo dark background `#101010`, panels `#1B1B1B`, divider `#2A2A2A`, text `#FFFFFF` / `#B3B3B3`, accent
  ramp cyan `#33B5E5` (default), green `#99CC00`, orange `#FFBB33`, purple `#AA66CC` — the Holo palette.

## Review Focus

1. A game installed while the launcher is open (or a copy uninstalled) must show its new state without a restart.
2. An APK the launcher cannot read (corrupt, a split-only install) must yield a plain tile, not a crash or a hang.
3. A second tap while a conversion is running must not start a second conversion.
4. A converted game whose launcher provider is missing or refuses must start with defaults.
5. Very large icon or art images (4096² PNGs) must not run the launcher out of memory.

---

### Task 1: `ApkScan` — ABIs and image sizes from an APK

**Files:** Create `android/java/com/retrobug/launcher/ApkScan.java`; test `tools/java-tests/ApkScanTest.java`;
modify `tools/run_java_tests.sh` (run it).

**Interfaces — Produces:**
- `final class ApkScan { static Info scan(File apk) throws IOException; static int[] imageSize(byte[] head) }`
- `static final class Info { final Set<String> abis; final List<Image> images; boolean is32BitOnly() }` —
  `is32BitOnly()` is true when `abis` contains `armeabi` or `armeabi-v7a` and not `arm64-v8a`.
- `static final class Image { final String path; final int width, height; }`
- `imageSize` returns `{w, h}` for PNG (IHDR), JPEG (first SOF0–SOF15 marker except C4/C8/CC), WebP (VP8, VP8L,
  VP8X), or `null`. `scan` reads at most the first 128 KiB of each `.png/.jpg/.jpeg/.webp` entry, skips `*.9.png`.

- [ ] **Step 1: Write the failing test.** `ApkScanTest` builds zips in the scratch dir: (a) `lib/armeabi-v7a/libx.so`
  only → `is32BitOnly()`; (b) `armeabi-v7a` + `arm64-v8a` → false; (c) no `lib/` → false; (d) a PNG 1280×720, a JPEG
  800×480 with an EXIF APP1 segment before SOF0, a WebP VP8X 1920×1080, a `foo.9.png` → three images with those sizes,
  the 9-patch absent; (e) a truncated PNG → absent, no exception. Exact bytes are written by helpers in the test.
- [ ] **Step 2: Run** `bash tools/run_java_tests.sh` — Expected: compile FAIL (no `ApkScan`).
- [ ] **Step 3: Implement `ApkScan`** as specified (ZipFile; header parsing by hand).
- [ ] **Step 4: Run** — Expected: `ApkScanTest: N checks, 0 failures`; other suites unchanged.
- [ ] **Step 5: Commit** "Launcher: ApkScan (ABIs and image sizes from an APK)".

### Task 2: `ArtPicker` — the backdrop choice

**Files:** Create `android/java/com/retrobug/launcher/ArtPicker.java`; test `tools/java-tests/ArtPickerTest.java`.

**Interfaces — Consumes:** `ApkScan.Image`. **Produces:** `final class ArtPicker { static ApkScan.Image pick(List<ApkScan.Image> images) }` — null when nothing qualifies.

Scoring (exact): an image qualifies when `width >= 800 && height >= 480`, or `height >= 800 && width >= 480`; paths
under `res/mipmap*` or containing `ic_launcher` are excluded. Score = `width*height / 10000` + 300 if the lower-cased
path contains any of `splash, loading, title, background, bg, cover, banner, menu` + 100 if `width > height`.
Ties: shorter path wins, then lexicographic.

- [ ] **Step 1: Failing test** — rows: keyword beats a larger plain image (`assets/splash.png` 1280×720 vs
  `assets/tex/a.png` 2048×2048 → splash); landscape beats portrait at equal keyword and size; 640×360 never chosen;
  `res/mipmap-xxxhdpi/ic_launcher.png` 1024² never chosen; empty list → null; ties by shorter path.
- [ ] **Step 2: Run** — Expected: compile FAIL. **Step 3: Implement.** **Step 4: Run** — Expected: 0 failures.
- [ ] **Step 5: Commit** "Launcher: ArtPicker (the backdrop from an APK's own art)".

### Task 3: `GameSettings` — the per-game record

**Files:** Create `android/java/com/retrobug/launcher/GameSettings.java`; test `tools/java-tests/GameSettingsTest.java`.

**Produces:** `final class GameSettings { String cpu = "fast"; boolean overlay = false; int target = 35;
static GameSettings parse(String text); String format(); }` — `format()` writes `cpu=…\noverlay=0|1\ntarget=N\n`;
`parse` ignores unknown keys and keeps defaults for missing or invalid values (`cpu` ∈ {fast, safe}; `target` ∈
24..36).

- [ ] **Step 1: Failing test** — round trip; defaults on empty text; `cpu=turbo` → fast; `target=99` → 35; unknown
  `foo=1` ignored. **Step 2–4** as above. **Step 5: Commit** "Launcher: GameSettings record".

### Task 4: `GameCatalog`, `ArtCache`, `ConvertJob`

**Files:** Create `android/java/com/retrobug/launcher/GameCatalog.java`, `ArtCache.java`, `ConvertJob.java`; modify
`android/AndroidManifest.xml` (add `<queries><intent><action MAIN/><category LAUNCHER/></intent></queries>` and
`REQUEST_DELETE_PACKAGES`); modify `LauncherActivity.java` (its `repackGame` body moves into `ConvertJob`; the
`-e repack` path calls `ConvertJob` and keeps writing `<label>.txt` / `<label>.apk` where it does today).

**Interfaces — Consumes:** `ApkScan`, `ArtPicker`, `ApkRepackager.transform`, `RepackSigner.sign`,
`RepackInstaller.install`. **Produces:**
- `final class Game { String key; String packageName; String label; long versionCode; File apk; boolean installed /*original*/; boolean added; State state; }`, `enum State { NOT_CONVERTED, CONVERTING, READY, UPDATE, UNREADABLE }`.
- `final class GameCatalog { GameCatalog(Context, Executor); void refresh(Listener); interface Listener { void onGame(Game g); void onDone(); } }` — installed launcher apps whose `sourceDir` APK `is32BitOnly()`, plus files in the launcher's games dir; READY when `<pkg>.rb` is installed and its `versionName` suffix `rb<N>` equals the launcher's runtime stamp, UPDATE when installed with an older stamp.
- `final class ArtCache { ArtCache(Context, Executor); void icon(Game, Consumer<Bitmap>); void backdrop(Game, Consumer<Bitmap>) }` — decoded with `inSampleSize` to ≤ the screen's longer side (backdrop) / 192 px (icon); cached under `getCacheDir()/art/<pkg>-<versionCode>-{icon,art}.png`; callbacks on the main thread.
- `final class ConvertJob { static boolean running(); static void start(Context, Game, GameSettings, Callback); interface Callback { void onProgress(String line); void onDone(boolean ok, String report); } }` — refuses (returns false) while one runs; converts with rename `.rb` and `settings.target`; installs through `RepackInstaller`.
- `ApkRepackager` sets the output's `versionName` to `<original>-rb<stamp>` (stamp = `RUNTIME_STAMP`, a constant in `ApkRepackager`, starting at 1).

- [ ] **Step 1: Failing test** — in `ApkRepackagerTest`, a row: the output's versionName is `<original>-rb1`.
- [ ] **Step 2: Run** — Expected: FAIL (versionName unchanged). **Step 3: Implement** the stamp in `ApkRepackager` (`AxmlEditor` gets `setVersionName`), then `GameCatalog`, `ArtCache`, `ConvertJob`, the manifest changes, and the `repackGame` move.
- [ ] **Step 4: Run** `bash tools/run_java_tests.sh` — Expected: 0 failures. Build `bash tools/build_launcher_apk.sh` — Expected: rc 0. Device check: `$TEMP/game.sh build/fnskittles.apk com.halfbrick.fruitninjavsskittles` still prints `Success` (the scripted path works).
- [ ] **Step 5: Commit** "Launcher: game catalog, art cache, convert job; copies carry a runtime stamp".

### Task 5: `HoloTheme` and `BootScreen`

**Files:** Create `android/java/com/retrobug/launcher/ui/HoloTheme.java`, `ui/BootScreen.java`.

**Produces:** `HoloTheme` constants (Global Constraints colours), `int accent(Context)` (from the app setting),
`Drawable panel()`, `Drawable tile(boolean pressed)`, `StateListDrawable button()`, `TextView title(...)`, `View
actionBar(Context, String title, View... actions)`, `void fadeRise(View v, int index)` (Xenra's card entrance: 220 ms,
40 ms stagger, 24 dp rise). `BootScreen`: the RetroBug wordmark in Roboto Light with a cyan glow sweep, ~1.5 s, a tap
skips; then `onFinished` runs.

- [ ] **Step 1:** build and install; **Step 2:** screenshot the boot on the tablet and the OnePlus — Expected: the
  wordmark centred, the sweep visible, no clipping in portrait or landscape. **Step 3: Commit** "Launcher: Holo theme and boot screen".

### Task 6: `LibraryScreen` — grid, badges, tap to convert or play

**Files:** Create `ui/LibraryScreen.java`, `ui/GameTile.java`; modify `LauncherActivity.java` (route HOME to it after
the boot screen).

**Consumes:** `GameCatalog`, `ArtCache`, `ConvertJob`, `HoloTheme`. Behaviour: action bar "RetroBug" + "Add APK" (the
existing picker); a `GridView` of `GameTile`s (icon 72 dp, name, badge); tiles `fadeRise` as icons arrive. Tap:
NOT_CONVERTED/UPDATE → `ConvertJob.start` (tile shows an indeterminate ring; a second tap while running shows a toast
"Already getting this game ready"); READY → `startActivity(getPackageManager().getLaunchIntentForPackage(pkg + ".rb"))`;
UNREADABLE → toast "Can't read this file". A `PACKAGE_ADDED/REMOVED/REPLACED` receiver (registered `RECEIVER_EXPORTED`
is not needed: these are system broadcasts) refreshes the catalog.

- [ ] **Step 1:** build, install on the tablet. **Step 2: Device check** — Expected: every installed 32-bit game appears
  with its own icon within ~2 s (Fruit Ninja, Mickey, Looney, KitKat, Cars, ABGo, Gumball …); scrolling smooth; tap on
  a NOT_CONVERTED game → ring → Android install prompt (tap Install) → tile READY; tap READY → the game opens.
  **Step 3: Commit** "Launcher: library grid; tap converts, then plays".

### Task 7: `GameMenu` — art header and four tabs

**Files:** Create `ui/GameMenu.java`, `ui/SettingsTab.java`, `ui/ModsTab.java`, `ui/PatchesTab.java`, `ui/DeleteTab.java`;
`GameSettingsStore.java` (files under `getFilesDir()/games/<pkg>.properties`, via `GameSettings`).

**Behaviour (spec):** header = `ArtCache.backdrop` dimmed 50 % and blurred (a downscale-upscale blur; no RenderScript) —
with no backdrop, the icon scaled up and blurred the same way —
icon, title, the state's action button, Details fold. Settings: Permissions (`ACTION_APPLICATION_DETAILS_SETTINGS`,
`package:<pkg>.rb`, disabled until converted), CPU mode, Android target (24/28/33/35; a change marks the game UPDATE),
Performance overlay, Clear game data (confirmation; then `ActivityManager`-free path: open the copy's app-info page —
the launcher has no permission to clear another app's data, so the button says "Opens the game's storage page"),
Resolution (disabled, "Coming with M6"). Mods / Patches: empty states. Delete: "Remove the playable copy"
(`Intent.ACTION_DELETE`, `package:<pkg>.rb`) and, for added APKs, "Remove from RetroBug" (deletes the stored file and
its settings); both behind a confirmation.

- [ ] **Step 1:** build, install. **Step 2: Device check** on both devices — Expected: long press opens the menu with
  the game's art; each tab renders; Permissions opens Android's page for the copy; Delete → Android's uninstall prompt
  for `<pkg>.rb` only. **Step 3: Commit** "Launcher: game menu (art, settings, mods, patches, delete)".

### Task 8: Settings channel to converted games

**Files:** Create `android/java/com/retrobug/launcher/SettingsProvider.java`; modify `android/AndroidManifest.xml`
(the provider, `android:exported="true"`, authority `com.retrobug.launcher.settings`); modify
`ApkRepackager.java` (always add the shim provider and a `<queries><provider android:authorities="com.retrobug.launcher.settings"/></queries>`
element — `AxmlEditor.addQueriesProvider`); modify `android/shim/com/retrobug/shim/RbShimProvider.java` (fetch
`content://com.retrobug.launcher.settings/<original pkg>`, write the text to `getFilesDir()/rb-settings.properties`);
modify `android/jni/runtime_boot.cpp` (read that file at boot: `cpu=safe` → Tier I for inbound and constructors;
`overlay=1` → the overlay: the shim adds a small text view to each resumed activity's decor view, refreshed once a
second from a new JNI entry in librbguest, `Java_com_retrobug_shim_Overlay_nativeStats`, which returns
`"<fps> fps · <M> M instr/s"` — frames counted at `eglSwapBuffers` (its HostSlots counter), the rate from the slice
probe the profiler already keeps).

**Interfaces:** `SettingsProvider.query` → a one-row cursor `(settings TEXT)` with `GameSettings.format()`, only when
`getCallingPackage()`'s signing certificate equals `RepackSigner`'s certificate (`AndroidKeyStore` alias
`retrobug-repack`); else an empty cursor. `AxmlEditor.addQueriesProvider(String authority) : boolean`.

- [ ] **Step 1: Failing test** — `AxmlEditorTest.addsQueriesProvider` (aapt2 dump shows `queries` → `provider`
  with the authority; a second call adds nothing). **Step 2: Run** — Expected: FAIL. **Step 3: Implement** the editor
  method, the repackager wiring, the provider, the shim fetch, the runtime read.
- [ ] **Step 4: Run** Java tests — Expected: 0 failures. Device check: set Fruit Ninja's CPU mode to Safe, start it —
  Expected: logcat shows `natives on Tier I`; set Fast — `Tier J`; uninstall the launcher's provider path by starting
  the game with the launcher force-stopped — Expected: still starts (defaults or last values).
- [ ] **Step 5: Commit** "Settings reach converted games through the launcher's provider".

### Task 9: App Settings, Developer corner, docs, both devices

**Files:** Create `ui/SettingsScreen.java`, `ui/DeveloperScreen.java` (today's self-test, reports, diagnostics views,
moved); modify `LauncherActivity.java` (only routing remains); modify `HANDOFF.md`.

- [ ] **Step 1:** move the views; accent picker (writes the app preference; `HoloTheme.accent` reads it); version
  tapped 7× reveals Developer ("You're now a developer", Android's own phrase is not copied — RetroBug says
  "Developer tools are on").
- [ ] **Step 2: Run** `bash tools/run_java_tests.sh`, `RB_SERIAL=R52Y106BECZ bash tools/run_engine_tests.sh arm64-v8a`,
  same with `75ff8cd3`, `bash tools/run_bridge_check.sh 75ff8cd3` — Expected: all green (the engine is untouched).
- [ ] **Step 3: Device pass** — screenshots of every screen on the tablet (landscape) and the OnePlus (portrait); the
  spec's end-to-end run on Fruit Ninja vs Skittles; the Review Focus items checked by hand (install a game while the
  launcher is open; a corrupt APK in the games dir; double tap during convert; launcher force-stopped before a game
  starts; a 4096² art PNG fixture).
- [ ] **Step 4: Commit** "Launcher: Holo redesign done — settings, developer corner, docs".
