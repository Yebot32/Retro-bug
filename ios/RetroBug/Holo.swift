// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The look, ported from the Android launcher's HoloTheme: the same near-black surfaces, the same four
// Holo accents, Roboto, the 56 dp action bar with its 3 dp accent underline, flat 2 dp buttons, the
// 40 %-accent pressed state, and the 128 dp app-drawer grid. The constants are the Android ones, because
// "looks like the Android app" means the same values, not a palette that resembles them.

import SwiftUI
import UIKit

extension Color {
    init(rgb: UInt32) {
        self.init(red: Double((rgb >> 16) & 0xFF) / 255.0,
                  green: Double((rgb >> 8) & 0xFF) / 255.0,
                  blue: Double(rgb & 0xFF) / 255.0)
    }
}

enum Holo {
    static let bg = Color(rgb: 0x101010)
    static let panel = Color(rgb: 0x1B1B1B)
    static let panelRaised = Color(rgb: 0x262626)
    static let divider = Color(rgb: 0x2A2A2A)
    static let text = Color.white
    static let text2 = Color(rgb: 0xB3B3B3)
    static let text3 = Color(rgb: 0x7A7A7A)
    static let danger = Color(rgb: 0xFF4444)

    static let accentNames = ["Holo blue", "Holo green", "Holo orange", "Holo purple"]
    static let accents = [Color(rgb: 0x33B5E5), Color(rgb: 0x99CC00), Color(rgb: 0xFFBB33), Color(rgb: 0xAA66CC)]

    /// Text that reads on any of the light Holo accents, as the Android theme uses.
    static let darkOnAccent = Color(rgb: 0x101010)

    /// Roboto, the platform family the Android launcher asks for by name. Registered through UIAppFonts;
    /// a build that misses them falls back to the system font instead of failing.
    static func light(_ size: CGFloat) -> Font { .custom("Roboto-Light", size: size) }
    static func regular(_ size: CGFloat) -> Font { .custom("Roboto-Regular", size: size) }
    static func medium(_ size: CGFloat) -> Font { .custom("Roboto-Medium", size: size) }
    static func condensed(_ size: CGFloat) -> Font { .custom("RobotoCondensed-Regular", size: size) }
}

/// The accent and the developer flag moved from the Android app's "ui" preferences: the same colours,
/// the same seven-tap developer switch, so the two apps read the same way.
final class HoloState: ObservableObject {
    @Published var accentIndex: Int {
        didSet { UserDefaults.standard.set(accentIndex, forKey: "accent") }
    }
    @Published var developer: Bool {
        didSet { UserDefaults.standard.set(developer, forKey: "developer") }
    }

    init() {
        let saved = UserDefaults.standard.object(forKey: "accent") as? Int ?? 0
        accentIndex = (saved >= 0 && saved < Holo.accents.count) ? saved : 0
        developer = UserDefaults.standard.bool(forKey: "developer")
    }

    var accent: Color { Holo.accents[accentIndex] }
    var accentName: String { Holo.accentNames[accentIndex] }

    func nextAccent() { accentIndex = (accentIndex + 1) % Holo.accents.count }
}

// MARK: - Action bar

struct HoloActionBar<Actions: View>: View {
    let title: String
    let accent: Color
    let actions: Actions

    init(title: String, accent: Color, @ViewBuilder actions: () -> Actions) {
        self.title = title
        self.accent = accent
        self.actions = actions()
    }

    var body: some View {
        HStack(spacing: 0) {
            Text(title)
                .font(Holo.regular(21))
                .foregroundColor(Holo.text)
                .lineLimit(1)
            Spacer(minLength: 8)
            actions
        }
        .padding(.leading, 16)
        .padding(.trailing, 8)
        .frame(height: 56)
        .background(Holo.panel)
        .overlay(alignment: .bottom) {
            Rectangle().fill(accent).frame(height: 3)
        }
    }
}

struct HoloBarAction: View {
    let label: String
    let accent: Color
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(label.uppercased())
                .font(Holo.medium(14))
                .kerning(0.5)
                .foregroundColor(accent)
                .padding(.horizontal, 14)
                .padding(.vertical, 14)
                .contentShape(Rectangle())
        }
        .buttonStyle(HoloTileButtonStyle(accent: accent))
    }
}

// MARK: - Button styles (Holo flat slabs and the pressed tile)

struct HoloTileButtonStyle: ButtonStyle {
    let accent: Color

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .background(configuration.isPressed ? accent.opacity(0.4) : Color.clear)
    }
}

struct HoloSlabButtonStyle: ButtonStyle {
    let accent: Color
    let primary: Bool

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(Holo.medium(16))
            .kerning(0.5)
            .foregroundColor(primary ? Holo.darkOnAccent : Holo.text)
            .padding(.horizontal, 20)
            .padding(.vertical, 12)
            .frame(maxWidth: primary ? .infinity : nil)
            .background(
                RoundedRectangle(cornerRadius: 2)
                    .fill(configuration.isPressed
                          ? (primary ? accent.opacity(0.75) : accent.opacity(0.4))
                          : (primary ? accent : Holo.panelRaised)))
    }
}

// MARK: - Preference rows (Settings screen and the game dialog)

struct HoloRow<Trailing: View>: View {
    let title: String
    let subtitle: String?
    let enabled: Bool
    let accent: Color
    let action: (() -> Void)?
    let trailing: Trailing

    init(title: String, subtitle: String? = nil, enabled: Bool = true, accent: Color = Holo.accents[0],
         action: (() -> Void)? = nil, @ViewBuilder trailing: () -> Trailing) {
        self.title = title
        self.subtitle = subtitle
        self.enabled = enabled
        self.accent = accent
        self.action = action
        self.trailing = trailing()
    }

    var body: some View {
        let content = HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(Holo.regular(17))
                    .foregroundColor(enabled ? Holo.text : Holo.text3)
                if let subtitle = subtitle {
                    Text(subtitle)
                        .font(Holo.regular(13))
                        .foregroundColor(enabled ? Holo.text2 : Holo.text3)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 8)
            trailing
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Holo.bg)

        Group {
            if let action = action, enabled {
                Button(action: action) { content }
                    .buttonStyle(HoloTileButtonStyle(accent: accent))
            } else {
                content
            }
        }
    }
}

extension HoloRow where Trailing == EmptyView {
    init(title: String, subtitle: String? = nil, enabled: Bool = true, accent: Color = Holo.accents[0],
         action: (() -> Void)? = nil) {
        self.init(title: title, subtitle: subtitle, enabled: enabled, accent: accent, action: action) {
            EmptyView()
        }
    }
}

struct HoloValue: View {
    let text: String
    let accent: Color

    var body: some View {
        Text(text)
            .font(Holo.medium(15))
            .foregroundColor(accent)
    }
}

struct HoloDivider: View {
    var body: some View { Rectangle().fill(Holo.divider).frame(height: 1) }
}

struct HoloSection: View {
    let title: String
    let accent: Color

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title.uppercased())
                .font(Holo.medium(14))
                .kerning(0.6)
                .foregroundColor(accent)
                .padding(.leading, 20)
                .padding(.top, 24)
                .padding(.bottom, 6)
            Rectangle()
                .fill(accent.opacity(0.53))
                .frame(height: 1)
                .padding(.horizontal, 16)
        }
    }
}

// MARK: - Toast and confirm dialog (Holo shapes, not UIAlertController's)

struct HoloToast: View {
    let text: String

    var body: some View {
        Text(text)
            .font(Holo.regular(14))
            .foregroundColor(Holo.text)
            .padding(.horizontal, 16)
            .padding(.vertical, 11)
            .background(RoundedRectangle(cornerRadius: 2).fill(Holo.panelRaised))
            .shadow(color: .black.opacity(0.6), radius: 6, y: 3)
            .padding(.bottom, 28)
    }
}

struct HoloConfirm: View {
    let title: String
    let message: String
    let confirmLabel: String
    let accent: Color
    let onConfirm: () -> Void
    let onCancel: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.6).ignoresSafeArea().onTapGesture { onCancel() }
            VStack(alignment: .leading, spacing: 0) {
                Text(title)
                    .font(Holo.regular(19))
                    .foregroundColor(Holo.text)
                    .padding(.horizontal, 20)
                    .padding(.top, 20)
                Text(message)
                    .font(Holo.regular(14))
                    .foregroundColor(Holo.text2)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 20)
                    .padding(.top, 10)
                HStack(spacing: 0) {
                    Spacer(minLength: 0)
                    Button(action: onCancel) {
                        Text("CANCEL")
                            .font(Holo.medium(14))
                            .kerning(0.5)
                            .foregroundColor(accent)
                            .padding(.horizontal, 14)
                            .padding(.vertical, 12)
                    }
                    .buttonStyle(HoloTileButtonStyle(accent: accent))
                    Button(action: onConfirm) {
                        Text(confirmLabel.uppercased())
                            .font(Holo.medium(14))
                            .kerning(0.5)
                            .foregroundColor(accent)
                            .padding(.horizontal, 14)
                            .padding(.vertical, 12)
                    }
                    .buttonStyle(HoloTileButtonStyle(accent: accent))
                }
                .padding(.horizontal, 8)
                .padding(.bottom, 8)
                .padding(.top, 6)
            }
            .frame(maxWidth: 380)
            .background(Holo.panel)
        }
    }
}

// MARK: - Game icons

/// The APK's own launcher image, read by the engine (rb_ios_apk_icon), decoded once per file and cached.
final class IconStore: ObservableObject {
    @Published private(set) var icons: [String: UIImage] = [:]
    private var loading = Set<String>()

    func icon(for url: URL) -> UIImage? { icons[url.path] }

    func load(_ url: URL) {
        let key = url.path
        if icons[key] != nil || loading.contains(key) { return }
        loading.insert(key)
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            var length: UInt32 = 0
            let bytes = key.withCString { rb_ios_apk_icon($0, &length) }
            var image: UIImage?
            if let bytes = bytes, length > 0 {
                let data = Data(bytes: bytes, count: Int(length))
                rb_ios_free_bytes(UnsafeMutableRawPointer(bytes))
                image = UIImage(data: data)
            }
            DispatchQueue.main.async {
                guard let self = self else { return }
                self.loading.remove(key)
                if let image = image { self.icons[key] = image }
            }
        }
    }
}
