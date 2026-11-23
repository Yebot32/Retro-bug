// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
import SwiftUI

struct ReleaseUpdate: Decodable {
    struct Asset: Decodable {
        let name: String
        let state: String
        let browserDownloadUrl: URL
    }

    let tagName: String
    let draft: Bool
    let prerelease: Bool
    let body: String?
    let assets: [Asset]

    static func check() async -> ReleaseUpdate? {
        let endpoint = URL(string: "https://api.github.com/repos/Yebot32/Retro-bug/releases/latest")!
        var request = URLRequest(url: endpoint, timeoutInterval: 12)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        guard let (data, response) = try? await URLSession.shared.data(for: request),
              (response as? HTTPURLResponse)?.statusCode == 200 else { return nil }
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        guard let release = try? decoder.decode(Self.self, from: data),
              !release.draft, !release.prerelease, release.download != nil,
              let installed = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String,
              let current = version(installed), let latest = version(release.tagName),
              current.lexicographicallyPrecedes(latest) else { return nil }
        return release
    }

    private static func version(_ text: String) -> [Int]? {
        let trimmed = text.hasPrefix("v") ? text.dropFirst() : text[...]
        let parts = trimmed.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 3 else { return nil }
        let numbers = parts.compactMap { part -> Int? in
            guard !part.isEmpty, part.utf8.allSatisfy({ (48...57).contains($0) }) else { return nil }
            return Int(part)
        }
        return numbers.count == 3 ? numbers : nil
    }

    var download: URL? {
        assets.first { asset in
            let name = asset.name.lowercased()
            let url = asset.browserDownloadUrl
            return name.hasPrefix("retrobug") && name.hasSuffix(".ipa") && asset.state == "uploaded"
                && url.scheme == "https" && url.host == "github.com"
                && url.path == "/Yebot32/Retro-bug/releases/download/" + tagName + "/" + asset.name
        }?.browserDownloadUrl
    }
}

struct ReleaseUpdateSheet: View {
    let release: ReleaseUpdate
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    var body: some View {
        NavigationView {
            ScrollView {
                let notes = release.body ?? ""
                let markdown = try? AttributedString(markdown: notes,
                    options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))
                Text(markdown ?? AttributedString(notes))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
                    .padding()
            }
            .navigationTitle("RetroBug " + release.tagName)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Update") {
                        if let url = release.download { openURL(url) }
                        dismiss()
                    }
                }
            }
        }
        .navigationViewStyle(.stack)
    }
}
