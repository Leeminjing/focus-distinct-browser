#include "../src/window/address_input.h"
#include <cstdlib>
#include <iostream>

int main() {
    struct Case { const wchar_t* input; const wchar_t* expected; };
    const Case cases[] = {
        {L"example.com:8080", L"https://example.com:8080"},
        {L"Example.COM:8443/path?q=a:b#section", L"https://Example.COM:8443/path?q=a:b#section"},
        {L"localhost:8080", L"http://localhost:8080"},
        {L"LOCALHOST:8080/path", L"http://LOCALHOST:8080/path"},
        {L"127.0.0.1:8080", L"http://127.0.0.1:8080"},
        {L"3example.com:8080", L"https://3example.com:8080"},
        {L"example.com", L"https://example.com"},
        {L"example.com/path:part", L"https://example.com/path:part"},
        {L"example.com?q=a:b", L"https://example.com?q=a:b"},
        {L"example.com#part:one", L"https://example.com#part:one"},
        {L"https://example.com:8080", L"https://example.com:8080"},
        {L"HTTP://localhost:8080", L"HTTP://localhost:8080"},
        {L"about:blank", L"about:blank"},
        {L"file:///C:/test.html", L"file:///C:/test.html"},
        {L"", nullptr},
        {L"example.com:abc", nullptr},
        {L"example.com:", nullptr},
        {L"javascript:alert(1)", nullptr},
        {L"javascript:123", nullptr},
        {L"data:text/html,test", nullptr},
        {L"custom://example.com", nullptr},
    };
    for (const auto& test : cases) {
        const auto result = distinct::address_url(test.input);
        if (test.expected ? !result || *result != test.expected : result.has_value()) {
            std::wcerr << L"Unexpected address completion: " << test.input << L'\n';
            return EXIT_FAILURE;
        }
    }
    std::cout << "address input tests passed (21 cases)\n";
}
