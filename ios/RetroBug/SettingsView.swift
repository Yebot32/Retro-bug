// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// RetroBug's own settings, as the Android launcher's Holo preference rows: the accent colour (Holo blue,
// green, orange, purple) and About. Tapping the version seven times turns on the developer tools — the
// engine line, the library to boot, the log — kept out of a player's way, the way Android keeps its own.

import SwiftUI

struct SettingsView: View {
    @ObservedObject var holo: HoloState
    @Binding var libName: String
    let onBack: () -> Void
    let onShareLog: () -> Void

    @State private var taps = 0
    @State private var toast: String?

    private var versionLine: String {
        let info = Bundle.main.infoDictionary ?? [:]
        let version = info["CFBundleShortVersionString"] as? String ?? "?"
        let build = info["CFBundleVersion"] as? String ?? "?"
        let commit = info["RBGitCommit"] as? String ?? "?"
        return "\(version) (\(build)) · \(commit)"
    }

    var body: some View {
        VStack(spacing: 0) {
            HoloActionBar(title: "Settings", accent: holo.accent) {
                HoloBarAction(label: "Done", accent: holo.accent, action: onBack)
            }
            ScrollView {
                LazyVStack(spacing: 0) {
                    HoloSection(title: "Look", accent: holo.accent)
                    HoloRow(title: "Accent colour",
                            subtitle: "The colour of highlights, buttons and the action bar's line",
                            accent: holo.accent,
                            action: { holo.nextAccent() }) {
                        HoloValue(text: holo.accentName, accent: holo.accent)
                    }
                    HoloDivider()

                    HoloSection(title: "About", accent: holo.accent)
                    HoloRow(title: "RetroBug",
                            subtitle: "Plays 32-bit Android games on a 64-bit iPhone or iPad by running their "
                                + "ARM code in an emulator, while the game's Java runs as usual.")
                    HoloDivider()
                    HoloRow(title: "Version", subtitle: nil, accent: holo.accent,
                            action: { versionTapped() }) {
                        HoloValue(text: versionLine, accent: holo.accent)
                    }
                    HoloDivider()

                    if holo.developer {
                        HoloSection(title: "Developer", accent: holo.accent)
                        HoloRow(title: "Engine",
                                subtitle: rb_ios_jit_available() != 0
                                    ? "Automatic: Tier J when this build can map executable memory, else the "
                                        + "game's translated profile, else the interpreter"
                                    : "Automatic: no executable memory in this build, so the game's translated "
                                        + "profile, else the interpreter")
                        HoloDivider()
                        HoloRow(title: "Library to boot",
                                subtitle: "Empty means auto (the largest library in the APK)",
                                accent: holo.accent) {
                            TextField("auto", text: $libName)
                                .font(Holo.regular(15))
                                .foregroundColor(holo.accent)
                                .multilineTextAlignment(.trailing)
                                .autocorrectionDisabled()
                                .textInputAutocapitalization(.never)
                                .frame(width: 240)
                        }
                        HoloDivider()
                        HoloRow(title: "Share log",
                                subtitle: "retrobug.log, also in On My iPhone › RetroBug",
                                accent: holo.accent,
                                action: onShareLog)
                        HoloDivider()
                        HoloRow(title: "Turn off developer tools", accent: holo.accent,
                                action: { holo.developer = false })
                        HoloDivider()
                    }
                }
                .padding(.bottom, 40)
            }
        }
        .background(Holo.bg.ignoresSafeArea())
        .overlay(alignment: .bottom) {
            if let toast = toast {
                HoloToast(text: toast)
            }
        }
    }

    private func versionTapped() {
        if holo.developer {
            show("Developer tools are already on")
            return
        }
        taps += 1
        if taps >= 7 {
            holo.developer = true
            show("Developer tools are on")
        } else if taps >= 4 {
            show("\(7 - taps) more to turn on developer tools")
        }
    }

    private func show(_ text: String) {
        toast = text
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.2) {
            if toast == text { toast = nil }
        }
    }
}
