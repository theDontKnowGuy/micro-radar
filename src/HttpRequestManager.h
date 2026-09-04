#pragma once

#include <Arduino.h>
#include <vector>

struct HttpResult {
    bool success;           // Whether the request succeeded
    int statusCode;         // HTTP status code (0 if network error)
    String response;        // Response body (empty on error)
    String errorMessage;    // Error description if success == false
};

class HttpRequestManager
{
private:
    String BuildQueryString(const std::vector<std::pair<String, String>>& params) const;

public:
    HttpRequestManager() = default;
    ~HttpRequestManager() = default;

    // Percent-encodes everything outside the unreserved set of RFC 3986. Query
    // values and form bodies are built by string concatenation here, so an
    // unencoded '&' or '=' in a caller-supplied value would not merely break
    // the request -- it would append parameters of the sender's choosing.
    static String UrlEncode(const String& value);

    [[nodiscard]] HttpResult Get(const String& url, const std::vector<std::pair<String, String>>& params = {}, const std::vector<std::pair<String, String>>& headers = {});
    [[nodiscard]] HttpResult Post(const String& url, const String& body = "", const std::vector<std::pair<String, String>>& headers = {});
};
