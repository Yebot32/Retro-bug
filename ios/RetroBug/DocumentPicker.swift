// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The import picker: UIKit's UIDocumentPickerViewController, not SwiftUI's fileImporter. `asCopy: true`
// hands back a copy inside the app's own container, so there is no security-scoped URL to get wrong, and
// both delegate callbacks are logged — the SwiftUI modifier left the user with a sheet that accepted taps
// and delivered nothing.

import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct DocumentPicker: UIViewControllerRepresentable {
    /// A file is copied into the app's container by the picker (`asCopy`), so there is no scope to manage.
    /// A folder cannot be copied — a folder of APKs is gigabytes — so it arrives as a security-scoped URL
    /// and the importer reads the APKs out of it in place.
    enum Mode {
        case apk
        case folder
    }

    var mode: Mode = .apk
    let onPick: (URL) -> Void
    let onDone: () -> Void

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker: UIDocumentPickerViewController
        switch mode {
        case .apk:
            AppLog.write("picker: presented (asCopy, .item)")
            picker = UIDocumentPickerViewController(forOpeningContentTypes: [.item], asCopy: true)
        case .folder:
            AppLog.write("picker: presented (open, .folder)")
            picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder], asCopy: false)
        }
        picker.allowsMultipleSelection = false
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ uiViewController: UIDocumentPickerViewController, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        private let parent: DocumentPicker
        init(_ parent: DocumentPicker) { self.parent = parent }

        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            AppLog.write("picker: picked \(urls.map { $0.lastPathComponent })")
            if let url = urls.first { parent.onPick(url) }
            parent.onDone()
        }

        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
            AppLog.write("picker: cancelled")
            parent.onDone()
        }
    }
}

/// Shares a file (the log) through the system sheet: AirDrop, Mail, Messages, Save to Files.
struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]

    func makeUIViewController(context: Context) -> UIActivityViewController {
        AppLog.write("share: presenting \(items.count) item(s)")
        return UIActivityViewController(activityItems: items, applicationActivities: nil)
    }

    func updateUIViewController(_ uiViewController: UIActivityViewController, context: Context) {}
}
