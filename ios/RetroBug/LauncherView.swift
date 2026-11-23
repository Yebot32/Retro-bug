// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The launcher, as the Android app draws it: a Holo action bar (RetroBug, ADD APK, SETTINGS over the
// accent underline), the app-drawer grid of game tiles at 128 dp, a long press opening the game's page,
// a tap playing it. Imports go through UIKit's document picker (`asCopy`), and every step is written to
// Documents/retrobug.log — a failure the user can send us, not a silent no-op. The folder is also visible
// in Files (UIFileSharingEnabled): drop an APK into RetroBug › Games.

import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct ImportedGame: Identifiable {
    let url: URL
    let readable: Bool
    var id: URL { url }
    var name: String { url.deletingPathExtension().lastPathComponent }
}

struct LauncherView: View {
    private static let fileQueue = DispatchQueue(label: "com.retrobug.library", qos: .userInitiated)
    private enum ActiveSheet: String, Identifiable {
        case importer, importerFolder, share
        var id: String { rawValue }
    }

    @StateObject private var holo = HoloState()
    @StateObject private var icons = IconStore()
    @State private var games: [ImportedGame] = []
    @State private var sheet: ActiveSheet?
    @State private var addSource = false
    @State private var showSettings = false
    @State private var menuGame: ImportedGame?
    @State private var pendingDelete: ImportedGame?
    @State private var playing: ImportedGame?
    @State private var libName = ""
    @State private var toast: String?
    @State private var booting = true
    @State private var importsInFlight = 0
    @State private var handledQALaunch = false

    private var gamesDir: URL {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return docs.appendingPathComponent("Games", isDirectory: true)
    }

    var body: some View {
        ZStack {
            Holo.bg.ignoresSafeArea()
            VStack(spacing: 0) {
                HoloActionBar(title: "RetroBug", accent: holo.accent) {
                    HoloBarAction(label: importsInFlight == 0 ? "Add games" : "Importing…", accent: holo.accent) {
                        AppLog.write("picker: source asked for")
                        addSource = true
                    }
                    .disabled(importsInFlight != 0)
                    HoloBarAction(label: "Settings", accent: holo.accent) {
                        showSettings = true
                    }
                }
                if games.isEmpty {
                    empty
                } else if !booting {
                    // The tiles enter after the wordmark sweep, as the Android library does when its scan
                    // finishes: their fade-rise runs where it can be seen, not behind the boot screen.
                    grid
                } else {
                    Color.clear
                }
            }
            if booting {
                HoloBootScreen(accent: holo.accent) {
                    withAnimation(.easeOut(duration: 0.2)) { booting = false }
                }
                .transition(.opacity)
                .zIndex(1)
            }
            if showSettings {
                SettingsView(holo: holo, libName: $libName,
                             onBack: { showSettings = false },
                             onShareLog: { sheet = .share })
                    .transition(.opacity)
            }
            if let game = menuGame {
                GameMenuView(game: game, icon: icons.icon(for: game.url), holo: holo,
                             onPlay: { menuGame = nil; play(game) },
                             onShareLog: { menuGame = nil; sheet = .share },
                             onDelete: { menuGame = nil; pendingDelete = game },
                             onClose: { menuGame = nil })
            }
            if let pending = pendingDelete {
                HoloConfirm(title: "Remove from RetroBug?",
                            message: "The APK you added for \(pending.name) is deleted from RetroBug. "
                                + "The original game is not touched.",
                            confirmLabel: "Remove", accent: holo.accent,
                            onConfirm: { deleteConfirmed(pending) },
                            onCancel: { pendingDelete = nil })
            }
        }
        .preferredColorScheme(.dark)
        .overlay(alignment: .bottom) {
            if let toast = toast {
                HoloToast(text: toast)
            }
        }
        .confirmationDialog("Add games", isPresented: $addSource, titleVisibility: .visible) {
            Button("Choose APK or OBB") {
                AppLog.write("picker: requested (apk)")
                // The dialog's dismissal and the sheet's presentation in the same runloop step race: the
                // sheet is presented while the dialog is still on screen and UIKit drops it.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { sheet = .importer }
            }
            Button("Choose folder of APKs") {
                AppLog.write("picker: requested (folder)")
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { sheet = .importerFolder }
            }
            Button("Cancel", role: .cancel) {}
        }
        .sheet(item: $sheet) { which in
            switch which {
            case .importer:
                DocumentPicker(onPick: importURL, onDone: { sheet = nil })
            case .importerFolder:
                DocumentPicker(mode: .folder, onPick: importFolder, onDone: { sheet = nil })
            case .share:
                ShareSheet(items: [AppLog.url])
            }
        }
        .fullScreenCover(item: $playing) { game in
            GameView(apk: game.url, lib: libName, accent: holo.accent)
        }
        .onAppear {
            prepare()
        }
        .onOpenURL { url in
            AppLog.write("openURL: \(url.path)")
            importURL(url)
        }
    }

    private var empty: some View {
        VStack {
            Spacer()
            Text("No games yet. Add an APK to get started.")
                .font(Holo.light(16))
                .foregroundColor(Holo.text2)
                .multilineTextAlignment(.center)
                .padding(32)
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var grid: some View {
        ScrollView {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 128), spacing: 4, alignment: .top)],
                      spacing: 4) {
                ForEach(Array(games.enumerated()), id: \.element.id) { index, game in
                    GameTileView(game: game, icon: icons.icon(for: game.url),
                                 accent: holo.accent, index: index,
                                 onTap: { play(game) },
                                 onLongPress: { menuGame = game })
                        .onAppear { if game.readable { icons.load(game.url) } }
                }
            }
            .padding(8)
        }
    }

    private func prepare() {
        AppLog.start()
        let fm = FileManager.default
        try? fm.createDirectory(at: gamesDir, withIntermediateDirectories: true)
        let readme = gamesDir.appendingPathComponent("README.txt")
        if !fm.fileExists(atPath: readme.path) {
            let text = """
            RetroBug — put .apk files in this folder.
            Add APKs containing 32-bit ARM game code. Compatibility varies by game.
            """
            try? text.write(to: readme, atomically: true, encoding: .utf8)
        }
        AppLog.write("library: opened, games dir \(gamesDir.path)")
        reload()
    }

    private func reload() {
        let directory = gamesDir
        Self.fileQueue.async {
            let found = Self.apkFiles(in: directory).map { ImportedGame(url: $0, readable: Self.hasZipHeader($0)) }
            DispatchQueue.main.async {
                games = found
                AppLog.write("library: \(found.count) game(s)")
                runRequestedGame(in: found)
            }
        }
    }

    private func runRequestedGame(in games: [ImportedGame]) {
        guard !handledQALaunch,
              let fileName = ProcessInfo.processInfo.environment["RETROBUG_QA_GAME"] else { return }
        handledQALaunch = true
        guard let game = games.first(where: { $0.url.lastPathComponent == fileName }) else {
            AppLog.write("QA: requested APK is missing from the library")
            return
        }
        AppLog.write("QA: launching an imported APK")
        play(game)
    }

    private func play(_ game: ImportedGame) {
        guard game.readable else {
            show("This APK could not be read")
            return
        }
        AppLog.write("play: \(game.name) lib=\(libName)")
        playing = game
    }

    private static func apkFiles(in directory: URL) -> [URL] {
        let files = (try? FileManager.default.contentsOfDirectory(
            at: directory, includingPropertiesForKeys: [.isRegularFileKey], options: [.skipsHiddenFiles])) ?? []
        return files.filter {
            $0.pathExtension.lowercased() == "apk" &&
                (try? $0.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true
        }.sorted { $0.lastPathComponent.localizedCaseInsensitiveCompare($1.lastPathComponent) == .orderedAscending }
    }

    private static func hasZipHeader(_ url: URL) -> Bool {
        guard let file = try? FileHandle(forReadingFrom: url) else { return false }
        defer { try? file.close() }
        guard let data = try? file.read(upToCount: 4) else { return false }
        let magic = [UInt8](data)
        return magic == [0x50, 0x4b, 0x03, 0x04] || magic == [0x50, 0x4b, 0x05, 0x06]
    }

    /// Validate the staged copy before replacing an existing game. A failed copy leaves the old APK in place.
    private static func copyImport(_ source: URL, to destination: URL) throws {
        let fm = FileManager.default
        let directory = destination.deletingLastPathComponent()
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let staging = directory.appendingPathComponent(".import-\(UUID().uuidString)")
        defer { try? fm.removeItem(at: staging) }
        try fm.copyItem(at: source, to: staging)
        if destination.pathExtension.lowercased() == "apk" {
            guard hasZipHeader(staging) else { throw CocoaError(.fileReadCorruptFile) }
        } else {
            let values = try staging.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            guard values.isRegularFile == true, (values.fileSize ?? 0) > 0 else {
                throw CocoaError(.fileReadCorruptFile)
            }
        }
        if fm.fileExists(atPath: destination.path) {
            _ = try fm.replaceItemAt(destination, withItemAt: staging)
        } else {
            try fm.moveItem(at: staging, to: destination)
        }
    }

    private func importURL(_ url: URL) {
        let extensionName = url.pathExtension.lowercased()
        let apk = extensionName == "apk"
        let dest: URL
        if apk {
            dest = gamesDir.appendingPathComponent(url.lastPathComponent)
        } else if extensionName == "obb" {
            let parts = url.deletingPathExtension().lastPathComponent.split(separator: ".", maxSplits: 2)
            guard parts.count == 3, parts[0] == "main" || parts[0] == "patch",
                  parts[1].utf8.allSatisfy({ (48...57).contains($0) }),
                  String(parts[2]).range(of: #"\A[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+\z"#,
                                         options: .regularExpression) != nil else {
                show("Choose an OBB with its original Android expansion filename.")
                return
            }
            dest = URL(fileURLWithPath: GameUIView.dataDir(), isDirectory: true)
                .appendingPathComponent("obb", isDirectory: true)
                .appendingPathComponent(String(parts[2]), isDirectory: true)
                .appendingPathComponent(url.lastPathComponent)
        } else {
            show("Choose an APK or OBB file.")
            return
        }
        if url.standardizedFileURL == dest.standardizedFileURL {
            reload()
            return
        }
        let scoped = url.startAccessingSecurityScopedResource()
        importsInFlight += 1
        Self.fileQueue.async {
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            let result = Result { try Self.copyImport(url, to: dest) }
            DispatchQueue.main.async {
                importsInFlight -= 1
                switch result {
                case .success:
                    if apk {
                        icons.invalidate(dest)
                        icons.load(dest)
                    }
                    show("Added \(dest.lastPathComponent)")
                    AppLog.write("import: copied \(dest.lastPathComponent)")
                case .failure(let error):
                    show("Could not add \(url.lastPathComponent). Check that the file opens in Files and try again.")
                    AppLog.write("import: FAILED \(url.lastPathComponent): \(error.localizedDescription)")
                }
                reload()
            }
        }
    }

    /// Imports every `.apk` directly inside a chosen folder. The picker hands the folder over as a
    /// security-scoped URL (a folder is not copied into the container the way a single file is), so the
    /// scope is held for the whole batch. Existing names are skipped rather than overwritten: choosing a
    /// folder twice must not re-copy gigabytes or replace a game that is already playable.
    private func importFolder(_ folder: URL) {
        let fm = FileManager.default
        let scoped = folder.startAccessingSecurityScopedResource()
        AppLog.write("import: folder \(folder.path) security-scoped=\(scoped)")
        let destDir = gamesDir
        importsInFlight += 1
        Self.fileQueue.async {
            defer { if scoped { folder.stopAccessingSecurityScopedResource() } }
            let apks = Self.apkFiles(in: folder)
            var added = 0
            var skipped = 0
            var failed = 0
            for src in apks {
                let dest = destDir.appendingPathComponent(src.lastPathComponent)
                if fm.fileExists(atPath: dest.path) {
                    skipped += 1
                    AppLog.write("import: skipped existing \(src.lastPathComponent)")
                    continue
                }
                do {
                    try Self.copyImport(src, to: dest)
                    added += 1
                    AppLog.write("import: copied \(src.lastPathComponent)")
                } catch {
                    failed += 1
                    AppLog.write("import: FAILED \(src.lastPathComponent): \(error.localizedDescription)")
                }
            }
            DispatchQueue.main.async {
                importsInFlight -= 1
                if apks.isEmpty {
                    show("No APKs in that folder")
                } else {
                    var summary = "Added \(added) game" + (added == 1 ? "" : "s")
                    if skipped > 0 { summary += ", skipped \(skipped) already added" }
                    if failed > 0 { summary += ", \(failed) failed" }
                    show(summary)
                }
                reload()
            }
        }
    }

    private func deleteConfirmed(_ game: ImportedGame) {
        AppLog.write("delete: \(game.url.lastPathComponent)")
        pendingDelete = nil
        Self.fileQueue.async {
            let result = Result { try FileManager.default.removeItem(at: game.url) }
            DispatchQueue.main.async {
                switch result {
                case .success:
                    icons.invalidate(game.url)
                    show("Removed \(game.name)")
                case .failure(let error):
                    show("Could not remove \(game.name)")
                    AppLog.write("delete: FAILED \(error.localizedDescription)")
                }
                reload()
            }
        }
    }

    private func show(_ text: String) {
        toast = text
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.4) {
            if toast == text { toast = nil }
        }
    }
}

// MARK: - One game in the grid

struct GameTileView: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let game: ImportedGame
    let icon: UIImage?
    let accent: Color
    let index: Int
    let onTap: () -> Void
    let onLongPress: () -> Void

    /// A tile is not a Button: the button's own press handling wins arbitration and swallowed the long
    /// press (build 29's game page was unreachable). A plain view with the two gestures, plus this flag,
    /// keeps a long press from also playing the game whichever recognizer reports first.
    @State private var pressed = false
    @State private var suppressTap = false
    @State private var shown = false

    private var badge: (String, Color) {
        game.readable ? ("IMPORTED", accent) : ("CAN'T READ THIS FILE", Holo.danger)
    }

    var body: some View {
        VStack(spacing: 0) {
            Group {
                if let icon = icon {
                    Image(uiImage: icon).resizable().scaledToFit()
                } else {
                    Image("GameDefault").resizable().scaledToFit()
                }
            }
            .frame(width: 72, height: 72)

            Text(game.name)
                .font(Holo.regular(14))
                .foregroundColor(Holo.text)
                .multilineTextAlignment(.center)
                .lineLimit(2)
                .padding(.top, 8)

            Text(badge.0)
                .font(Holo.condensed(11))
                .kerning(0.3)
                .foregroundColor(badge.1)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
                .padding(.top, 2)
        }
        .frame(maxWidth: .infinity)
        .padding(.horizontal, 8)
        .padding(.top, 14)
        .padding(.bottom, 12)
        .background(pressed ? accent.opacity(0.4) : Color.clear)
        .contentShape(Rectangle())
        .opacity(shown ? 1 : 0)
        .offset(y: shown ? 0 : 24)
        .onAppear {
            if reduceMotion {
                shown = true
                return
            }
            // Holo's card entrance: fade in and rise 24 dp over 220 ms, 40 ms per index (capped at 10).
            withAnimation(.easeOut(duration: 0.22).delay(Double(min(index, 10)) * 0.04)) {
                shown = true
            }
        }
        .onTapGesture {
            if suppressTap {
                suppressTap = false
            } else {
                onTap()
            }
        }
        .onLongPressGesture(minimumDuration: 0.45,
                            pressing: { down in
                                pressed = down
                                if down { suppressTap = false }
                            },
                            perform: {
                                suppressTap = true
                                onLongPress()
                            })
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
        .accessibilityAction { onTap() }
        .accessibilityAction(named: Text("Game options")) { onLongPress() }
    }
}
