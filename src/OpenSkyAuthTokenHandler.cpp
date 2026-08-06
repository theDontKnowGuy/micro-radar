#include "OpenSkyAuthTokenHandler.h"
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

    if (!resp.success) {
        Serial.print("[ERROR] OpenSky token request failed: ");
        Serial.println(resp.errorMessage);
        return "";
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, resp.response);

    if (error) {
        Serial.print("[ERROR] OpenSky token response JSON parse failed: ");
        Serial.println(error.f_str());
        return "";
    }

    const JsonVariant token = doc["access_token"];
    if (!token.is<String>()) {
        Serial.println("[WARN] Missing or non-string 'access_token' in OpenSky API response");
        return "";
    }

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

    if (clientId.isEmpty() || clientSecret.isEmpty())
        return "";

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
