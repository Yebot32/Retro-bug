// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A game's page, opened by a long press on its tile — the Android launcher's dialog in the same Holo
// dress: the icon, the name and the one action a tap would take, then the rows that apply on iOS
// (details, the log, removing the APK). The panel is the Android dialog's shape: centred, dimmed
// outside, a bitmap header over a panel body.

import SwiftUI
import UIKit

struct GameMenuView: View {
    let game: ImportedGame
    let icon: UIImage?
    @ObservedObject var holo: HoloState

    let onPlay: () -> Void
    let onShareLog: () -> Void
    let onDelete: () -> Void
    let onClose: () -> Void

    @State private var showDetails = false

    private var detailsText: String {
        let fm = FileManager.default
        let attrs = try? fm.attributesOfItem(atPath: game.url.path)
        let size = (attrs?[.size] as? NSNumber)?.int64Value ?? 0
        let modified: String
        if let date = attrs?[.modificationDate] as? Date {
            modified = formatter.string(from: date)
        } else {
            modified = "?"
        }
        let info = Bundle.main.infoDictionary ?? [:]
        let build = info["CFBundleVersion"] as? String ?? "?"
        return """
        file      \(game.url.lastPathComponent)
        size      \(ByteCountFormatter.string(fromByteCount: size, countStyle: .file))
        modified  \(modified)
        folder    \(game.url.deletingLastPathComponent().path)
        build     \(build)
        """
    }

    private var formatter: DateFormatter {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm"
        return f
    }

    var body: some View {
        ZStack {
            Color.black.opacity(0.6).ignoresSafeArea().onTapGesture { onClose() }
            VStack(spacing: 0) {
                header
                if showDetails {
                    Text(detailsText)
                        .font(.system(size: 13, design: .monospaced))
                        .foregroundColor(Holo.text2)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(20)
                        .background(Color(rgb: 0x151515))
                }
                ScrollView {
                    LazyVStack(spacing: 0) {
                        HoloRow(title: "Details",
                                subtitle: "The APK RetroBug plays, where it is, and this build",
                                accent: holo.accent,
                                action: { showDetails.toggle() }) {
                            HoloValue(text: showDetails ? "HIDE" : "SHOW", accent: holo.accent)
                        }
                        HoloDivider()
                        HoloRow(title: "Share log",
                                subtitle: "The running log, including this game's boot",
                                accent: holo.accent,
                                action: onShareLog)
                        HoloDivider()
                        HoloRow(title: "Remove from RetroBug",
                                subtitle: "Deletes the APK you added. The original game is not touched.",
                                accent: holo.accent,
                                action: onDelete)
                        HoloDivider()
                    }
                }
                .frame(maxHeight: 260)
            }
            .frame(maxWidth: 760)
            .background(Holo.panel)
            .clipShape(RoundedRectangle(cornerRadius: 2))
        }
    }

    private var header: some View {
        HStack(spacing: 16) {
            Group {
                if let icon = icon {
                    Image(uiImage: icon).resizable().scaledToFit()
                } else {
                    Image("GameDefault").resizable().scaledToFit()
                }
            }
            .frame(width: 72, height: 72)

            VStack(alignment: .leading, spacing: 3) {
                HStack(alignment: .firstTextBaseline, spacing: 12) {
                    Text(game.name)
                        .font(Holo.light(24))
                        .foregroundColor(Holo.text)
                        .lineLimit(2)
                    Spacer(minLength: 8)
                    Button(action: onClose) {
                        Text("CLOSE")
                            .font(Holo.medium(13))
                            .kerning(0.5)
                            .foregroundColor(holo.accent)
                            .padding(.horizontal, 10)
                            .padding(.vertical, 6)
                    }
                    .buttonStyle(HoloTileButtonStyle(accent: holo.accent))
                }
                Text("Ready to play")
                    .font(Holo.regular(13))
                    .foregroundColor(Holo.text2)
                Button(action: onPlay) {
                    Text("PLAY")
                }
                .buttonStyle(HoloSlabButtonStyle(accent: holo.accent, primary: true))
                .frame(width: 132)
                .padding(.top, 8)
            }
        }
        .padding(20)
        .background(
            ZStack {
                Holo.panelRaised
                LinearGradient(colors: [Color.black.opacity(0.0), Color.black.opacity(0.35)],
                               startPoint: .top, endPoint: .bottom)
            }
        )
        .overlay(alignment: .bottom) { Rectangle().fill(holo.accent).frame(height: 2) }
    }
}
