import Foundation
import Network

enum NetInfo {
    /// Wi-Fi 등 en* 인터페이스의 IPv4 주소
    static func ipv4Addresses() -> [String] {
        var result: [String] = []
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return [] }
        defer { freeifaddrs(head) }
        for node in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let ifa = node.pointee
            guard let addr = ifa.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET),
                  String(cString: ifa.ifa_name).hasPrefix("en") else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count),
                           nil, 0, NI_NUMERICHOST) == 0 {
                result.append(String(cString: host))
            }
        }
        return result
    }
}

/// 로컬 네트워크 권한 팝업은 확장에서 뜨지 않으므로 앱에서 미리 띄운다
final class LocalNetworkPermission {
    private var browser: NWBrowser?

    func request() {
        guard browser == nil else { return }
        let b = NWBrowser(for: .bonjour(type: Wire.bonjourType, domain: nil), using: .tcp)
        b.start(queue: .main)
        browser = b
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) { [weak self] in
            self?.browser?.cancel()
            self?.browser = nil
        }
    }
}
