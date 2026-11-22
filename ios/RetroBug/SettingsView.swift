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
    @AppStorage("performanceOverlay") private var performanceOverlay = false
    @AppStorage("renderScale") private var renderScale = 1.0
    @AppStorage("runtimeJit") private var runtimeJit = true

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

                    HoloSection(title: "Performance", accent: holo.accent)
                    HoloRow(title: "Performance overlay", subtitle: "Show FPS and the active CPU mode",
                            accent: holo.accent, action: { performanceOverlay.toggle() }) {
                        HoloValue(text: performanceOverlay ? "On" : "Off", accent: holo.accent)
                    }
                    HoloDivider()
                    HoloRow(title: "Render scale", subtitle: "Lower resolution reduces graphics work. Applies on launch.",
                            accent: holo.accent, action: {
                        renderScale = renderScale > 0.75 ? 0.75 : renderScale > 0.5 ? 0.5 : 1.0
                    }) {
                        HoloValue(text: "\(Int(renderScale * 100))%", accent: holo.accent)
                    }
                    HoloDivider()
                    HoloRow(title: "Runtime JIT", subtitle: "Use JIT when iOS grants access. Applies on launch.",
                            accent: holo.accent, action: { runtimeJit.toggle() }) {
                        HoloValue(text: runtimeJit ? "Automatic" : "Off", accent: holo.accent)
                    }
                    HoloDivider()

                    HoloSection(title: "About", accent: holo.accent)
                    HoloRow(title: "RetroBug",
                            subtitle: "Translates 32-bit ARM game code and provides Android APIs for supported games.")
                    HoloDivider()

                    HoloRow(title: "Version", subtitle: nil, accent: holo.accent,
                            action: { versionTapped() }) {
                        HoloValue(text: versionLine, accent: holo.accent)
                    }
                    HoloDivider()
                    HoloRow(title: "Share log",
                            subtitle: "retrobug.log — the last run's log, for a bug report",
                            accent: holo.accent,
                            action: onShareLog)
                    HoloDivider()

                    HoloSection(title: "Community", accent: holo.accent)
                    communityLink("Discord", "Join Yebot Studios", "https://discord.gg/hPefjfkkVs")
                    communityLink("GitHub", "Source code and releases", "https://github.com/Yebot32/Retro-bug")
                    communityLink("YouTube", "Watch Yebot32’s videos", "https://www.youtube.com/@Yebot32")
                    communityLink("PayPal", "Support development", "https://www.paypal.com/paypalme/Yebot32")

                    if holo.developer {
                        HoloSection(title: "Developer", accent: holo.accent)
                        HoloRow(title: "Engine",
                                subtitle: runtimeJit && rb_ios_jit_available() != 0
                                    ? "A matching translated profile runs first, followed by JIT and the interpreter."
                                    : "A matching translated profile runs first, followed by the interpreter.")
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

    private func communityLink(_ title: String, _ subtitle: String, _ url: String) -> some View {
        HoloRow(title: title, subtitle: subtitle, accent: holo.accent, action: {
            guard let destination = URL(string: url) else { return }
            openURL(destination)
        })
    }

    @Environment(\.openURL) private var openURL

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
