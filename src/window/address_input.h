#pragma once
#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>

namespace distinct {
// Complete a trimmed address-bar value; WebView2 remains the URL parser.
// A colon in the path/query is not a scheme, and a numeric authority port
// on a domain/localhost is not a scheme either.
inline std::optional<std::wstring> address_url(const std::wstring& input) {
    if (input.empty()) return std::nullopt;
    const auto authority_end = input.find_first_of(L"/?#");
    const auto colon = input.find(L':');
    if (colon == std::wstring::npos || colon > authority_end)
        return L"https://" + input;

    auto host = input.substr(0, colon);
    std::transform(host.begin(), host.end(), host.begin(), towlower);
    if (host == L"http" || host == L"https" || host == L"about" || host == L"file")
        return input;

    const auto port = input.substr(colon + 1, authority_end == std::wstring::npos
        ? std::wstring::npos : authority_end - colon - 1);
    const bool numeric_port = !port.empty() && port.find_first_not_of(L"0123456789") == std::wstring::npos;
    if (!numeric_port || (host != L"localhost" && host.find(L'.') == std::wstring::npos))
        return std::nullopt;
    const bool local_address = host == L"localhost" ||
        host.find_first_not_of(L"0123456789.") == std::wstring::npos;
    return (local_address ? L"http://" : L"https://") + input;
}
}
