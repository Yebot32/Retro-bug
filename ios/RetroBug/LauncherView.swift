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
    let id = UUID()
    let url: URL
    let readable: Bool
    var name: String { url.deletingPathExtension().lastPathComponent }
}

struct LauncherView: View {
    private enum ActiveSheet: String, Identifiable {
        case importer, share
        var id: String { rawValue }
    }

    @StateObject private var holo = HoloState()
    @StateObject private var icons = IconStore()
    @State private var games: [ImportedGame] = []
    @State private var sheet: ActiveSheet?
    @State private var showSettings = false
    @State private var menuGame: ImportedGame?
    @State private var pendingDelete: ImportedGame?
    @State private var playing: ImportedGame?
    @State private var libName = ""
    @State private var toast: String?

    private var gamesDir: URL {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return docs.appendingPathComponent("Games", isDirectory: true)
    }

    var body: some View {
        ZStack {
            Holo.bg.ignoresSafeArea()
            VStack(spacing: 0) {
                HoloActionBar(title: "RetroBug", accent: holo.accent) {
                    HoloBarAction(label: "Add APK", accent: holo.accent) {
                        AppLog.write("picker: requested")
                        sheet = .importer
                    }
                    HoloBarAction(label: "Settings", accent: holo.accent) {
                        showSettings = true
                    }
                }
                if games.isEmpty {
                    empty
                } else {
                    grid
                }
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
        .sheet(item: $sheet) { which in
            switch which {
            case .importer:
                DocumentPicker(onPick: importURL, onDone: { sheet = nil })
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
                ForEach(games) { game in
                    GameTileView(game: game, icon: icons.icon(for: game.url), accent: holo.accent,
                                 onTap: { play(game) },
                                 onLongPress: { menuGame = game })
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
            For now use a bootcheck APK (assets/rb32/ and assets/rb32sys/ inside), not a store APK.
            """
            try? text.write(to: readme, atomically: true, encoding: .utf8)
        }
        AppLog.write("library: opened, games dir \(gamesDir.path)")
        reload()
    }

    private func reload() {
        let fm = FileManager.default
        let files = (try? fm.contentsOfDirectory(at: gamesDir, includingPropertiesForKeys: nil)) ?? []
        games = files
            .filter { $0.pathExtension.lowercased() == "apk" }
            .sorted { $0.lastPathComponent.localizedCaseInsensitiveCompare($1.lastPathComponent) == .orderedAscending }
            .map { url in
                var readable = false
                if let head = try? FileHandle(forReadingFrom: url).read(upToCount: 4), head.count == 4 {
                    let magic = [UInt8](head)
                    readable = magic == [0x50, 0x4b, 0x03, 0x04] || magic == [0x50, 0x4b, 0x05, 0x06]
                }
                return ImportedGame(url: url, readable: readable)
            }
        for game in games { icons.load(game.url) }
        AppLog.write("library: \(games.count) game(s): \(games.map { $0.name })")
    }

    private func play(_ game: ImportedGame) {
        AppLog.write("play: \(game.name) lib=\(libName)")
        playing = game
    }

    /// Copies an APK into the library. `asCopy` gives a URL inside our container; the extra security-scope
    /// call is harmless and logged, because a wrong answer there is exactly the failure that used to look
    /// like "nothing happened".
    private func importURL(_ url: URL) {
        let fm = FileManager.default
        AppLog.write("import: \(url.path) exists=\(fm.fileExists(atPath: url.path))")
        guard url.pathExtension.lowercased() == "apk" else {
            show("\(url.lastPathComponent) is not an APK")
            AppLog.write("import: refused, extension is \(url.pathExtension)")
            return
        }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        AppLog.write("import: security-scoped=\(scoped)")
        do {
            try fm.createDirectory(at: gamesDir, withIntermediateDirectories: true)
            let dest = gamesDir.appendingPathComponent(url.lastPathComponent)
            try? fm.removeItem(at: dest)
            try fm.copyItem(at: url, to: dest)
            let size = ((try? fm.attributesOfItem(atPath: dest.path)[.size]) as? NSNumber)?.int64Value ?? 0
            let readable = ByteCountFormatter.string(fromByteCount: size, countStyle: .file)
            var kind = ""
            if let head = try? FileHandle(forReadingFrom: dest).read(upToCount: 4), head.count == 4 {
                let magic = [UInt8](head)
                if magic != [0x50, 0x4b, 0x03, 0x04] && magic != [0x50, 0x4b, 0x05, 0x06] {
                    kind = " (not a zip/APK)"
                }
            }
            show("Added \(dest.lastPathComponent) (\(readable))\(kind)")
            AppLog.write("import: copied to \(dest.path) size=\(size)\(kind)")
        } catch {
            show("Could not copy \(url.lastPathComponent): \(error.localizedDescription)")
            AppLog.write("import: FAILED: \(error)")
        }
        reload()
    }

    private func deleteConfirmed(_ game: ImportedGame) {
        AppLog.write("delete: \(game.url.lastPathComponent)")
        try? FileManager.default.removeItem(at: game.url)
        pendingDelete = nil
        show("Removed \(game.name)")
        reload()
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
    let game: ImportedGame
    let icon: UIImage?
    let accent: Color
    let onTap: () -> Void
    let onLongPress: () -> Void

    /// A long press must not also count as the tap that plays the game. Whichever recognizer SwiftUI lets
    /// through, the flag makes the pair behave: the long press arms it, the button action spends it.
    @State private var suppressTap = false

    private var badge: (String, Color) {
        game.readable ? ("READY", accent) : ("CAN'T READ THIS FILE", Holo.danger)
    }

    var body: some View {
        Button {
            if suppressTap {
                suppressTap = false
            } else {
                onTap()
            }
        } label: {
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
            .contentShape(Rectangle())
        }
        .buttonStyle(HoloTileButtonStyle(accent: accent))
        .onLongPressGesture(minimumDuration: 0.45,
                            pressing: { down in if down { suppressTap = false } },
                            perform: {
                                suppressTap = true
                                onLongPress()
                            })
    }
}
