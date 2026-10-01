import ReplayKit
import SwiftUI

/// 시스템 화면 방송 시작 버튼
struct BroadcastPicker: UIViewRepresentable {
    func makeUIView(context: Context) -> RPSystemBroadcastPickerView {
        let picker = RPSystemBroadcastPickerView(frame: CGRect(x: 0, y: 0, width: 64, height: 64))
        picker.preferredExtension = Self.extensionBundleID
        picker.showsMicrophoneButton = false
        return picker
    }

    func updateUIView(_ uiView: RPSystemBroadcastPickerView, context: Context) {}

    /// SideStore가 재서명하면서 번들 ID를 바꾸므로 하드코딩하지 않고 PlugIns에서 찾는다
    static let extensionBundleID: String? = {
        guard let dir = Bundle.main.builtInPlugInsURL,
              let items = try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)
        else { return nil }
        for url in items where url.pathExtension == "appex" {
            guard let bundle = Bundle(url: url),
                  let ext = bundle.infoDictionary?["NSExtension"] as? [String: Any],
                  ext["NSExtensionPointIdentifier"] as? String == "com.apple.broadcast-services-upload"
            else { continue }
            return bundle.bundleIdentifier
        }
        return nil
    }()
}
