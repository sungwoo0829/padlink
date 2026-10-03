import UIKit

/// 펜 모드: Apple Pencil 입력을 PC로 보낸다. PC 모니터 비율에 맞춘 활성 영역만 쓰고,
/// 손가락 터치는 무시한다(손바닥 인식 차단). PC가 화면을 보내 주면 그 영역에 띄워서
/// 액정타블렛이 되고, 안 보내면 PC 모니터를 보면서 쓰는 판타블렛이다.
final class PenViewController: UIViewController, UIPencilInteractionDelegate {
    private let server = StreamServer(port: Wire.penPort, name: "pen", bonjourName: "PadLink Pen")
    private let canvas = PenCanvasView()
    private let video = VideoReceiver()
    private var wasConnected = false
    private let statusLabel = UILabel()
    private let closeButton = UIButton(type: .system)
    /// 화면 위에 겹치는 메뉴. 화면을 받는 중이면 잠시 뒤 숨고, 두 손가락 탭으로 다시 뜬다.
    private let overlay = UIView()
    private var overlayVisible = true
    private var overlayShownAt: TimeInterval = 0
    private var wasReceiving = false
    private var statusTimer: Timer?
    private var targetName = ""

    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }
    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge { .all }

    override func loadView() {
        view = canvas
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        canvas.onSamples = { [weak self] samples in self?.send(samples) }
        canvas.attachVideo(video.layer)
        video.onNeedKeyframe = { [server] in
            server.sendAll(Wire.frame(.keyframeRequest, pts: 0, payload: Data()))
        }

        let pencil = UIPencilInteraction()
        pencil.delegate = self
        canvas.addInteraction(pencil)

        // 메뉴 배경을 건드린 펜 입력은 응답자 체인을 따라 캔버스로 그대로 간다
        overlay.backgroundColor = UIColor(white: 0, alpha: 0.6)
        overlay.layer.cornerRadius = 10
        overlay.translatesAutoresizingMaskIntoConstraints = false
        canvas.addSubview(overlay)

        closeButton.setTitle("닫기", for: .normal)
        closeButton.titleLabel?.font = .systemFont(ofSize: 17, weight: .semibold)
        closeButton.addTarget(self, action: #selector(close), for: .touchUpInside)
        closeButton.translatesAutoresizingMaskIntoConstraints = false
        overlay.addSubview(closeButton)

        statusLabel.textColor = UIColor(white: 0.75, alpha: 1)
        statusLabel.font = .systemFont(ofSize: 13)
        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        overlay.addSubview(statusLabel)

        NSLayoutConstraint.activate([
            overlay.topAnchor.constraint(equalTo: canvas.safeAreaLayoutGuide.topAnchor, constant: 8),
            overlay.leadingAnchor.constraint(equalTo: canvas.safeAreaLayoutGuide.leadingAnchor, constant: 8),
            overlay.trailingAnchor.constraint(lessThanOrEqualTo: canvas.trailingAnchor, constant: -8),
            closeButton.topAnchor.constraint(equalTo: overlay.topAnchor, constant: 2),
            closeButton.bottomAnchor.constraint(equalTo: overlay.bottomAnchor, constant: -2),
            closeButton.leadingAnchor.constraint(equalTo: overlay.leadingAnchor, constant: 14),
            statusLabel.centerYAnchor.constraint(equalTo: closeButton.centerYAnchor),
            statusLabel.leadingAnchor.constraint(equalTo: closeButton.trailingAnchor, constant: 16),
            statusLabel.trailingAnchor.constraint(equalTo: overlay.trailingAnchor, constant: -14),
        ])

        // 손바닥에 실수로 뜨지 않게 두 손가락 탭만 받는다 (Pencil은 무시)
        let tap = UITapGestureRecognizer(target: self, action: #selector(showOverlay))
        tap.numberOfTouchesRequired = 2
        tap.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
        tap.cancelsTouchesInView = false
        canvas.addGestureRecognizer(tap)

        server.makeGreeting = { Wire.frame(.hello, pts: 0, payload: Wire.helloPayload(role: "pen")) }
        server.onFrame = { [weak self, video] kind, flags, payload in
            switch kind {
            case Wire.Kind.videoFrame.rawValue:
                video.submit(payload, hevc: flags & 2 != 0)
            case Wire.Kind.penConfig.rawValue:
                DispatchQueue.main.async { self?.applyConfig(payload) }
            case Wire.Kind.control.rawValue:
                // 송출(방송)이 시작되면 펜 모드를 닫는다 — 둘은 하나만 켠다
                if LocalControl.parse(payload)?["cmd"] as? String == "close" {
                    DispatchQueue.main.async {
                        Log.write("송출이 시작돼서 펜 모드를 닫음")
                        self?.close()
                    }
                }
            default:
                break
            }
        }
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        UIApplication.shared.isIdleTimerDisabled = true
        Log.sink = { [server] line in
            server.sendAll(Wire.frame(.log, pts: 0, payload: Data(line.utf8)))
        }
        server.start()
        // 송출(방송) 중이면 멈춘다 — 펜 모드와 송출은 하나만 켠다
        LocalControl.send(["cmd": "stop"], toPort: Wire.port)
        statusTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.updateStatus()
        }
        updateStatus()
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        statusTimer?.invalidate()
        statusTimer = nil
        server.stop()
        video.clear()
        Log.sink = nil
        UIApplication.shared.isIdleTimerDisabled = false
    }

    @objc private func close() {
        dismiss(animated: true)
    }

    @objc private func showOverlay() {
        overlayShownAt = ProcessInfo.processInfo.systemUptime
        setOverlay(visible: true)
    }

    private func setOverlay(visible: Bool) {
        guard visible != overlayVisible else { return }
        overlayVisible = visible
        if visible {
            overlay.isHidden = false
            UIView.animate(withDuration: 0.2) { self.overlay.alpha = 1 }
        } else {
            UIView.animate(withDuration: 0.3, animations: { self.overlay.alpha = 0 }) { _ in
                if !self.overlayVisible { self.overlay.isHidden = true }
            }
        }
    }

    private func updateStatus() {
        let connected = server.hasClients
        if wasConnected && !connected { video.clear() }
        wasConnected = connected

        // 화면을 받는 동안만 메뉴를 숨긴다. 받기 시작하면 3초는 보여 준다.
        let receiving = video.isReceiving
        let now = ProcessInfo.processInfo.systemUptime
        if receiving && !wasReceiving { overlayShownAt = now }
        wasReceiving = receiving
        if !receiving {
            setOverlay(visible: true)
        } else if overlayVisible && now - overlayShownAt > 3 {
            setOverlay(visible: false)
        }

        if connected {
            statusLabel.text = receiving
                ? "PC 연결됨 · \(targetName) · 액정타블렛 · 두 손가락 탭: 메뉴"
                : "PC 연결됨 · \(targetName) — PC 화면을 보면서 그리세요 (PC에서 화면 보내기를 켜면 액정타블렛)"
        } else {
            let ip = NetInfo.ipv4Addresses().first.map { " · Wi-Fi \($0)" } ?? ""
            statusLabel.text = "PC 연결 대기 중 (USB\(ip), 포트 \(Wire.penPort))"
        }
    }

    private func applyConfig(_ payload: Data) {
        guard let info = try? JSONSerialization.jsonObject(with: payload) as? [String: Any],
              let w = info["width"] as? Double, let h = info["height"] as? Double, w > 0, h > 0 else { return }
        canvas.targetAspect = CGFloat(w / h)
        targetName = "\(Int(w))×\(Int(h))"
        Log.write("펜 영역: PC 화면 \(targetName)")
        updateStatus()
    }

    private func send(_ samples: [Wire.PenSample]) {
        let pts = UInt64(ProcessInfo.processInfo.systemUptime * 1_000_000)
        server.sendAll(Wire.frame(.penSamples, pts: pts, payload: Wire.penPayload(samples)))
    }

    private func sendButton(_ button: Wire.PenButton, _ phase: Wire.ButtonPhase) {
        let pts = UInt64(ProcessInfo.processInfo.systemUptime * 1_000_000)
        server.sendAll(Wire.frame(.penButton, pts: pts, payload: Wire.buttonPayload(button, phase)))
    }

    // MARK: - UIPencilInteractionDelegate (Pencil Pro 스퀴즈, 더블탭)

    func pencilInteraction(_ interaction: UIPencilInteraction, didReceiveTap tap: UIPencilInteraction.Tap) {
        sendButton(.doubleTap, .tap)
    }

    func pencilInteraction(_ interaction: UIPencilInteraction, didReceiveSqueeze squeeze: UIPencilInteraction.Squeeze) {
        switch squeeze.phase {
        case .began:
            sendButton(.squeeze, .began)
        case .ended, .cancelled:
            sendButton(.squeeze, .ended)
        default:
            break
        }
    }
}

/// 펜 입력을 받는 화면. 240Hz 묶음 샘플(coalesced touches)을 모두 보낸다.
final class PenCanvasView: UIView {
    var onSamples: (([Wire.PenSample]) -> Void)?
    var targetAspect: CGFloat = 16.0 / 9.0 {
        didSet { setNeedsLayout() }
    }

    private var activeArea = CGRect.zero
    private var videoLayer: CALayer?
    private let areaLayer = CAShapeLayer()
    private let cursorLayer = CAShapeLayer()

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = UIColor(white: 0.06, alpha: 1)
        isMultipleTouchEnabled = true

        areaLayer.fillColor = UIColor(white: 0.12, alpha: 1).cgColor
        areaLayer.strokeColor = UIColor(white: 0.32, alpha: 1).cgColor
        areaLayer.lineWidth = 1
        layer.addSublayer(areaLayer)

        cursorLayer.fillColor = UIColor.clear.cgColor
        cursorLayer.strokeColor = UIColor.systemTeal.cgColor
        cursorLayer.lineWidth = 1.5
        layer.addSublayer(cursorLayer)

        addGestureRecognizer(UIHoverGestureRecognizer(target: self, action: #selector(hovered(_:))))
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    /// PC 화면을 펜 영역에 겹쳐 띄운다 (펜 좌표와 1:1)
    func attachVideo(_ video: CALayer) {
        videoLayer = video
        layer.insertSublayer(video, above: areaLayer)
        setNeedsLayout()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        // 화면 끝까지 쓴다. PC 화면 비율이 iPad와 같으면(2360x1640) 꽉 차고, 다르면 남는 쪽이 검게 남는다.
        let room = bounds
        var w = room.width
        var h = w / targetAspect
        if h > room.height {
            h = room.height
            w = h * targetAspect
        }
        activeArea = CGRect(x: room.midX - w / 2, y: room.midY - h / 2, width: w, height: h)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        areaLayer.path = UIBezierPath(rect: activeArea).cgPath
        videoLayer?.frame = activeArea
        CATransaction.commit()
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) { handle(touches, event, .down) }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) { handle(touches, event, .move) }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { handle(touches, event, .up) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { handle(touches, event, .cancel) }

    private func handle(_ touches: Set<UITouch>, _ event: UIEvent?, _ phase: Wire.PenPhase) {
        guard let touch = touches.first(where: { $0.type == .pencil }) else { return }
        let all = event?.coalescedTouches(for: touch) ?? [touch]
        var samples: [Wire.PenSample] = []
        samples.reserveCapacity(all.count)
        for (i, t) in all.enumerated() {
            let isLast = i == all.count - 1
            let p: Wire.PenPhase
            switch phase {
            case .down: p = i == 0 ? .down : .move
            case .up, .cancel: p = isLast ? phase : .move
            default: p = phase
            }
            samples.append(sample(t, phase: p))
        }
        onSamples?(samples)
        if phase == .up || phase == .cancel {
            showCursor(at: touch.location(in: self), contact: false)
        } else {
            showCursor(at: touch.location(in: self), contact: true)
        }
    }

    private func sample(_ t: UITouch, phase: Wire.PenPhase) -> Wire.PenSample {
        let (x, y) = normalize(t.location(in: self))
        let force = t.maximumPossibleForce > 0 ? t.force / t.maximumPossibleForce : 0
        return Wire.PenSample(
            phase: phase, x: x, y: y,
            pressure: phase == .up || phase == .cancel ? 0 : Float(min(1, max(0, force))),
            altitude: Float(t.altitudeAngle),
            azimuth: Float(t.azimuthAngle(in: self)),
            roll: Float(t.rollAngle),
            z: 0)
    }

    @objc private func hovered(_ recognizer: UIHoverGestureRecognizer) {
        let phase: Wire.PenPhase
        switch recognizer.state {
        case .began, .changed: phase = .hover
        case .ended, .cancelled, .failed: phase = .hoverExit
        default: return
        }
        let location = recognizer.location(in: self)
        let (x, y) = normalize(location)
        onSamples?([Wire.PenSample(
            phase: phase, x: x, y: y, pressure: 0,
            altitude: Float(recognizer.altitudeAngle),
            azimuth: Float(recognizer.azimuthAngle(in: self)),
            roll: Float(recognizer.rollAngle),
            z: Float(recognizer.zOffset))])
        if phase == .hover {
            showCursor(at: location, contact: false)
        } else {
            hideCursor()
        }
    }

    private func normalize(_ p: CGPoint) -> (Float, Float) {
        guard activeArea.width > 0, activeArea.height > 0 else { return (0, 0) }
        return (Float((p.x - activeArea.minX) / activeArea.width), Float((p.y - activeArea.minY) / activeArea.height))
    }

    private func showCursor(at p: CGPoint, contact: Bool) {
        let r: CGFloat = contact ? 3 : 7
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        cursorLayer.path = UIBezierPath(ovalIn: CGRect(x: p.x - r, y: p.y - r, width: r * 2, height: r * 2)).cgPath
        cursorLayer.fillColor = contact ? UIColor.systemTeal.cgColor : UIColor.clear.cgColor
        CATransaction.commit()
    }

    private func hideCursor() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        cursorLayer.path = nil
        CATransaction.commit()
    }
}
