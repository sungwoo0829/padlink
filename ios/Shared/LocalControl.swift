import Foundation
import Network

/// 같은 iPad 안의 앱과 방송 확장 사이 제어 메시지. 상대 서버(127.0.0.1)에 잠깐 붙어서 보낸다.
/// 펜 모드(액정타블렛)와 송출은 USB 대역을 나눠 쓰지 않도록 하나만 켜 둔다.
enum LocalControl {
    private static let queue = DispatchQueue(label: "padlink.local-control")

    static func send(_ message: [String: Any], toPort port: UInt16) {
        guard let payload = try? JSONSerialization.data(withJSONObject: message),
              let nwPort = NWEndpoint.Port(rawValue: port) else { return }
        let connection = NWConnection(host: "127.0.0.1", port: nwPort, using: .tcp)
        connection.stateUpdateHandler = { state in
            switch state {
            case .ready:
                connection.send(content: Wire.frame(.control, pts: 0, payload: payload),
                                completion: .contentProcessed { _ in
                                    queue.asyncAfter(deadline: .now() + 0.5) { connection.cancel() }
                                })
            case .waiting, .failed:
                connection.cancel()  // 상대가 꺼져 있음
            default:
                break
            }
        }
        connection.start(queue: queue)
        queue.asyncAfter(deadline: .now() + 3) { connection.cancel() }
    }

    static func parse(_ payload: Data) -> [String: Any]? {
        try? JSONSerialization.jsonObject(with: payload) as? [String: Any]
    }
}
