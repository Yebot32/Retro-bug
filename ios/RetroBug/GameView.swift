// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The game view: a CAEAGLLayer-backed UIView, the session's render thread, touch forwarding and a small
// status/error overlay. The engine's C surface is rb_ios.h (platform/ios).

import SwiftUI
import UIKit
import QuartzCore

struct GameView: View {
    let apk: URL
    let lib: String
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        ZStack(alignment: .topLeading) {
            GameSurface(apk: apk, lib: lib)
                .ignoresSafeArea()
            Button {
                dismiss()
            } label: {
                Label("Exit", systemImage: "xmark")
                    .font(.caption)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(Color.black.opacity(0.55))
                    .foregroundColor(.white)
                    .clipShape(Capsule())
            }
            .padding(16)
        }
        .statusBarHidden(true)
    }
}

private struct GameSurface: UIViewRepresentable {
    let apk: URL
    let lib: String

    func makeUIView(context: Context) -> GameUIView {
        GameUIView(apk: apk, lib: lib)
    }

    func updateUIView(_ uiView: GameUIView, context: Context) {}
}

final class GameUIView: UIView {
    private let apk: URL
    private let lib: String
    private var session: OpaquePointer?
    private var started = false
    private var lastSize = CGSize.zero

    private let errorLabel = UILabel()
    private let statusLabel = UILabel()
    private var timer: Timer?

    private var touchIds: [ObjectIdentifier: Int32] = [:]
    private var nextTouch: Int32 = 0

    override class var layerClass: AnyClass { CAEAGLLayer.self }

    init(apk: URL, lib: String) {
        self.apk = apk
        self.lib = lib
        super.init(frame: .zero)
        backgroundColor = .black
        contentScaleFactor = UIScreen.main.scale
        if let eagl = layer as? CAEAGLLayer {
            eagl.drawableProperties = [
                kEAGLDrawablePropertyRetainedBacking: false,
                kEAGLDrawablePropertyColorFormat: kEAGLColorFormatRGBA8,
            ]
        }
        for label in [errorLabel, statusLabel] {
            label.numberOfLines = 0
            label.textColor = .white
            label.font = .systemFont(ofSize: 12, weight: .regular)
            label.translatesAutoresizingMaskIntoConstraints = false
            label.layer.shadowColor = UIColor.black.cgColor
            label.layer.shadowOpacity = 0.8
            label.layer.shadowRadius = 3
            addSubview(label)
        }
        NSLayoutConstraint.activate([
            errorLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            errorLabel.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -12),
            errorLabel.centerYAnchor.constraint(equalTo: centerYAnchor),
            statusLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            statusLabel.topAnchor.constraint(equalTo: topAnchor, constant: 44),
        ])
        boot()
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    deinit {
        timer?.invalidate()
        if let s = session {
            rb_ios_stop(s)
            rb_ios_destroy(s)
        }
    }

    private static func dataDir() -> String {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = base.appendingPathComponent("RetroBug", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.path
    }

    private func boot() {
        var err = [CChar](repeating: 0, count: 16384)
        let dir = GameUIView.dataDir()
        AppLog.write("game: booting \(apk.lastPathComponent) lib=\(lib) dir=\(dir)")
        var created: OpaquePointer?
        apk.path.withCString { apkPtr in
            lib.withCString { libPtr in
                dir.withCString { dirPtr in
                    AppLog.url.path.withCString { logPtr in
                        created = rb_ios_create(apkPtr, libPtr, dirPtr, logPtr, 2, &err, Int32(err.count))
                    }
                }
            }
        }
        guard let created else {
            let text = String(cString: err)
            AppLog.write("game: create FAILED: \(text)")
            errorLabel.text = text
            return
        }
        AppLog.write("game: session created")
        session = created
        statusLabel.text = "booting…"
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            self?.refreshStatus()
        }
    }

    private var lastError = ""

    private func refreshStatus() {
        guard let s = session else { return }
        var err = [CChar](repeating: 0, count: 4096)
        rb_ios_last_error(s, &err, Int32(err.count))
        let text = String(cString: err)
        if !text.isEmpty && text != lastError {
            lastError = text
            AppLog.write("game: error: \(text)")
            errorLabel.text = text
        }
        let frames = rb_ios_frame_count(s)
        statusLabel.text = started ? "\(frames) frames" : "starting…"
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        guard let s = session, bounds.width > 1, bounds.height > 1 else { return }
        if started && lastSize == bounds.size { return }
        lastSize = bounds.size
        if started {
            rb_ios_stop(s)
            started = false
        }
        let scale = contentScaleFactor
        let w = Int32(bounds.width * scale)
        let h = Int32(bounds.height * scale)
        var err = [CChar](repeating: 0, count: 4096)
        let pointer = UnsafeMutableRawPointer(Unmanaged.passUnretained(layer).toOpaque())
        AppLog.write("game: start \(bounds.width)x\(bounds.height) @\(scale) -> \(w)x\(h) px")
        let ok = rb_ios_start(s, pointer, w, h, &err, Int32(err.count))
        started = ok != 0
        if started {
            AppLog.write("game: render thread started")
        } else {
            let text = String(cString: err)
            AppLog.write("game: start FAILED: \(text)")
            errorLabel.text = text
        }
    }

    override func willMove(toWindow newWindow: UIWindow?) {
        super.willMove(toWindow: newWindow)
        if newWindow == nil, let s = session {
            rb_ios_stop(s)
            started = false
            lastSize = .zero
        }
    }

    // ---- touches: Cocos2d-x's Java layer scales MotionEvent coordinates by contentScaleFactor and forwards
    // (id, x, y, source). Here the points already are in our view, so the scale is the screen's.
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            let id = nextTouch
            nextTouch += 1
            touchIds[ObjectIdentifier(touch)] = id
            send(0, touch)
        }
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches { send(1, touch) }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            send(2, touch)
            touchIds.removeValue(forKey: ObjectIdentifier(touch))
        }
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            send(3, touch)
            touchIds.removeValue(forKey: ObjectIdentifier(touch))
        }
    }

    private func send(_ kind: Int32, _ touch: UITouch) {
        guard let s = session, let id = touchIds[ObjectIdentifier(touch)] else { return }
        let point = touch.location(in: self)
        let scale = Float(contentScaleFactor)
        let x = Float(point.x) * scale
        let y = Float(point.y) * scale
        switch kind {
        case 0: rb_ios_touch_begin(s, id, x, y)
        case 1: rb_ios_touch_move(s, id, x, y)
        case 2: rb_ios_touch_end(s, id, x, y)
        default: rb_ios_touch_cancel(s, id, x, y)
        }
    }
}
