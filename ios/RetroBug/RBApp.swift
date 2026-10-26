// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The iOS app: a library of imported APKs, and the game view. The engine is the same core/ the Android app
// runs (S1); this file is only the entry point.

import SwiftUI

@main
struct RetroBugApp: App {
    var body: some Scene {
        WindowGroup {
            LibraryView()
        }
    }
}
