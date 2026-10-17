# RetroBug launcher redesign: Holo era — design

Date: 2026-10-18. Branch: `ui-holo` (off `main` at fad28b3). Status: agreed in conversation, awaiting the human's
review of this file.

## Intent

The human wants RetroBug to feel like a nostalgic player's app, not a developer tool. Agreed in conversation:

- **Look:** the Android Holo era (Ice Cream Sandwich to KitKat, 2011–2014) — the era the games come from — with the
  polish of the human's Xenra app (colour ramps, entrance animations, a library that never blocks the UI thread,
  selection glow). Not Xenra's Xbox branding.
- **Library:** the 32-bit games already installed on the device, found automatically, plus APK files the human adds.
- **Art:** each game's icon on its tile; on the game's page, the best large image inside the APK (splash, loading or
  title art) as a backdrop, falling back to the icon.
- **Tap does the thing:** the first tap on a game that is not converted converts it and starts the install (one Android
  prompt); once installed, the next tap opens the game directly — the player never hunts for it on the phone.
- **Per-game menu:** a long press on a tile opens a menu with four tabs — Settings, Mods, Patches, Delete.

Assumptions (stated to the human, not corrected): players are the audience, so the diagnostics move behind a hidden
developer corner; it must work on the tablet (landscape) and phones (portrait); no new libraries.

Success: on the Tab S9 and the OnePlus, the library shows every installed 32-bit game with its real icon within a
second or two of start, scrolling stays smooth, a game goes from tile to playing in two taps plus Android's own install prompt (tap 1 converts and
installs, tap 2 plays), and every screen reads as 2012-era Android.

## Screens

**Boot.** A short boot animation in the spirit of the ICS glowing logo, with the RetroBug name; it plays while the
catalog scans and never delays a ready library by more than its own length (about 1.5 s; skippable by a tap).

**Library (home).** Holo dark background; an action bar with the RetroBug name, a cyan underline and an "Add APK"
action. An app-drawer grid of game tiles: icon, name, and a small state badge — *Not converted*, *Ready*,
*Update available* (the copy was made by an older RetroBug). Installed games and added APKs share the grid. Tiles fade
and rise in as their art arrives (Xenra's card entrance), on a background lane.

**Tap.** The tile's state decides: *Not converted* or *Update available* → convert and install (a progress ring on the
tile while converting; then Android's install prompt; the tile turns *Ready* when the install completes). *Ready* →
open the converted game at once (its launcher intent, `PackageManager.getLaunchIntentForPackage("<pkg>.rb")`). A
failure shows one plain sentence on the tile's menu header and keeps the tile in its previous state.

**Long press** → the game menu.

**Game menu (long press).** A Holo dialog. Its header is the game's page: the chosen backdrop, dimmed and softly
blurred, with the icon, title and a cyan action button that follows the state ("Make playable", "Play", "Update"), a
status line (from the M5 compatibility table once it exists; until then omitted) and a "Details" fold (package,
version, size, 32-bit libraries, the last conversion report). Below it, four tabs:

- **Settings** (per game):
  - *Permissions* — opens Android's app-permission page for the game's converted copy
    (`Settings.ACTION_APPLICATION_DETAILS_SETTINGS`).
  - *CPU mode* — Fast (Tier J, default) or Safe (Tier I).
  - *Android target* — the targetSdk the copy is converted for; changing it re-converts (the existing `-e target`
    path). Shown with a one-line note on what older targets fix and that Play Protect may block them.
  - *Performance overlay* — on/off; an fps and guest-instruction-rate readout drawn over the game.
  - *Clear game data* — opens the converted copy's storage page, where Android's own "Clear storage" lives (one app
    cannot clear another's data); the original game is never touched.
  - *Resolution* — a render scale for games whose surface RetroBug sizes; listed but disabled with "Coming with M6"
    until it works.
- **Mods** — asset overlays (texture packs, translations, music), each with an on/off switch. Empty state ("Add a mod
  pack") until M6.
- **Patches** — code patches, function hooks and cheats, each with an on/off switch and a "made for version X" check.
  Empty state until M6.
- **Delete** — two choices, each with a confirmation: *Remove the playable copy* (uninstalls `<pkg>.rb` through
  Android's own uninstall prompt, `Intent.ACTION_DELETE`; needs the normal `REQUEST_DELETE_PACKAGES` permission) and,
  for an added APK, *Remove from RetroBug* (deletes the launcher's stored copy). The original installed game is never
  touched; an installed original stays in the library while it is installed.
**Settings (app).** Holo preference rows: accent colour (cyan default; green, orange, purple — a Xenra-style ramp),
what the library shows (installed / added / both), About. Tapping the version seven times reveals **Developer**: the
current self-test, reports and diagnostic run modes, moved unchanged.

## Parts

| Part | Job | Depends on | Tested by |
|---|---|---|---|
| `GameCatalog` | Lists games: installed launcher apps (a `<queries>` MAIN/LAUNCHER intent, no special permission) whose base APK has `lib/armeabi*/` and no `lib/arm64-v8a/`, plus added APKs; merges them; works out each game's state from whether `<pkg>.rb` is installed and which RetroBug version made it. | PackageManager (thin wrapper), `ApkScan` | PC test (`ApkScan`) + device check |
| `ApkScan` | Pure Java: from an APK's zip directory — its ABIs, its image entries with their pixel sizes read from PNG/JPEG/WebP headers only. | nothing Android | PC test with fixture APKs |
| `ArtPicker` | Pure Java: scores image entries for the backdrop — keyword in the path (splash, loading, title, background, bg, cover, banner) up; landscape up; under 800×480 ruled out; icons and 9-patches excluded; highest score wins, none → icon. | `ApkScan` | PC test with fixture entries |
| `ArtCache` | Decodes the icon (PackageManager / `getPackageArchiveInfo`) and the chosen backdrop once, downscaled to the screen, into the app cache keyed by package + version code. | Android | Device check |
| `HoloTheme` | Palette, accent ramp, Roboto, panel/button/tab drawables, press and focus states, edge glow. | Android | Screenshots |
| `GameSettingsStore` | The per-game settings (and later mods and patches), stored by the launcher and served to converted games through a provider guarded by a signature permission (only apps signed with the device's RetroBug repack key can read it). | Android | PC test for the record format; device check for the provider |
| Screens | `BootScreen`, `LibraryScreen`, `GameMenu` (header + four tabs), `SettingsScreen`, `DeveloperScreen`. `LauncherActivity` only routes between them. | the parts above | Screenshots on both devices |
| Runtime hook | At boot, the converted game's runtime asks the provider for its settings (CPU mode, overlay) and applies them; absent or unreadable → today's defaults. | `GameSettingsStore` | Device check |

Unchanged: `ApkRepackager`, `RepackInstaller`, `RepackSigner`, `AxmlEditor`, the engine.

## Data flow

Start → the boot screen plays while `GameCatalog` scans on a background executor. Each game found posts a tile; its
icon and backdrop come from `ArtCache` on the same lane and fade in. The UI thread never opens an APK or decodes an
image. Changes in the system (a game installed, a copy converted) refresh the catalog through a package broadcast.

## Errors

- An APK that cannot be read: a plain tile, "Can't read this file", never a crash.
- No large art: the menu header uses the icon over a blurred copy of itself.
- Conversion or install failure: a plain sentence in the menu header ("Couldn't convert: …"), with the full report under
  Details.
- Settings provider unreachable from a converted game: the runtime uses the settings it last saved (its defaults when
  it never saved any) and logs one line.
- A game that cannot be offered gets a tile that says why: its 32-bit code is only in a split APK, or an added file is
  64-bit or has no native code. A game with no Java code (hasCode="false") shows CPU mode and the overlay disabled,
  with the reason.

## Out of scope

The contents of Mods and Patches (M6); the per-game compatibility text (needs the M5 table); any change to how games
are converted; sounds (Holo had none).

## Testing

PC: `ApkScan` and `ArtPicker` rows in `tools/run_java_tests.sh` (fixture APKs built by the script, fixture images of
known sizes); the settings record format. Device: screenshots of every screen on the Tab S9 (landscape) and the
OnePlus (portrait), the library's time-to-icons, and an end-to-end run on Fruit Ninja vs Skittles — tap (converts, installs), tap (opens the game), long press → Delete
→ Remove the playable copy (uninstalled; the original untouched). Existing suites stay green.
