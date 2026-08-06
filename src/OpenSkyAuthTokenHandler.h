#pragma once

#include "HttpRequestManager.h"

class OpenSkyAuthTokenHandler
{
private:
    HttpRequestManager& http;

    String bearerToken = "";
    unsigned long tokenExpiry = 0;  // millis() timestamp
    unsigned long nextAttempt = 0;  // millis() timestamp, set after a failed fetch

    String FetchBearerToken(const String& url, const String& clientId, const String& clientSecret);

public:
    OpenSkyAuthTokenHandler(HttpRequestManager& httpRequestManager) : http(httpRequestManager) {}
    ~OpenSkyAuthTokenHandler() = default;

    // Empty when no credentials are configured, or while backing off from a
    // rejected one -- callers fall back to OpenSky's anonymous allowance.
    [[nodiscard]] String GetValidToken(const String& clientId, const String& clientSecret);
};