#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// usbmuxd 응답을 읽기 위한 최소한의 XML plist 파서
struct PlistValue {
    enum class Type { None, Dict, Array, String, Integer, Bool, Real, Data, Date };
    Type type = Type::None;
    std::string text;
    int64_t integer = 0;
    bool boolean = false;
    std::vector<std::pair<std::string, PlistValue>> dict;
    std::vector<PlistValue> array;

    const PlistValue* Get(std::string_view key) const;
    std::string String(std::string_view key) const;
    int64_t Integer(std::string_view key, int64_t fallback = -1) const;
};

bool ParseXmlPlist(std::string_view xml, PlistValue& out);
