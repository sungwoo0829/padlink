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

                Section("PC 접속 정보") {
                    if addresses.isEmpty {
                        Text("Wi-Fi 주소 없음 (USB로는 접속 가능)")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(addresses, id: \.self) { address in
                        LabeledContent("Wi-Fi IP", value: address)
                    }
                    LabeledContent("프로토콜", value: "\(Wire.port)")
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

    private func refresh() {
        addresses = NetInfo.ipv4Addresses()
        permission.request()
    }
}
