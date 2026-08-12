#pragma once

#include "HttpRequestManager.h"

class OpenSkyAuthTokenHandler
{
private:
    HttpRequestManager& http;

    String bearerToken = "";
    unsigned long tokenExpiry = 0;  // millis() timestamp
    unsigned long nextAttempt = 0;  // millis() timestamp, set after a failed fetch

    // Why the last attempt came back empty, short enough to put on the panel.
    // Set on every failure and cleared on every success, so it is only ever read
    // in the state it describes.
    String lastError;

    String FetchBearerToken(const String& url, const String& clientId, const String& clientSecret);

public:
    OpenSkyAuthTokenHandler(HttpRequestManager& httpRequestManager) : http(httpRequestManager) {}
    ~OpenSkyAuthTokenHandler() = default;

    // Empty when no credentials are configured, or while backing off from a
    // rejected pair. There is nothing to fall back to: OpenSky is not asked for
    // aircraft without a token, so an empty return means the fetch is skipped.
    [[nodiscard]] String GetValidToken(const String& clientId, const String& clientSecret);

    // What went wrong the last time a token could not be had. Meaningful only
    // when GetValidToken() has returned empty; the caller puts it on the radar
    // face, which is the only channel a unit with no serial console has. Kept
    // across the five-minute backoff on purpose -- during it GetValidToken()
    // returns empty without asking anything, and the reason it is backing off
    // is still the reason there are no aircraft.
    [[nodiscard]] const String& LastError() const { return lastError; }
};
