import Foundation
import Network

/// iPad 쪽 TCP 서버. PC는 USB(usbmuxd)나 Wi-Fi로 여기에 접속한다.
/// USB(usbmux)는 PC → 기기 방향 연결만 되므로 iPad가 항상 듣는 쪽이다.
final class StreamServer {
    final class Client {
        let connection: NWConnection
        var isReady = false
        var pendingBytes = 0
        var waitingForKeyframe = true
        var keyframeRequested = false
        var inbox = Data()
        init(_ connection: NWConnection) { self.connection = connection }
    }

    /// 이보다 많이 밀리면 영상은 다음 키프레임까지 버리고, 소리는 그 조각을 버린다
    static let maxPendingBytes = 4 << 20

    let port: UInt16
    let name: String
    let bonjourName: String?
    /// 접속 직후 가장 먼저 보낼 데이터 (서버 큐에서 호출됨)
    var makeGreeting: (() -> Data?)?
    var onKeyframeNeeded: (() -> Void)?
    /// PC가 보낸 프레임 (kind, payload) — 서버 큐에서 호출됨
    var onFrame: ((UInt8, Data) -> Void)?

    private let queue: DispatchQueue
    private var listener: NWListener?
    private var clients: [ObjectIdentifier: Client] = [:]
    private var stopped = false
    private let lock = NSLock()
    private var readyCount = 0

    init(port: UInt16, name: String, bonjourName: String? = nil) {
        self.port = port
        self.name = name
        self.bonjourName = bonjourName
        queue = DispatchQueue(label: "padlink.server.\(name)")
    }

    var hasClients: Bool {
        lock.lock()
        defer { lock.unlock() }
        return readyCount > 0
    }

    func start() {
        queue.async {
            self.stopped = false
            self.startListener()
        }
    }

    func stop() {
        queue.async {
            self.stopped = true
            self.listener?.cancel()
            self.listener = nil
            self.clients.values.forEach { $0.connection.cancel() }
        }
    }

    func disconnectAll() {
        queue.async { self.clients.values.forEach { $0.connection.cancel() } }
    }

    /// 로그·소리처럼 순서만 지키면 되는 데이터
    func sendAll(_ data: Data) {
        queue.async {
            for client in self.clients.values where client.isReady && client.pendingBytes <= Self.maxPendingBytes {
                self.transmit(data, to: client)
            }
        }
    }

    /// 영상: 밀리면 버리고, 디코더가 깨지지 않게 다음 키프레임부터 다시 보낸다
    func sendVideo(_ data: Data, isKey: Bool) {
        queue.async {
            var needKey = false
            for client in self.clients.values where client.isReady {
                if client.pendingBytes > Self.maxPendingBytes {
                    if !client.waitingForKeyframe {
                        client.waitingForKeyframe = true
                        client.keyframeRequested = false
                    }
                    continue
                }
                if client.waitingForKeyframe && !isKey {
                    if !client.keyframeRequested {
                        client.keyframeRequested = true
                        needKey = true
                    }
                    continue
                }
                client.waitingForKeyframe = false
                client.keyframeRequested = false
                self.transmit(data, to: client)
            }
            if needKey { self.onKeyframeNeeded?() }
        }
    }

    // MARK: - 서버 큐 안에서만 호출

    private func startListener() {
        guard !stopped, listener == nil, let nwPort = NWEndpoint.Port(rawValue: port) else { return }
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let params = NWParameters(tls: nil, tcp: tcp)
        params.allowLocalEndpointReuse = true
        do {
            let l = try NWListener(using: params, on: nwPort)
            if let bonjourName {
                l.service = NWListener.Service(name: bonjourName, type: Wire.bonjourType)
            }
            l.stateUpdateHandler = { [weak self] state in
                guard let self else { return }
                switch state {
                case .ready:
                    Log.write("\(self.name): \(self.port) 포트 대기 중")
                case .failed(let error):
                    Log.write("\(self.name): 리스너 실패 \(error), 1초 뒤 재시도")
                    self.listener?.cancel()
                    self.listener = nil
                    self.queue.asyncAfter(deadline: .now() + 1) { self.startListener() }
                default:
                    break
                }
            }
            l.newConnectionHandler = { [weak self] connection in self?.accept(connection) }
            l.start(queue: queue)
            listener = l
        } catch {
            Log.write("\(name): \(port) 포트를 열 수 없음 \(error)")
        }
    }

    private func accept(_ connection: NWConnection) {
        let client = Client(connection)
        let id = ObjectIdentifier(client)
        clients[id] = client
        connection.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                if let greeting = self.makeGreeting?() { self.transmit(greeting, to: client) }
                client.isReady = true
                client.keyframeRequested = true
                self.adjustReadyCount(1)
                Log.write("\(self.name): 접속 \(connection.endpoint)")
                self.onKeyframeNeeded?()
            case .failed:
                connection.cancel()
            case .cancelled:
                if client.isReady {
                    client.isReady = false
                    self.adjustReadyCount(-1)
                    Log.write("\(self.name): 접속 종료 \(connection.endpoint)")
                }
                connection.stateUpdateHandler = nil
                self.clients.removeValue(forKey: id)
            default:
                break
            }
        }
        connection.start(queue: queue)
        receive(on: connection, client: client)
    }

    /// PC가 보내는 프레임을 읽고, 끊김도 여기서 알아챈다
    private func receive(on connection: NWConnection, client: Client) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
            if let data, !data.isEmpty, let self {
                client.inbox.append(data)
                self.parseInbox(client)
            }
            if isComplete || error != nil {
                connection.cancel()
                return
            }
            self?.receive(on: connection, client: client)
        }
    }

    private func parseInbox(_ client: Client) {
        while client.inbox.count >= Wire.headerSize {
            let header = [UInt8](client.inbox.prefix(Wire.headerSize))
            let length = Int(header[4]) | Int(header[5]) << 8 | Int(header[6]) << 16 | Int(header[7]) << 24
            // PC 화면 키프레임이 들어오므로 넉넉하게
            guard length <= 16 << 20 else {
                client.connection.cancel()
                return
            }
            guard client.inbox.count >= Wire.headerSize + length else { return }
            let start = client.inbox.startIndex
            let payload = client.inbox.subdata(in: (start + Wire.headerSize)..<(start + Wire.headerSize + length))
            client.inbox.removeSubrange(start..<(start + Wire.headerSize + length))
            onFrame?(header[0], payload)
        }
    }

    private func transmit(_ data: Data, to client: Client) {
        client.pendingBytes += data.count
        client.connection.send(content: data, completion: .contentProcessed { [weak client] error in
            guard let client else { return }
            client.pendingBytes -= data.count
            if error != nil { client.connection.cancel() }
        })
    }

    private func adjustReadyCount(_ delta: Int) {
        lock.lock()
        readyCount += delta
        lock.unlock()
    }
}
