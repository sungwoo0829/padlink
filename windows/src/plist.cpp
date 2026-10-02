#include "plist.h"

#include <cstdlib>

const PlistValue* PlistValue::Get(std::string_view key) const {
    for (const auto& [k, v] : dict)
        if (k == key) return &v;
    return nullptr;
}

std::string PlistValue::String(std::string_view key) const {
    const PlistValue* v = Get(key);
    return v && v->type == Type::String ? v->text : std::string();
}

int64_t PlistValue::Integer(std::string_view key, int64_t fallback) const {
    const PlistValue* v = Get(key);
    return v && v->type == Type::Integer ? v->integer : fallback;
}

namespace {
std::string Unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string_view::npos) {
            out += s[i];
            continue;
        }
        std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else out.append(s.substr(i, semi - i + 1));
        i = semi;
    }
    return out;
}

struct Parser {
    std::string_view s;
    size_t i = 0;

    // 다음 태그를 읽는다. 주석·<?xml?>·<!DOCTYPE>은 건너뛴다.
    bool NextTag(std::string& name, bool& closing, bool& selfClosing) {
        for (;;) {
            size_t lt = s.find('<', i);
            if (lt == std::string_view::npos) return false;
            if (s.compare(lt, 4, "<!--") == 0) {
                size_t end = s.find("-->", lt);
                if (end == std::string_view::npos) return false;
                i = end + 3;
                continue;
            }
            if (s.compare(lt, 2, "<?") == 0 || s.compare(lt, 2, "<!") == 0) {
                size_t end = s.find('>', lt);
                if (end == std::string_view::npos) return false;
                i = end + 1;
                continue;
            }
            size_t gt = s.find('>', lt);
            if (gt == std::string_view::npos) return false;
            std::string_view body = s.substr(lt + 1, gt - lt - 1);
            closing = !body.empty() && body.front() == '/';
            if (closing) body.remove_prefix(1);
            selfClosing = !body.empty() && body.back() == '/';
            if (selfClosing) body.remove_suffix(1);
            size_t space = body.find_first_of(" \t\r\n");
            name = std::string(body.substr(0, space));
            i = gt + 1;
            return true;
        }
    }

    bool ReadText(const std::string& tag, std::string& out) {
        std::string close = "</" + tag + ">";
        size_t end = s.find(close, i);
        if (end == std::string_view::npos) return false;
        out = Unescape(s.substr(i, end - i));
        i = end + close.size();
        return true;
    }

    bool ParseValue(const std::string& name, bool selfClosing, PlistValue& v) {
        using T = PlistValue::Type;
        if (name == "dict") {
            v.type = T::Dict;
            if (selfClosing) return true;
            for (;;) {
                std::string tag;
                bool closing = false, sc = false;
                if (!NextTag(tag, closing, sc)) return false;
                if (closing && tag == "dict") return true;
                if (closing || tag != "key") return false;
                std::string key;
                if (!sc && !ReadText("key", key)) return false;
                std::string valueTag;
                if (!NextTag(valueTag, closing, sc) || closing) return false;
                PlistValue child;
                if (!ParseValue(valueTag, sc, child)) return false;
                v.dict.emplace_back(std::move(key), std::move(child));
            }
        }
        if (name == "array") {
            v.type = T::Array;
            if (selfClosing) return true;
            for (;;) {
                std::string tag;
                bool closing = false, sc = false;
                if (!NextTag(tag, closing, sc)) return false;
                if (closing && tag == "array") return true;
                if (closing) return false;
                PlistValue child;
                if (!ParseValue(tag, sc, child)) return false;
                v.array.push_back(std::move(child));
            }
        }
        if (name == "true" || name == "false") {
            v.type = T::Bool;
            v.boolean = name == "true";
            std::string ignored;
            return selfClosing || ReadText(name, ignored);
        }
        std::string text;
        if (!selfClosing && !ReadText(name, text)) return false;
        if (name == "string") {
            v.type = T::String;
        } else if (name == "integer") {
            v.type = T::Integer;
            v.integer = std::strtoll(text.c_str(), nullptr, 10);
        } else if (name == "real") {
            v.type = T::Real;
        } else if (name == "data") {
            v.type = T::Data;
        } else if (name == "date") {
            v.type = T::Date;
        }
        v.text = std::move(text);
        return true;
    }
};
}  // namespace

bool ParseXmlPlist(std::string_view xml, PlistValue& out) {
    Parser p{xml};
    std::string tag;
    bool closing = false, selfClosing = false;
    if (!p.NextTag(tag, closing, selfClosing)) return false;
    if (tag == "plist" && !p.NextTag(tag, closing, selfClosing)) return false;
    if (closing) return false;
    return p.ParseValue(tag, selfClosing, out);
}
