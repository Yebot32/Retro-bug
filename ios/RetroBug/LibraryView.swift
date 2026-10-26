// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The library: import an APK, then play it. Imports go through UIKit's document picker (`asCopy`), and
// every step is written to Documents/retrobug.log — a failure the user can send us, not a silent no-op.
// The folder is also visible in Files (UIFileSharingEnabled): drop an APK into RetroBug › Games.

import SwiftUI
import UniformTypeIdentifiers

struct ImportedGame: Identifiable {
    let id = UUID()
    let url: URL
    var name: String { url.lastPathComponent }
}

struct LibraryView: View {
    private enum ActiveSheet: String, Identifiable {
        case importer, share
        var id: String { rawValue }
    }

    @State private var games: [ImportedGame] = []
    @State private var sheet: ActiveSheet?
    @State private var playing: ImportedGame?
    @State private var libName = "libgame.so"
    @State private var notice = ""

    private var gamesDir: URL {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return docs.appendingPathComponent("Games", isDirectory: true)
    }

    private var buildLabel: String {
        let info = Bundle.main.infoDictionary ?? [:]
        let version = info["CFBundleShortVersionString"] as? String ?? "?"
        let build = info["CFBundleVersion"] as? String ?? "?"
        let commit = info["RBGitCommit"] as? String ?? "?"
        return "RetroBug \(version) (\(build)) · \(commit)"
    }

    var body: some View {
        NavigationView {
            List {
                Section("Games") {
                    if games.isEmpty {
                        Text("No games yet. Import an APK with +, or copy one into On My iPhone › RetroBug › Games.")
                            .foregroundColor(.secondary)
                    }
                    ForEach(games) { game in
                        HStack {
                            Text(game.name)
                            Spacer()
                            Button("Play") { play(game) }
                        }
                    }
                    .onDelete(perform: delete)
                }
                Section("Library to boot") {
                    TextField("libgame.so", text: $libName)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                }
                if !notice.isEmpty {
                    Section("Last action") {
                        Text(notice).font(.footnote)
                    }
                }
                Section {
                    Button {
                        AppLog.write("share: log requested")
                        sheet = .share
                    } label: {
                        Label("Share log", systemImage: "square.and.arrow.up")
                    }
                    Text("retrobug.log is also in On My iPhone › RetroBug.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                } header: {
                    Text("Diagnostics")
                } footer: {
                    Text(buildLabel).font(.caption2)
                }
            }
            .navigationTitle("RetroBug")
            .toolbar {
                ToolbarItemGroup(placement: .navigationBarTrailing) {
                    Button {
                        AppLog.write("picker: requested")
                        sheet = .importer
                    } label: {
                        Image(systemName: "plus")
                    }
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
                GameView(apk: game.url, lib: libName)
            }
        }
        .navigationViewStyle(.stack)
        .onAppear(perform: prepare)
        .onOpenURL { url in
            AppLog.write("openURL: \(url.path)")
            importURL(url)
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
        let files = (try? FileManager.default.contentsOfDirectory(at: gamesDir, includingPropertiesForKeys: nil)) ?? []
        games = files
            .filter { $0.pathExtension.lowercased() == "apk" }
            .sorted { $0.lastPathComponent.localizedCaseInsensitiveCompare($1.lastPathComponent) == .orderedAscending }
            .map { ImportedGame(url: $0) }
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
            notice = "\(url.lastPathComponent) is not an APK"
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
            notice = "added \(dest.lastPathComponent) (\(readable))\(kind)"
            AppLog.write("import: copied to \(dest.path) size=\(size)\(kind)")
        } catch {
            notice = "could not copy \(url.lastPathComponent): \(error.localizedDescription)"
            AppLog.write("import: FAILED: \(error)")
        }
        reload()
    }

    private func delete(at offsets: IndexSet) {
        for index in offsets {
            AppLog.write("delete: \(games[index].name)")
            try? FileManager.default.removeItem(at: games[index].url)
        }
        reload()
    }
}
