import Foundation
import os

/// 시뮬레이터·디버거가 없으니 로그는 os_log와 함께 PC로도 보낸다
enum Log {
    private static let logger = Logger(subsystem: "io.github.sungwoo0829.padlink", category: "padlink")
    /// 확장 시작 시 한 번 설정 (PC 전송용)
    static var sink: ((String) -> Void)?

    static func write(_ message: String) {
        logger.log("\(message, privacy: .public)")
        sink?(message)
    }
}
