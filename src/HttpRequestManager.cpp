#include "HttpRequestManager.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "NetworkTls.h"

namespace {

// HTTPClient's own default -- both for the initial TCP connect and for the
// read loop that follows it -- is 5000ms (HTTPCLIENT_DEFAULT_TCP_TIMEOUT).
// That is tight for a TLS handshake on this hardware even for a plain GET
// (see the PSRAM note in platformio.ini) and too tight for the OpenSky auth
// POST, which is a full handshake plus an OAuth2 exchange with Keycloak on
// the other end -- exactly the request that was timing out with HTTP error
// -11 (HTTPC_ERROR_READ_TIMEOUT) before this existed. Matches the timeout
// FirmwareUpdater already uses for the same reason.
constexpr uint32_t HTTP_TIMEOUT_MS = 15000;

}  // namespace

String HttpRequestManager::UrlEncode(const String& value)
{
    String encoded;
    encoded.reserve(value.length());

    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
            continue;
        }

        char escape[4];
        snprintf(escape, sizeof(escape), "%%%02X", static_cast<unsigned char>(c));
        encoded += escape;
    }

    return encoded;
}

String HttpRequestManager::BuildQueryString(const std::vector<std::pair<String, String>>& params) const
{
    if (params.empty())
        return "";

    String queryStream = "?";

    for (const auto& [key, value] : params)
    {
        if (queryStream.length() > 1)
            queryStream += "&";

        queryStream += UrlEncode(key) + "=" + UrlEncode(value);
    }

    return queryStream;
}

HttpResult HttpRequestManager::Get(const String& url, const std::vector<std::pair<String, String>>& params, const std::vector<std::pair<String, String>>& headers) {
    HttpResult result{ false, 0, "", "" };

    const String queryParams = BuildQueryString(params);
    const String fullUrl = url + queryParams;

    NetworkTls::Guard tlsGuard;
    WiFiClientSecure client;
    // Preserves the old begin(url) behaviour. Pinning roots for these public
    // APIs is a separate trust-policy change.
    client.setInsecure();

    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, fullUrl)) {
        result.errorMessage = "could not open connection";
        return result;
    }

    // add headers to request
    for (const auto& header : headers) {
        http.addHeader(header.first, header.second);
    }

    // send request and handle response
    NetworkTls::LogHeap("Before request", "GET", fullUrl.c_str());
    int responseCode = http.GET();
    NetworkTls::LogHeap("After request", "GET", fullUrl.c_str());
    result.statusCode = responseCode;

    if (responseCode > 0) {
        result.success = true;
        result.response = http.getString();
        NetworkTls::LogHeap("After response", "GET", fullUrl.c_str());
    }
    else {
        result.success = false;
        result.errorMessage = http.errorToString(responseCode);
        Serial.print("[GET] HTTP Error (");
        Serial.print(responseCode);
        Serial.print("): ");
        Serial.println(result.errorMessage);
    }

    http.end();
    client.stop();
    NetworkTls::LogHeap("After close", "GET", fullUrl.c_str());
    return result;
}

HttpResult HttpRequestManager::Post(const String& url, const String& body, const std::vector<std::pair<String, String>>& headers)
{
    HttpResult result{ false, 0, "", "" };

    NetworkTls::Guard tlsGuard;
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        result.errorMessage = "could not open connection";
        return result;
    }

    // add headers to request
    for (const auto& header : headers) {
        http.addHeader(header.first, header.second);
    }

    // send request and handle response
    NetworkTls::LogHeap("Before request", "POST", url.c_str());
    int responseCode = http.POST(body);
    NetworkTls::LogHeap("After request", "POST", url.c_str());
    result.statusCode = responseCode;

    if (responseCode > 0) {
        result.success = true;
        result.response = http.getString();
        NetworkTls::LogHeap("After response", "POST", url.c_str());
    }
    else {
        result.success = false;
        result.errorMessage = http.errorToString(responseCode);
        Serial.print("[POST] HTTP Error (");
        Serial.print(responseCode);
        Serial.print("): ");
        Serial.println(result.errorMessage);
    }

    http.end();
    client.stop();
    NetworkTls::LogHeap("After close", "POST", url.c_str());
    return result;
}
