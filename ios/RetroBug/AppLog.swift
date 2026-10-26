// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// One crash-safe log for the app and the engine. Every line is appended to Documents/retrobug.log and
// flushed immediately, so the last lines survive a crash; the library's "Share log" sends it. The engine
// writes its own lines to the same file (rb_ios_create's log_path), so a failed boot is diagnosable without
// a Mac or a debugger.

import Foundation
import UIKit

enum AppLog {
    static let url: URL = {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return docs.appendingPathComponent("retrobug.log")
    }()

    private static let queue = DispatchQueue(label: "retrobug.log")
    private static var started = false

    static func start() {
        guard !started else { return }
        started = true
        let fm = FileManager.default
        try? fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        if let attrs = try? fm.attributesOfItem(atPath: url.path), let size = attrs[.size] as? Int, size > 2_000_000 {
            try? fm.removeItem(at: url)
        }
        let info = Bundle.main.infoDictionary ?? [:]
        let version = info["CFBundleShortVersionString"] as? String ?? "?"
        let build = info["CFBundleVersion"] as? String ?? "?"
        let commit = info["RBGitCommit"] as? String ?? "?"
        let device = UIDevice.current
        write("=== RetroBug \(version) (\(build)) commit \(commit) ===")
        write("iOS \(device.systemVersion) \(device.model) scale \(UIScreen.main.scale)")
        write("Documents: \(fm.urls(for: .documentDirectory, in: .userDomainMask)[0].path)")
    }

    static func write(_ message: String) {
        let stamp = ISO8601DateFormatter().string(from: Date())
        let line = "\(stamp) \(message)"
        print("retrobug: \(line)")
        queue.async {
            guard let data = (line + "\n").data(using: .utf8) else { return }
            if let handle = try? FileHandle(forWritingTo: url) {
                defer { try? handle.close() }
                _ = try? handle.seekToEnd()
                try? handle.write(contentsOf: data)
                try? handle.synchronize()
            } else {
                try? data.write(to: url)
            }
        }
    }
}
