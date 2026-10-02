import SwiftUI

struct ContentView: View {
    @State private var addresses: [String] = []
    @State private var permission = LocalNetworkPermission()

    private var host: String { addresses.first ?? "<iPad IP>" }

    var body: some View {
        NavigationStack {
            List {
                Section("송출 (iPad → PC)") {
                    HStack(spacing: 16) {
                        BroadcastPicker()
                            .frame(width: 64, height: 64)
                        Text("버튼을 누르고 ‘PadLink 송출’을 골라 방송을 시작하세요. 제어 센터의 화면 기록 버튼을 길게 눌러도 됩니다.")
                    }
                    if BroadcastPicker.extensionBundleID == nil {
                        Text("방송 확장을 찾지 못했습니다. 설치가 잘못됐을 수 있어요.")
                            .foregroundStyle(.red)
                    }
                }

                Section("펜 (iPad → PC)") {
                    Button("펜 모드 시작") { openPenMode() }
                    Text("Apple Pencil 입력(필압·기울기·배럴 롤·호버)을 PC로 보냅니다. 스퀴즈와 더블탭은 PC에서 단축키로 바뀝니다.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }

                Section("PC 접속 정보") {
                    if addresses.isEmpty {
                        Text("Wi-Fi 주소 없음 (USB로는 접속 가능)")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(addresses, id: \.self) { address in
                        LabeledContent("Wi-Fi IP", value: address)
                    }
                    LabeledContent("송출", value: "\(Wire.port)")
                    LabeledContent("펜", value: "\(Wire.penPort)")
                    LabeledContent("원시 영상 (H.264)", value: "\(Wire.rawVideoPort)")
                    LabeledContent("원시 소리 (WAV)", value: "\(Wire.rawAudioPort)")
                }

                Section("ffplay로 바로 확인 (방송 중에)") {
                    command("ffplay -fflags nobuffer -flags low_delay -framedrop -f h264 tcp://\(host):\(Wire.rawVideoPort)")
                    command("ffplay -nodisp -f wav tcp://\(host):\(Wire.rawAudioPort)")
                }
            }
            .navigationTitle("PadLink")
        }
        .onAppear(perform: refresh)
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willEnterForegroundNotification)) { _ in
            refresh()
        }
    }

    private func command(_ text: String) -> some View {
        Text(text)
            .font(.system(.footnote, design: .monospaced))
            .textSelection(.enabled)
    }

    /// 시스템 제스처 막기·상태 막대 숨기기가 먹도록 UIKit으로 직접 전체 화면 표시
    private func openPenMode() {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        guard let window = scenes.flatMap(\.windows).first(where: \.isKeyWindow) ?? scenes.first?.windows.first,
              var top = window.rootViewController else { return }
        while let presented = top.presentedViewController { top = presented }
        let pen = PenViewController()
        pen.modalPresentationStyle = .fullScreen
        top.present(pen, animated: true)
    }

    private func refresh() {
        addresses = NetInfo.ipv4Addresses()
        permission.request()
    }
}
