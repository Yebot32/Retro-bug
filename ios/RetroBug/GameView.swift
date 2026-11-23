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
    let accent: Color
    @Environment(\.dismiss) private var dismiss
    @AppStorage("performanceOverlay") private var performanceOverlay = false
    @AppStorage("renderScale") private var renderScale = 1.0
    @AppStorage("runtimeJit") private var runtimeJit = true

    var body: some View {
        ZStack(alignment: .topLeading) {
            GameSurface(apk: apk, lib: lib, performanceOverlay: performanceOverlay,
                        renderScale: renderScale, runtimeJit: runtimeJit)
                .ignoresSafeArea()
            Button {
                dismiss()
            } label: {
                Text("EXIT")
                    .font(Holo.medium(13))
                    .kerning(0.5)
                    .foregroundColor(Holo.text)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 9)
                    .background(Holo.panel.opacity(0.92))
                    .overlay(alignment: .bottom) { Rectangle().fill(accent).frame(height: 2) }
            }
            .padding(16)
        }
        .statusBarHidden(true)
    }
}

private struct GameSurface: UIViewRepresentable {
    let apk: URL
    let lib: String
    let performanceOverlay: Bool
    let renderScale: Double
    let runtimeJit: Bool

    func makeUIView(context: Context) -> GameUIView {
        GameUIView(apk: apk, lib: lib, performanceOverlay: performanceOverlay,
                   renderScale: renderScale, runtimeJit: runtimeJit)
    }

    func updateUIView(_ uiView: GameUIView, context: Context) {}
}

final class GameUIView: UIView {
    private let apk: URL
    private let lib: String
    private let performanceOverlay: Bool
    private let runtimeJit: Bool
    private var session: OpaquePointer?
    private var started = false
    private var lastSize = CGSize.zero

    private let errorLabel = UILabel()
    private let bootLabel = UILabel()
    private let performanceLabel = UILabel()
    private var timer: Timer?
    private var lastFrameSample: (frames: Int32, time: TimeInterval)?

    private var touchIds: [ObjectIdentifier: Int32] = [:]
    private var nextTouch: Int32 = 0

    override class var layerClass: AnyClass { CAEAGLLayer.self }

    init(apk: URL, lib: String, performanceOverlay: Bool, renderScale: Double, runtimeJit: Bool) {
        self.apk = apk
        self.lib = lib
        self.performanceOverlay = performanceOverlay
        self.runtimeJit = runtimeJit
        super.init(frame: .zero)
        backgroundColor = .black
        isMultipleTouchEnabled = true
        contentScaleFactor = UIScreen.main.scale * CGFloat(min(1.0, max(0.5, renderScale)))
        layer.contentsScale = contentScaleFactor
        if let eagl = layer as? CAEAGLLayer {
            eagl.drawableProperties = [
                kEAGLDrawablePropertyRetainedBacking: false,
                kEAGLDrawablePropertyColorFormat: kEAGLColorFormatRGBA8,
            ]
        }
        for label in [errorLabel, bootLabel] {
            label.numberOfLines = 0
            label.textColor = .white
            label.font = .systemFont(ofSize: 12, weight: .regular)
            label.translatesAutoresizingMaskIntoConstraints = false
            label.layer.shadowColor = UIColor.black.cgColor
            label.layer.shadowOpacity = 0.8
            label.layer.shadowRadius = 3
            addSubview(label)
        }
        bootLabel.text = "STARTING…"
        bootLabel.font = .systemFont(ofSize: 15, weight: .medium)
        bootLabel.textColor = UIColor.white.withAlphaComponent(0.55)
        NSLayoutConstraint.activate([
            errorLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            errorLabel.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -12),
            errorLabel.centerYAnchor.constraint(equalTo: centerYAnchor),
            bootLabel.centerXAnchor.constraint(equalTo: centerXAnchor),
            bootLabel.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
        if performanceOverlay {
            performanceLabel.font = .monospacedDigitSystemFont(ofSize: 12, weight: .medium)
            performanceLabel.textColor = .white
            performanceLabel.backgroundColor = UIColor.black.withAlphaComponent(0.7)
            performanceLabel.translatesAutoresizingMaskIntoConstraints = false
            addSubview(performanceLabel)
            NSLayoutConstraint.activate([
                performanceLabel.topAnchor.constraint(equalTo: safeAreaLayoutGuide.topAnchor, constant: 12),
                performanceLabel.trailingAnchor.constraint(equalTo: safeAreaLayoutGuide.trailingAnchor, constant: -12),
            ])
        }
        boot()
        // The render thread stops drawing and calls the game's nativeOnPause/Resume itself; this only
        // flips a flag, so a backgrounding app never blocks on the guest.
        NotificationCenter.default.addObserver(
            self, selector: #selector(appWillResignActive), name: UIApplication.willResignActiveNotification,
            object: nil)
        NotificationCenter.default.addObserver(
            self, selector: #selector(appDidBecomeActive), name: UIApplication.didBecomeActiveNotification,
            object: nil)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    deinit {
        NotificationCenter.default.removeObserver(self)
        timer?.invalidate()
        if let s = session {
            rb_ios_stop_async(s)
        }
    }

    @objc private func appWillResignActive() {
        guard let s = session else { return }
        AppLog.write("game: app resign active -> pause")
        rb_ios_pause(s)
    }

    @objc private func appDidBecomeActive() {
        guard let s = session else { return }
        AppLog.write("game: app active -> resume")
        rb_ios_resume(s)
    }

    static func dataDir() -> String {
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
        rb_ios_set_runtime_jit(created, runtimeJit ? 1 : 0)
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
            bootLabel.isHidden = true
        }
        let frames = rb_ios_frame_count(s)
        bootLabel.isHidden = frames > 0 || !(errorLabel.text ?? "").isEmpty
        if performanceOverlay && frames > 0 {
            let time = CACurrentMediaTime()
            if let previous = lastFrameSample, frames >= previous.frames, time > previous.time {
                let fps = Double(frames - previous.frames) / (time - previous.time)
                let engine = rb_ios_session_engine(s)
                let mode = engine == 2 ? "JIT" : engine == 1 ? "AOT" : "Interpreter"
                performanceLabel.text = String(format: " %.1f FPS · %@ ", fps, mode)
            }
            lastFrameSample = (frames, time)
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        guard let s = session, bounds.width > 1, bounds.height > 1 else { return }
        if started && lastSize == bounds.size { return }
        lastSize = bounds.size
        lastFrameSample = nil
        if started {
            rb_ios_stop(s)
            started = false
        }
        let scale = contentScaleFactor
        let w = Int32(bounds.width * scale)
        let h = Int32(bounds.height * scale)
        let screen = window?.windowScene?.screen ?? UIScreen.main
        let rotation: Int32
        switch window?.windowScene?.interfaceOrientation {
        case .landscapeLeft: rotation = 1
        case .portraitUpsideDown: rotation = 2
        case .landscapeRight: rotation = 3
        default: rotation = 0
        }
        rb_ios_set_display_info(s, rotation, Int32((160 * screen.scale).rounded()),
                               Float(screen.maximumFramesPerSecond))
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
            // EXIT dismisses the game; the guest may be inside a long load, so the stop and teardown run
            // off the main thread. Touching the session after this is a use-after-free by design.
            session = nil
            AppLog.write("game: exit requested (background shutdown)")
            rb_ios_stop_async(s)
            started = false
            lastSize = .zero
            timer?.invalidate()
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
