// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The library: import an APK from Files, then play it. The engine itself is the shared core/; this view
// only copies the file into the app's Documents so the session can read it, and names the library to boot.

import SwiftUI
import UniformTypeIdentifiers

struct ImportedGame: Identifiable {
    let id = UUID()
    let url: URL
    var name: String { url.lastPathComponent }
}

struct LibraryView: View {
    @State private var games: [ImportedGame] = []
    @State private var importing = false
    @State private var playing: ImportedGame?
    @State private var libName = "libgame.so"
    @State private var notice = ""

    private var gamesDir: URL {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return docs.appendingPathComponent("Games", isDirectory: true)
    }

    var body: some View {
        NavigationView {
            List {
                Section("Games") {
                    if games.isEmpty {
                        Text("Import an APK with the + button.")
                            .foregroundColor(.secondary)
                    }
                    ForEach(games) { game in
                        HStack {
                            Text(game.name)
                            Spacer()
                            Button("Play") { playing = game }
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
                    Section("Last import") { Text(notice).font(.footnote) }
                }
            }
            .navigationTitle("RetroBug")
            .toolbar {
                Button {
                    importing = true
                } label: {
                    Image(systemName: "plus")
                }
            }
            .fileImporter(
                isPresented: $importing,
                allowedContentTypes: [UTType(filenameExtension: "apk") ?? .data],
                allowsMultipleSelection: false
            ) { result in
                importGame(result)
            }
            .fullScreenCover(item: $playing) { game in
                GameView(apk: game.url, lib: libName)
            }
        }
        .navigationViewStyle(.stack)
        .onAppear(perform: reload)
    }

    private func reload() {
        let files = (try? FileManager.default.contentsOfDirectory(at: gamesDir, includingPropertiesForKeys: nil)) ?? []
        games = files
            .filter { $0.pathExtension.lowercased() == "apk" }
            .sorted { $0.lastPathComponent.localizedCaseInsensitiveCompare($1.lastPathComponent) == .orderedAscending }
            .map { ImportedGame(url: $0) }
    }

    private func importGame(_ result: Result<[URL], Error>) {
        switch result {
        case .failure(let error):
            notice = "could not import: \(error.localizedDescription)"
        case .success(let urls):
            guard let url = urls.first else { return }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            do {
                try FileManager.default.createDirectory(at: gamesDir, withIntermediateDirectories: true)
                let dest = gamesDir.appendingPathComponent(url.lastPathComponent)
                try? FileManager.default.removeItem(at: dest)
                try FileManager.default.copyItem(at: url, to: dest)
                notice = "copied \(url.lastPathComponent)"
            } catch {
                notice = "could not copy: \(error.localizedDescription)"
            }
            reload()
        }
    }

    private func delete(at offsets: IndexSet) {
        for index in offsets {
            try? FileManager.default.removeItem(at: games[index].url)
        }
        reload()
    }
}
