// SPDX-License-Identifier: MIT
#pragma once

#include "ProfileDomain.h"
#include <Windows.h>
#include <charconv>
#include <limits>
#include <map>
#include <sstream>
#include <variant>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace HidHide::Profiles
{
    namespace Json
    {
        constexpr std::size_t MaxBytes{ 4 * 1024 * 1024 };
        struct Value;
        using Array = std::vector<Value>;
        using Object = std::map<std::wstring, Value>;
        struct Value
        {
            std::variant<std::nullptr_t, bool, std::uint64_t, std::int64_t, std::wstring, Array, Object> data;
        };

        inline std::wstring FromUtf8(std::string const& text)
        {
            if (text.size() > MaxBytes) throw std::runtime_error("JSON exceeds size limit");
            if (text.empty()) return {};
            auto count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (!count) throw std::runtime_error("JSON is not valid UTF-8");
            std::wstring result(static_cast<std::size_t>(count), L'\0');
            if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count) != count)
                throw std::runtime_error("JSON UTF-8 conversion failed");
            return result;
        }

        // Serialized catalog files must stay readable: reject output over the
        // read limit before anything is written.
        inline std::string Bounded(std::string bytes)
        {
            if (bytes.size() > MaxBytes) throw std::invalid_argument("Profile data exceeds the 4 MiB file size limit");
            return bytes;
        }

        inline std::string ToUtf8(std::wstring const& text)
        {
            if (text.empty()) return {};
            auto count = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (!count) throw std::runtime_error("Text contains invalid Unicode");
            std::string result(static_cast<std::size_t>(count), '\0');
            if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr) != count)
                throw std::runtime_error("UTF-8 conversion failed");
            return result;
        }

        class Parser
        {
        public:
            explicit Parser(std::wstring text) : m_Text(std::move(text)) {}
            Value Parse()
            {
                Skip(); auto value = ReadValue(0); Skip();
                if (m_Offset != m_Text.size()) Fail("Trailing JSON content");
                return value;
            }
        private:
            [[noreturn]] void Fail(char const* message) const { throw std::runtime_error(message); }
            void Skip() { while (m_Offset < m_Text.size() && (m_Text[m_Offset] == L' ' || m_Text[m_Offset] == L'\t' || m_Text[m_Offset] == L'\r' || m_Text[m_Offset] == L'\n')) ++m_Offset; }
            wchar_t Take() { if (m_Offset >= m_Text.size()) Fail("Truncated JSON"); return m_Text[m_Offset++]; }
            bool Consume(std::wstring const& token)
            {
                if (m_Text.compare(m_Offset, token.size(), token) != 0) return false;
                m_Offset += token.size(); return true;
            }
            Value ReadValue(unsigned depth)
            {
                if (depth > 64) Fail("JSON nesting exceeds limit");
                Skip();
                if (m_Offset >= m_Text.size()) Fail("Truncated JSON");
                if (m_Text[m_Offset] == L'{') return Value{ ReadObject(depth + 1) };
                if (m_Text[m_Offset] == L'[') return Value{ ReadArray(depth + 1) };
                if (m_Text[m_Offset] == L'"') return Value{ ReadString() };
                if (Consume(L"true")) return Value{ true };
                if (Consume(L"false")) return Value{ false };
                if (Consume(L"null")) return Value{ nullptr };
                return ReadNumber();
            }
            static int Hex(wchar_t c)
            {
                if (c >= L'0' && c <= L'9') return c - L'0';
                if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                return -1;
            }
            std::uint16_t ReadHex()
            {
                std::uint16_t value{};
                for (int i{}; i < 4; ++i) { auto h = Hex(Take()); if (h < 0) Fail("Invalid Unicode escape"); value = static_cast<std::uint16_t>((value << 4) | h); }
                return value;
            }
            std::wstring ReadString()
            {
                if (Take() != L'"') Fail("Expected string");
                std::wstring result;
                while (true)
                {
                    auto c = Take();
                    if (c == L'"') break;
                    if (c < 0x20) Fail("Unescaped control character");
                    if (c != L'\\') { result.push_back(c); }
                    else
                    {
                        switch (Take())
                        {
                        case L'"': result.push_back(L'"'); break; case L'\\': result.push_back(L'\\'); break;
                        case L'/': result.push_back(L'/'); break; case L'b': result.push_back(L'\b'); break;
                        case L'f': result.push_back(L'\f'); break; case L'n': result.push_back(L'\n'); break;
                        case L'r': result.push_back(L'\r'); break; case L't': result.push_back(L'\t'); break;
                        case L'u':
                        {
                            auto first = ReadHex();
                            if (first >= 0xd800 && first <= 0xdbff)
                            {
                                if (Take() != L'\\' || Take() != L'u') Fail("Invalid surrogate pair");
                                auto second = ReadHex(); if (second < 0xdc00 || second > 0xdfff) Fail("Invalid surrogate pair");
                                result.push_back(static_cast<wchar_t>(first)); result.push_back(static_cast<wchar_t>(second));
                            }
                            else if (first >= 0xdc00 && first <= 0xdfff) Fail("Unexpected low surrogate");
                            else if (first == 0) Fail("Embedded NUL is not allowed");
                            else result.push_back(static_cast<wchar_t>(first));
                            break;
                        }
                        default: Fail("Invalid string escape");
                        }
                    }
                    if (result.size() > MaxTextCharacters) Fail("JSON string exceeds limit");
                }
                return result;
            }
            Value ReadNumber()
            {
                auto start = m_Offset; bool negative = m_Text[m_Offset] == L'-'; if (negative) ++m_Offset;
                if (m_Offset >= m_Text.size() || m_Text[m_Offset] < L'0' || m_Text[m_Offset] > L'9') Fail("Invalid number");
                // JSON digits are ASCII only; iswdigit also accepts fullwidth and other Unicode digits.
                auto asciiDigit = [&](std::size_t offset) { return offset < m_Text.size() && m_Text[offset] >= L'0' && m_Text[offset] <= L'9'; };
                if (m_Text[m_Offset] == L'0' && asciiDigit(m_Offset + 1)) Fail("Leading zero in number");
                while (asciiDigit(m_Offset)) ++m_Offset;
                if (m_Offset < m_Text.size() && std::iswdigit(m_Text[m_Offset])) Fail("Invalid number");
                if (m_Offset < m_Text.size() && (m_Text[m_Offset] == L'.' || m_Text[m_Offset] == L'e' || m_Text[m_Offset] == L'E')) Fail("Floating point is not allowed");
                auto digits = m_Text.substr(start + (negative ? 1 : 0), m_Offset - start - (negative ? 1 : 0));
                std::uint64_t magnitude{};
                for (auto c : digits)
                {
                    auto digit = static_cast<unsigned>(c - L'0');
                    if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) Fail("Number out of range");
                    magnitude = magnitude * 10 + digit;
                }
                if (!negative) return Value{ magnitude };
                if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1) Fail("Number out of range");
                if (magnitude == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1)
                    return Value{ std::numeric_limits<std::int64_t>::min() };
                return Value{ -static_cast<std::int64_t>(magnitude) };
            }
            Object ReadObject(unsigned depth)
            {
                Object result; Take(); Skip(); if (m_Offset < m_Text.size() && m_Text[m_Offset] == L'}') { ++m_Offset; return result; }
                while (true)
                {
                    Skip(); if (m_Offset >= m_Text.size() || m_Text[m_Offset] != L'"') Fail("Expected object key");
                    auto key = ReadString(); Skip(); if (Take() != L':') Fail("Expected colon");
                    auto value = ReadValue(depth); if (!result.emplace(std::move(key), std::move(value)).second) Fail("Duplicate object key");
                    if (result.size() > MaxRulesPerProfile + 32) Fail("Object exceeds member limit");
                    Skip(); auto c = Take(); if (c == L'}') break; if (c != L',') Fail("Expected comma");
                }
                return result;
            }
            Array ReadArray(unsigned depth)
            {
                Array result; Take(); Skip(); if (m_Offset < m_Text.size() && m_Text[m_Offset] == L']') { ++m_Offset; return result; }
                while (true)
                {
                    result.emplace_back(ReadValue(depth)); if (result.size() > MaxRulesPerProfile) Fail("Array exceeds item limit");
                    Skip(); auto c = Take(); if (c == L']') break; if (c != L',') Fail("Expected comma");
                }
                return result;
            }
            std::wstring m_Text; std::size_t m_Offset{};
        };

        inline Object const& AsObject(Value const& v) { auto p = std::get_if<Object>(&v.data); if (!p) throw std::runtime_error("Expected object"); return *p; }
        inline Array const& AsArray(Value const& v) { auto p = std::get_if<Array>(&v.data); if (!p) throw std::runtime_error("Expected array"); return *p; }
        inline std::wstring const& AsString(Value const& v) { auto p = std::get_if<std::wstring>(&v.data); if (!p) throw std::runtime_error("Expected string"); return *p; }
        inline bool AsBool(Value const& v) { auto p = std::get_if<bool>(&v.data); if (!p) throw std::runtime_error("Expected boolean"); return *p; }
        inline std::uint64_t AsUnsigned(Value const& v) { auto p = std::get_if<std::uint64_t>(&v.data); if (!p) throw std::runtime_error("Expected unsigned integer"); return *p; }
        inline std::int64_t AsSigned(Value const& v)
        {
            if (auto p = std::get_if<std::int64_t>(&v.data)) return *p;
            if (auto p = std::get_if<std::uint64_t>(&v.data); p && *p <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return static_cast<std::int64_t>(*p);
            throw std::runtime_error("Expected signed integer");
        }
        inline Value const& Required(Object const& object, wchar_t const* name)
        {
            auto it = object.find(name); if (it == object.end()) throw std::runtime_error("Required JSON member is missing"); return it->second;
        }
        inline void ExactMembers(Object const& object, std::initializer_list<wchar_t const*> names)
        {
            std::set<std::wstring> expected(names.begin(), names.end());
            if (object.size() != expected.size()) throw std::runtime_error("Unknown or missing JSON member");
            for (auto const& [name, value] : object) { (void)value; if (!expected.count(name)) throw std::runtime_error("Unknown JSON member"); }
        }
        inline std::wstring Escape(std::wstring const& text)
        {
            std::wostringstream out; out << L'"';
            for (auto c : text)
            {
                switch (c)
                {
                case L'"': out << L"\\\""; break; case L'\\': out << L"\\\\"; break;
                case L'\b': out << L"\\b"; break; case L'\f': out << L"\\f"; break;
                case L'\n': out << L"\\n"; break; case L'\r': out << L"\\r"; break; case L'\t': out << L"\\t"; break;
                default:
                    if (c < 0x20) { wchar_t buffer[7]{}; swprintf_s(buffer, L"\\u%04x", static_cast<unsigned>(c)); out << buffer; }
                    else out << c;
                }
            }
            out << L'"'; return out.str();
        }
    }

    inline std::string SerializeProfile(Profile const& profile)
    {
        Validate(profile);
        std::wostringstream out;
        out << L"{\n  \"schemaVersion\": 1,\n  \"id\": " << Json::Escape(profile.id)
            << L",\n  \"revision\": " << profile.revision << L",\n  \"name\": " << Json::Escape(profile.name)
            << L",\n  \"kind\": " << Json::Escape(profile.kind == Kind::Application ? L"application" : L"global")
            << L",\n  \"enabled\": " << (profile.enabled ? L"true" : L"false")
            << L",\n  \"priority\": " << profile.priority << L",\n  \"executablePath\": " << Json::Escape(profile.kind == Kind::Application ? NormalizeExecutable(profile.executable).native() : L"")
            << L",\n  \"defaultVisibility\": \"visible\",\n  \"deviceRules\": [";
        for (std::size_t i{}; i < profile.rules.size(); ++i)
        {
            auto const& rule = profile.rules[i];
            out << (i ? L"," : L"") << L"\n    {\"identity\": " << Json::Escape(rule.identity)
                << L", \"friendlyName\": " << Json::Escape(rule.friendlyName)
                << L", \"visibility\": " << Json::Escape(rule.visibility == Visibility::Hidden ? L"hidden" : L"visible") << L"}";
        }
        out << (profile.rules.empty() ? L"]\n}" : L"\n  ]\n}") << L"\n";
        return Json::Bounded(Json::ToUtf8(out.str()));
    }

    inline Profile ParseProfile(std::string const& bytes)
    {
        auto root = Json::Parser(Json::FromUtf8(bytes)).Parse(); auto const& object = Json::AsObject(root);
        Json::ExactMembers(object, { L"schemaVersion", L"id", L"revision", L"name", L"kind", L"enabled", L"priority", L"executablePath", L"defaultVisibility", L"deviceRules" });
        if (Json::AsUnsigned(Json::Required(object, L"schemaVersion")) != SchemaVersion) throw std::runtime_error("Unsupported profile schema version");
        Profile profile; profile.id = Json::AsString(Json::Required(object, L"id")); profile.revision = Json::AsUnsigned(Json::Required(object, L"revision"));
        profile.name = Json::AsString(Json::Required(object, L"name")); auto kind = Json::AsString(Json::Required(object, L"kind"));
        if (kind == L"application") profile.kind = Kind::Application; else if (kind == L"global") profile.kind = Kind::Global; else throw std::runtime_error("Unknown profile kind");
        profile.enabled = Json::AsBool(Json::Required(object, L"enabled")); auto priority = Json::AsSigned(Json::Required(object, L"priority"));
        if (priority < std::numeric_limits<std::int32_t>::min() || priority > std::numeric_limits<std::int32_t>::max()) throw std::runtime_error("Profile priority out of range");
        profile.priority = static_cast<std::int32_t>(priority); profile.executable = Json::AsString(Json::Required(object, L"executablePath"));
        if (Json::AsString(Json::Required(object, L"defaultVisibility")) != L"visible") throw std::runtime_error("Only visible default is supported");
        for (auto const& item : Json::AsArray(Json::Required(object, L"deviceRules")))
        {
            auto const& ruleObject = Json::AsObject(item); Json::ExactMembers(ruleObject, { L"identity", L"friendlyName", L"visibility" });
            DeviceRule rule; rule.identity = Json::AsString(Json::Required(ruleObject, L"identity")); rule.friendlyName = Json::AsString(Json::Required(ruleObject, L"friendlyName"));
            auto visibility = Json::AsString(Json::Required(ruleObject, L"visibility"));
            if (visibility == L"hidden") rule.visibility = Visibility::Hidden; else if (visibility == L"visible") rule.visibility = Visibility::Visible; else throw std::runtime_error("Unknown device visibility");
            profile.rules.emplace_back(std::move(rule));
        }
        if (profile.kind == Kind::Application) profile.executable = NormalizeExecutable(profile.executable);
        Validate(profile); return profile;
    }

    inline std::string SerializeSettings(Settings const& settings)
    {
        if (!settings.revision || !IsStableId(settings.selectedGlobalId)) throw std::invalid_argument("Settings are invalid");
        std::wostringstream out;
        out << L"{\n  \"schemaVersion\": 1,\n  \"revision\": " << settings.revision
            << L",\n  \"selectedGlobalId\": " << Json::Escape(settings.selectedGlobalId)
            << L",\n  \"mode\": " << Json::Escape(settings.mode == Mode::Automatic ? L"automatic" : L"useGlobal")
            << L",\n  \"paused\": " << (settings.paused ? L"true" : L"false")
            << L",\n  \"startWithWindows\": " << (settings.startWithWindows ? L"true" : L"false")
            << L",\n  \"allowedApplications\": [";
        // Entries differing only in case are one application; ParseSettings rejects duplicates.
        std::set<std::wstring> applications;
        for (auto const& application : settings.allowedApplications) applications.insert(NormalizeExecutable(application).native());
        std::size_t index{};
        for (auto const& application : applications)
            out << (index++ ? L", " : L"") << Json::Escape(application);
        out << L"]\n}\n";
        return Json::Bounded(Json::ToUtf8(out.str()));
    }

    inline Settings ParseSettings(std::string const& bytes)
    {
        auto root = Json::Parser(Json::FromUtf8(bytes)).Parse(); auto const& object = Json::AsObject(root);
        Json::ExactMembers(object, { L"schemaVersion", L"revision", L"selectedGlobalId", L"mode", L"paused", L"startWithWindows", L"allowedApplications" });
        if (Json::AsUnsigned(Json::Required(object, L"schemaVersion")) != SchemaVersion) throw std::runtime_error("Unsupported settings schema version");
        Settings settings; settings.revision = Json::AsUnsigned(Json::Required(object, L"revision")); settings.selectedGlobalId = Json::AsString(Json::Required(object, L"selectedGlobalId"));
        auto mode = Json::AsString(Json::Required(object, L"mode")); if (mode == L"automatic") settings.mode = Mode::Automatic; else if (mode == L"useGlobal") settings.mode = Mode::UseGlobal; else throw std::runtime_error("Unknown profile mode");
        settings.paused = Json::AsBool(Json::Required(object, L"paused")); settings.startWithWindows = Json::AsBool(Json::Required(object, L"startWithWindows"));
        for (auto const& item : Json::AsArray(Json::Required(object, L"allowedApplications")))
            if (!settings.allowedApplications.emplace(NormalizeExecutable(Json::AsString(item))).second) throw std::runtime_error("Duplicate allowed application");
        if (!settings.revision || !IsStableId(settings.selectedGlobalId)) throw std::runtime_error("Settings are invalid"); return settings;
    }
}
