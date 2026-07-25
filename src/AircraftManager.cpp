#include "AircraftManager.h"

constexpr int SCREEN_SIZE = 240;
constexpr int SCREEN_SIZE_DIV_2 = (SCREEN_SIZE / 2);

#include <ArduinoJson.h>

void AircraftManager::Initialise()
{
    // get centre point + radius
    lat = configServer.GetStoredString("latitude").toDouble();
    lon = configServer.GetStoredString("longitude").toDouble();
    rad = configServer.GetStoredString("radius").toDouble();

    // configuration
    const String renderSpeed = configServer.GetStoredString("speed");
    const String speedUnit = configServer.GetStoredString("speed-unit");
    const String renderAltitude = configServer.GetStoredString("altitude");
    const String altitudeUnit = configServer.GetStoredString("altitude-unit");
    const String renderDestination = configServer.GetStoredString("destination");
    const String renderTris = configServer.GetStoredString("triangle");
    if (!renderSpeed.isEmpty()) displaySpeed = renderSpeed == "true";
    if (!speedUnit.isEmpty()) displaySpeedInKnots = speedUnit == "knots";
    if (!renderAltitude.isEmpty()) displayAltitude = renderAltitude == "true";
    if (!altitudeUnit.isEmpty()) displayAltitudeInFeet = altitudeUnit == "feet" || altitudeUnit == "kft";
    if (!renderDestination.isEmpty()) displayDestination = renderDestination == "true";
    if (!renderTris.isEmpty()) displayTriangles = renderTris == "true" ? true : false;

    // calculate how often we can call OpenSky API before being rate limited
    constexpr int MS_PER_DAY = 24 * 60 * 60 * 1000;
    constexpr int ANONYMOUS_TOKENS_PER_DAY = 400;
    constexpr int AUTHED_TOKENS_PER_DAY = 4000;
    constexpr int TOKEN_BUFFER = 3;
    int dailyRequestBudget = ANONYMOUS_TOKENS_PER_DAY - TOKEN_BUFFER; // non-authed tokens minus buffer

    const String token = authHandler.GetValidToken(configServer.GetStoredString("opensky-id"), configServer.GetStoredString("opensky-secret"));
    if (!token.isEmpty())
        dailyRequestBudget = AUTHED_TOKENS_PER_DAY - TOKEN_BUFFER; // authed tokens minus buffer

    fetchInterval = MS_PER_DAY / dailyRequestBudget;
}

void AircraftManager::Update()
{
    unsigned long now = millis();

    // fetch cycle
    if (now - lastFetch >= fetchInterval) {
        lastFetch = now;

        // auth
        const String token = authHandler.GetValidToken(
            configServer.GetStoredString("opensky-id"),
            configServer.GetStoredString("opensky-secret")
        );

        std::vector<std::pair<String, String>> headers = {};
        if (!token.isEmpty()) headers.push_back({ "Authorization", "Bearer " + token });

        // request
        HttpResult result = http.Get(
            "https://opensky-network.org/api/states/all",
            {
              {"lamin", String(lat - rad)},
              {"lamax", String(lat + rad)},
              {"lomin", String(lon - rad)},
              {"lomax", String(lon + rad)}
            },
            headers
        );

        // If request failed, skip this update
        if (!result.success) {
            Serial.print("[WARN] OpenSky API request failed: ");
            Serial.println(result.errorMessage);
            return;
        }

        // track
        JsonDocument doc;
        deserializeJson(doc, result.response);
        auto aircraft = JsonParser::ParseArray<Aircraft>(doc["states"]);
        now = millis(); // override with post-parse timestamp

        for (auto& ac : aircraft) {
            auto it = trackedAircraft.find(ac.icao24);
            if (it == trackedAircraft.end())
                trackedAircraft.emplace(ac.icao24, TrackedAircraft{ ac, now });
            else
                it->second.Update(ac, now);
        }

        // remove any planes that disappeared from the feed
        for (auto it = trackedAircraft.begin(); it != trackedAircraft.end(); ) {
            bool aircraftPresent = std::any_of(aircraft.begin(), aircraft.end(), [&](const Aircraft& ac) { return ac.icao24 == it->first; });
            if (!aircraftPresent)
                it = trackedAircraft.erase(it);
            else
                ++it;
        }
    }

    ResolveNextDestination();
}

void AircraftManager::Draw(LGFX_Sprite& backbuffer)
{
    DrawRadarCircles(backbuffer);

    for (auto& [icao, tracked] : trackedAircraft) {
        if (tracked.state.onGround) continue;

        tracked.Tick();
        auto [predLat, predLon] = tracked.GetDisplayPosition();
        auto [x, y] = ProjectCoordinateToScreen(predLat, predLon);

        DrawAircraftInfo(backbuffer, x, y, tracked);

        if (displayTriangles)
            DrawAircraftTriangle(backbuffer, x, y, tracked);
        else
            backbuffer.fillCircle(x, y, 3, lgfx::color888(0, 255, 0));
    }
}

void AircraftManager::DrawRadarCircles(LGFX_Sprite& backbuffer) const
{
    constexpr int CENTRE = SCREEN_SIZE_DIV_2 - 1;
    constexpr int OUTER = SCREEN_SIZE_DIV_2 - 1;

    backbuffer.drawCircle(CENTRE, CENTRE, OUTER, lgfx::color888(0, 200, 0));
    backbuffer.drawCircle(CENTRE, CENTRE, (OUTER / 3) * 2, lgfx::color888(0, 64, 0));
    backbuffer.drawCircle(CENTRE, CENTRE, OUTER / 3, lgfx::color888(0, 32, 0));
}

std::pair<int, int> AircraftManager::ProjectCoordinateToScreen(float predLat, float predLon) const
{
    const float dLon = predLon - lon;
    const float dLat = predLat - lat;

    const float normLon = (dLon + rad) / (2.0f * rad);
    const float normLat = (dLat + rad) / (2.0f * rad);

    const int x = static_cast<int>(normLon * SCREEN_SIZE);
    const int y = static_cast<int>(SCREEN_SIZE - (normLat * SCREEN_SIZE));

    return { x, y };
}

void AircraftManager::DrawAircraftInfo(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const
{
    const int lineHeight = tft.fontHeight() + 1;
    int line = 0;

    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(0, 128, 0));
    backbuffer.drawString(tracked.state.callsign, x + 5, y + 5 + lineHeight * line++);

    if (displaySpeed) {
        String speedLabel;
        if (displaySpeedInKnots) {
            const float speedKnots = tracked.state.velocity * 1.94384f;
            speedLabel = String(speedKnots, 0) + "kt";
        } else {
            speedLabel = String(tracked.state.velocity, 1) + "m/s";
        }
        backbuffer.drawString(speedLabel, x + 5, y + 5 + lineHeight * line++);
    }

    if (displayAltitude) {
        String altitudeLabel;
        if (displayAltitudeInFeet) {
            const float altitudeFeet = tracked.state.baroAltitude * 3.28084f;
            if (altitudeFeet < 3000.0f)
                altitudeLabel = String(altitudeFeet, 0) + "ft";
            else if (altitudeFeet < 10000.0f)
                altitudeLabel = String(altitudeFeet / 1000.0f, 1) + "Kft";
            else
                altitudeLabel = String(altitudeFeet / 1000.0f, 0) + "Kft";
        } else {
            altitudeLabel = String(tracked.state.baroAltitude, 0) + "m";
        }
        backbuffer.drawString(altitudeLabel, x + 5, y + 5 + lineHeight * line++);
    }

    if (displayDestination && !tracked.route.isEmpty())
        backbuffer.drawString(tracked.route, x + 5, y + 5 + lineHeight * line);
}

void AircraftManager::ResolveNextDestination()
{
    constexpr unsigned long LOOKUP_INTERVAL_MS = 1000;

    if (!displayDestination)
        return;

    const unsigned long now = millis();
    if (now - lastDestinationLookup < LOOKUP_INTERVAL_MS)
        return;

    for (auto& [icao, tracked] : trackedAircraft) {
        if (tracked.state.onGround || tracked.state.callsign.isEmpty() || tracked.destinationLookupAttempted)
            continue;

        tracked.destinationLookupAttempted = true;
        lastDestinationLookup = now;

        String safeCallsign;
        safeCallsign.reserve(tracked.state.callsign.length());
        for (size_t i = 0; i < tracked.state.callsign.length(); ++i) {
            const char c = tracked.state.callsign[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                safeCallsign += c;
        }

        if (safeCallsign.isEmpty())
            return;

        const HttpResult result = http.Get("https://api.adsbdb.com/v0/callsign/" + safeCallsign);
        if (!result.success || result.statusCode < 200 || result.statusCode >= 300)
            return;

        JsonDocument doc;
        if (deserializeJson(doc, result.response))
            return;

        const JsonVariant flightRoute = doc["response"]["flightroute"];
        const JsonVariant origin = flightRoute["origin"];
        const JsonVariant destination = flightRoute["destination"];
        if (origin.isNull() || destination.isNull())
            return;

        String originCode = origin["iata_code"].as<String>();
        if (originCode.isEmpty())
            originCode = origin["icao_code"].as<String>();

        String destinationCode = destination["iata_code"].as<String>();
        if (destinationCode.isEmpty())
            destinationCode = destination["icao_code"].as<String>();

        if (!originCode.isEmpty() && !destinationCode.isEmpty())
            tracked.route = originCode + "-" + destinationCode;
        return;
    }
}

void AircraftManager::DrawAircraftTriangle(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const
{
    const float dx = std::sin(radians(tracked.state.trueTrack));
    const float dy = -std::cos(radians(tracked.state.trueTrack));
    const float px = -dy;
    const float py = dx;

    constexpr float TRIANGLE_LENGTH = 6.0f;
    constexpr float TRIANGLE_WIDTH = 3.0f;

    const float tipX = x + dx * TRIANGLE_LENGTH;
    const float tipY = y + dy * TRIANGLE_LENGTH;
    const float leftX = x - dx * TRIANGLE_LENGTH * 0.5f + px * TRIANGLE_WIDTH * 0.5f;
    const float leftY = y - dy * TRIANGLE_LENGTH * 0.5f + py * TRIANGLE_WIDTH * 0.5f;
    const float rightX = x - dx * TRIANGLE_LENGTH * 0.5f - px * TRIANGLE_WIDTH * 0.5f;
    const float rightY = y - dy * TRIANGLE_LENGTH * 0.5f - py * TRIANGLE_WIDTH * 0.5f;

    backbuffer.fillTriangle(tipX, tipY, leftX, leftY, rightX, rightY, lgfx::color888(0, 255, 0));
}
