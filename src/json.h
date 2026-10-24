#pragma once
#include <winrt/Windows.Data.Json.h>
#include <initializer_list>
#include <cmath>
#include <stdexcept>
#include <string>
namespace distinct {
using Json = winrt::Windows::Data::Json::JsonObject;
using Value = winrt::Windows::Data::Json::JsonValue;
using JsonType = winrt::Windows::Data::Json::JsonValueType;
inline auto js(const std::string& value) { return Value::CreateStringValue(winrt::to_hstring(value)); }
inline auto js(const wchar_t* value) { return Value::CreateStringValue(value ? value : L""); }
inline auto jn(double value) { return Value::CreateNumberValue(value); }
inline auto jb(bool value) { return Value::CreateBooleanValue(value); }
inline Json object(std::initializer_list<std::pair<const wchar_t*, winrt::Windows::Data::Json::IJsonValue>> entries) {
    Json result;
    for (const auto& entry : entries) result.SetNamedValue(entry.first, entry.second);
    return result;
}
struct Error : std::runtime_error {
    std::string code;
    Error(std::string c, std::string message) : runtime_error(std::move(message)), code(std::move(c)) {}
};
inline std::string string(const Json& json, const wchar_t* field, size_t limit = 4096) {
    if (!json.HasKey(field) || json.GetNamedValue(field).ValueType() != JsonType::String)
        throw Error("invalid_request", "Missing or invalid string field: " + winrt::to_string(field));
    auto value = winrt::to_string(json.GetNamedString(field));
    if (value.empty() || value.size() > limit || value.find('\0') != std::string::npos)
        throw Error("invalid_request", "Empty, oversized or NUL-containing field: " + winrt::to_string(field));
    return value;
}
inline double number(const Json& json, const wchar_t* field, double low, double high) {
    if (!json.HasKey(field) || json.GetNamedValue(field).ValueType() != JsonType::Number)
        throw Error("invalid_request", "Missing numeric field: " + winrt::to_string(field));
    double value = json.GetNamedNumber(field);
    if (!std::isfinite(value) || value < low || value > high)
        throw Error("invalid_request", "Out of range: " + winrt::to_string(field));
    return value;
}
inline void checked(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw Error("platform_error", std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
}
}
