#include "OpenSkyAuthTokenHandler.h"
#include "Diagnostics.h"
#include <ArduinoJson.h>

String OpenSkyAuthTokenHandler::FetchBearerToken(const String& url, const String& clientId, const String& clientSecret)
{
    // Both values are typed in by the owner on the configuration page. An
    // unencoded '&' or '=' in a secret would otherwise be read by the auth
    // server as the start of another form field.
    String body = "grant_type=client_credentials";
    body += "&client_id=" + HttpRequestManager::UrlEncode(clientId);
    body += "&client_secret=" + HttpRequestManager::UrlEncode(clientSecret);

    const HttpResult resp = http.Post(
        url,
        body,
        {
            {"Content-Type", "application/x-www-form-urlencoded"}
        }
    );

    // Through Diagnostics rather than Serial from here down. A radar that
    // cannot authenticate shows an empty face, and an empty face is exactly the
    // state nobody can debug over a USB cable that is not plugged in -- these
    // are the four lines that say why, so they are the four that have to reach
    // the dashboard.
    if (!resp.success) {
        Diagnostics::Warn("OpenSky token request failed: %s", resp.errorMessage.c_str());
        lastError = "OpenSky auth unreachable";
        return "";
    }

    // Checked before the body is parsed, because HttpRequestManager counts any
    // answer at all as a success. A rejected pair comes back as a 401 carrying a
    // JSON error object, which parses cleanly and has no access_token in it --
    // so without this the one failure an owner can actually fix reported itself
    // as a malformed response.
    if (resp.statusCode < 200 || resp.statusCode >= 300) {
        Diagnostics::Warn("OpenSky rejected the stored credentials (HTTP %d)", resp.statusCode);
        lastError = resp.statusCode == 401 || resp.statusCode == 403
            ? "OpenSky login rejected"
            : "OpenSky auth error";
        return "";
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, resp.response);

    if (error) {
        Diagnostics::Warn("OpenSky token response JSON parse failed: %s", error.c_str());
        lastError = "Bad reply from OpenSky";
        return "";
    }

    const JsonVariant token = doc["access_token"];
    if (!token.is<String>()) {
        Diagnostics::Warn("Missing or non-string 'access_token' in OpenSky API response");
        lastError = "Bad reply from OpenSky";
        return "";
    }

    lastError = "";
    return token.as<String>();
}

String OpenSkyAuthTokenHandler::GetValidToken(const String& clientId, const String& clientSecret)
{
    constexpr char TOKEN_URL[] =
        "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";
    constexpr unsigned long TOKEN_LIFETIME_MS = 29UL * 60UL * 1000UL;  // 30 min, 1 min buffer
    // Wrong credentials would otherwise put an auth POST in front of every
    // single aircraft fetch, because a failed fetch leaves no token to cache.
    constexpr unsigned long FAILURE_BACKOFF_MS = 5UL * 60UL * 1000UL;

    if (clientId.isEmpty() || clientSecret.isEmpty()) {
        lastError = "No OpenSky key stored";
        return "";
    }

    // Subtract-then-compare rather than `millis() > deadline`: millis() wraps
    // after 49.7 days, and the plain comparison reads every token as expired
    // from that moment on.
    const unsigned long now = millis();
    if (!bearerToken.isEmpty() && static_cast<long>(now - tokenExpiry) < 0)
        return bearerToken;
    if (bearerToken.isEmpty() && static_cast<long>(now - nextAttempt) < 0)
        return "";

    bearerToken = FetchBearerToken(TOKEN_URL, clientId, clientSecret);
    if (bearerToken.isEmpty())
        nextAttempt = now + FAILURE_BACKOFF_MS;
    else
        tokenExpiry = now + TOKEN_LIFETIME_MS;

    return bearerToken;
}
