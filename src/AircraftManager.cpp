#include "AircraftManager.h"

constexpr int SCREEN_SIZE = 240;
constexpr int SCREEN_SIZE_DIV_2 = (SCREEN_SIZE / 2);
constexpr unsigned long LABEL_LAYOUT_INTERVAL_MS = 1000;

#include <ArduinoJson.h>
#include <algorithm>

void AircraftManager::Initialise()
{
    // get centre point + radius
    lat = configServer.GetStoredString("latitude").toDouble();
    lon = configServer.GetStoredString("longitude").toDouble();
    rad = configServer.GetStoredString("radius").toDouble();
    openskyClientId = configServer.GetStoredString("opensky-id");
    openskyClientSecret = configServer.GetStoredString("opensky-secret");

    // configuration
    const String renderSpeed = configServer.GetStoredString("speed");
    const String speedUnit = configServer.GetStoredString("speed-unit");
    const String renderAltitude = configServer.GetStoredString("altitude");
    const String altitudeUnit = configServer.GetStoredString("altitude-unit");
    const String renderDestination = configServer.GetStoredString("destination");
    const String markerStyle = configServer.GetStoredString("aircraft-marker");
    if (!renderSpeed.isEmpty()) displaySpeed = renderSpeed == "true";
    if (!speedUnit.isEmpty()) displaySpeedInKnots = speedUnit == "knots";
    if (!renderAltitude.isEmpty()) displayAltitude = renderAltitude == "true";
    if (!altitudeUnit.isEmpty()) displayAltitudeInFeet = altitudeUnit == "feet" || altitudeUnit == "kft";
    if (!renderDestination.isEmpty()) displayDestination = renderDestination == "true";
    if (markerStyle == "triangle")
        aircraftMarkerStyle = AircraftMarkerStyle::Triangle;
    else if (markerStyle == "dot")
        aircraftMarkerStyle = AircraftMarkerStyle::Dot;
    else
        aircraftMarkerStyle = AircraftMarkerStyle::RadarVector;

    // calculate how often we can call OpenSky API before being rate limited
    constexpr int MS_PER_DAY = 24 * 60 * 60 * 1000;
    constexpr int ANONYMOUS_TOKENS_PER_DAY = 400;
    constexpr int AUTHED_TOKENS_PER_DAY = 4000;
    constexpr int TOKEN_BUFFER = 3;
    int dailyRequestBudget = ANONYMOUS_TOKENS_PER_DAY - TOKEN_BUFFER; // non-authed tokens minus buffer

    const String token = authHandler.GetValidToken(openskyClientId, openskyClientSecret);
    if (!token.isEmpty())
        dailyRequestBudget = AUTHED_TOKENS_PER_DAY - TOKEN_BUFFER; // authed tokens minus buffer

    fetchInterval = MS_PER_DAY / dailyRequestBudget;

    networkStateMutex = xSemaphoreCreateMutex();
    if (networkStateMutex == nullptr) {
        Serial.println("[ERROR] Could not create network state mutex; live updates disabled");
        return;
    }

    // This worker also runs the label optimizer. Give its deeply nested
    // scoring pass enough stack without consuming the render task's stack.
    constexpr uint32_t NETWORK_TASK_STACK = 16384;
    const BaseType_t taskCreated = xTaskCreatePinnedToCore(
        NetworkTaskEntry,
        "radar-network",
        NETWORK_TASK_STACK,
        this,
        1,
        &networkTaskHandle,
        0
    );
    if (taskCreated != pdPASS) {
        networkTaskHandle = nullptr;
        Serial.println("[ERROR] Could not create network task; live updates disabled");
    }
}

void AircraftManager::Update()
{
    ConsumeNetworkResults();

    const unsigned long now = millis();
    if (now - lastFetch >= fetchInterval &&
        ScheduleNetworkJob(NetworkJobType::FetchAircraft))
        lastFetch = now;

    ResolveNextDestination();
}

void AircraftManager::NetworkTaskEntry(void* context)
{
    static_cast<AircraftManager*>(context)->NetworkTaskLoop();
}

void AircraftManager::NetworkTaskLoop()
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        NetworkJobType job = NetworkJobType::None;
        String icao;
        String callsign;
        std::vector<RenderAircraft> layoutAircraft;
        std::vector<TrackedAircraft> layoutTracked;
        if (xSemaphoreTake(networkStateMutex, portMAX_DELAY) == pdTRUE) {
            job = networkJob;
            icao = networkJobIcao;
            callsign = networkJobCallsign;
            if (job == NetworkJobType::SolveLabels) {
                layoutAircraft.swap(labelLayoutJobAircraft);
                layoutTracked.swap(labelLayoutJobTracked);
            }
            networkJob = NetworkJobType::None;
            xSemaphoreGive(networkStateMutex);
        }

        if (job == NetworkJobType::FetchAircraft)
            RunAircraftFetch();
        else if (job == NetworkJobType::ResolveDestination)
            RunDestinationLookup(icao, callsign);
        else if (job == NetworkJobType::SolveLabels) {
            // Vector moves can change the address of the copied tracking
            // records, so reconnect the render records before solving.
            for (size_t i = 0; i < layoutAircraft.size(); ++i)
                layoutAircraft[i].tracked = &layoutTracked[i];

            SolveAircraftLabels(layoutAircraft);

            std::vector<LabelLayoutResult> results;
            results.reserve(layoutAircraft.size());
            for (const auto& current : layoutAircraft) {
                results.push_back({
                    current.tracked->state.icao24,
                    current.tracked->labelOffsetX,
                    current.tracked->labelOffsetY,
                    current.tracked->lastLabelMove
                });
            }

            if (xSemaphoreTake(networkStateMutex, portMAX_DELAY) == pdTRUE) {
                completedLabelLayout.swap(results);
                labelLayoutReady = true;
                networkBusy = false;
                xSemaphoreGive(networkStateMutex);
            }
        }
        else if (xSemaphoreTake(networkStateMutex, portMAX_DELAY) == pdTRUE) {
            networkBusy = false;
            xSemaphoreGive(networkStateMutex);
        }
    }
}

bool AircraftManager::ScheduleNetworkJob(NetworkJobType job, const String& icao, const String& callsign)
{
    if (networkTaskHandle == nullptr || networkStateMutex == nullptr)
        return false;

    if (xSemaphoreTake(networkStateMutex, 0) != pdTRUE)
        return false;

    if (networkBusy) {
        xSemaphoreGive(networkStateMutex);
        return false;
    }

    networkBusy = true;
    networkJob = job;
    networkJobIcao = icao;
    networkJobCallsign = callsign;
    xSemaphoreGive(networkStateMutex);
    xTaskNotifyGive(networkTaskHandle);
    return true;
}

bool AircraftManager::ScheduleLabelLayout(const std::vector<RenderAircraft>& aircraft)
{
    if (aircraft.empty() || networkTaskHandle == nullptr || networkStateMutex == nullptr)
        return false;

    if (xSemaphoreTake(networkStateMutex, 0) != pdTRUE)
        return false;

    if (networkBusy) {
        xSemaphoreGive(networkStateMutex);
        return false;
    }

    labelLayoutJobAircraft = aircraft;
    labelLayoutJobTracked.clear();
    labelLayoutJobTracked.reserve(aircraft.size());
    for (const auto& current : aircraft)
        labelLayoutJobTracked.push_back(*current.tracked);
    for (size_t i = 0; i < labelLayoutJobAircraft.size(); ++i)
        labelLayoutJobAircraft[i].tracked = &labelLayoutJobTracked[i];

    networkBusy = true;
    networkJob = NetworkJobType::SolveLabels;
    networkJobIcao = "";
    networkJobCallsign = "";
    xSemaphoreGive(networkStateMutex);
    xTaskNotifyGive(networkTaskHandle);
    return true;
}

void AircraftManager::RunAircraftFetch()
{
    const String token = authHandler.GetValidToken(openskyClientId, openskyClientSecret);
    std::vector<std::pair<String, String>> headers;
    if (!token.isEmpty())
        headers.push_back({ "Authorization", "Bearer " + token });

    const HttpResult result = http.Get(
        "https://opensky-network.org/api/states/all",
        {
            {"lamin", String(lat - rad)},
            {"lamax", String(lat + rad)},
            {"lomin", String(lon - rad)},
            {"lomax", String(lon + rad)}
        },
        headers
    );

    std::vector<Aircraft> fetchedAircraft;
    bool fetchSucceeded = false;
    if (result.success && result.statusCode >= 200 && result.statusCode < 300) {
        JsonDocument doc;
        const DeserializationError error = deserializeJson(doc, result.response);
        if (!error && doc["states"].is<JsonArray>()) {
            fetchedAircraft = JsonParser::ParseArray<Aircraft>(doc["states"]);
            fetchSucceeded = true;
        } else {
            Serial.print("[WARN] OpenSky response parse failed: ");
            Serial.println(error ? error.c_str() : "missing states array");
        }
    } else {
        Serial.print("[WARN] OpenSky API request failed");
        if (result.statusCode != 0) {
            Serial.print(" (HTTP ");
            Serial.print(result.statusCode);
            Serial.print(")");
        }
        if (!result.errorMessage.isEmpty()) {
            Serial.print(": ");
            Serial.print(result.errorMessage);
        }
        Serial.println();
    }

    if (xSemaphoreTake(networkStateMutex, portMAX_DELAY) == pdTRUE) {
        if (fetchSucceeded) {
            completedAircraftFetch.swap(fetchedAircraft);
            aircraftFetchReady = true;
        }
        networkBusy = false;
        xSemaphoreGive(networkStateMutex);
    }
}

void AircraftManager::RunDestinationLookup(const String& icao, const String& callsign)
{
    String safeCallsign;
    safeCallsign.reserve(callsign.length());
    for (size_t i = 0; i < callsign.length(); ++i) {
        const char c = callsign[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            safeCallsign += c;
    }

    String route;
    if (!safeCallsign.isEmpty()) {
        const HttpResult result = http.Get("https://api.adsbdb.com/v0/callsign/" + safeCallsign);
        if (result.success && result.statusCode >= 200 && result.statusCode < 300) {
            JsonDocument doc;
            if (!deserializeJson(doc, result.response)) {
                const JsonVariant flightRoute = doc["response"]["flightroute"];
                const JsonVariant origin = flightRoute["origin"];
                const JsonVariant destination = flightRoute["destination"];
                if (!origin.isNull() && !destination.isNull()) {
                    String originCode = origin["iata_code"].as<String>();
                    if (originCode.isEmpty())
                        originCode = origin["icao_code"].as<String>();

                    String destinationCode = destination["iata_code"].as<String>();
                    if (destinationCode.isEmpty())
                        destinationCode = destination["icao_code"].as<String>();

                    if (!originCode.isEmpty() && !destinationCode.isEmpty())
                        route = originCode + "-" + destinationCode;
                }
            }
        }
    }

    if (xSemaphoreTake(networkStateMutex, portMAX_DELAY) == pdTRUE) {
        completedRouteIcao = icao;
        completedRouteCallsign = callsign;
        completedRoute = route;
        routeLookupReady = true;
        networkBusy = false;
        xSemaphoreGive(networkStateMutex);
    }
}

void AircraftManager::ConsumeNetworkResults()
{
    std::vector<Aircraft> fetchedAircraft;
    bool hasAircraftFetch = false;
    String routeIcao;
    String routeCallsign;
    String route;
    bool hasRouteLookup = false;
    std::vector<LabelLayoutResult> labelLayout;
    bool hasLabelLayout = false;

    if (networkStateMutex != nullptr &&
        xSemaphoreTake(networkStateMutex, 0) == pdTRUE) {
        if (aircraftFetchReady) {
            fetchedAircraft.swap(completedAircraftFetch);
            aircraftFetchReady = false;
            hasAircraftFetch = true;
        }
        if (routeLookupReady) {
            routeIcao = completedRouteIcao;
            routeCallsign = completedRouteCallsign;
            route = completedRoute;
            routeLookupReady = false;
            hasRouteLookup = true;
        }
        if (labelLayoutReady) {
            labelLayout.swap(completedLabelLayout);
            labelLayoutReady = false;
            hasLabelLayout = true;
        }
        xSemaphoreGive(networkStateMutex);
    }

    if (hasAircraftFetch) {
        const unsigned long receivedAt = millis();
        for (auto& aircraft : fetchedAircraft) {
            auto tracked = trackedAircraft.find(aircraft.icao24);
            if (tracked == trackedAircraft.end()) {
                auto inserted = trackedAircraft.emplace(
                    aircraft.icao24,
                    TrackedAircraft{ aircraft, receivedAt }
                );
                // The first appearance is also sweep-latched. Its state is
                // available for locating the bearing, but it stays hidden
                // until the beam reaches it.
                inserted.first->second.visibleOnRadar = false;
            } else {
                tracked->second.QueueUpdate(aircraft, receivedAt);
            }
        }

        for (auto& [icao, tracked] : trackedAircraft) {
            const bool stillPresent = std::any_of(
                fetchedAircraft.begin(),
                fetchedAircraft.end(),
                [&](const Aircraft& aircraft) { return aircraft.icao24 == icao; }
            );
            if (!stillPresent)
                tracked.QueueRemoval();
        }
    }

    if (hasRouteLookup && !route.isEmpty()) {
        auto tracked = trackedAircraft.find(routeIcao);
        if (tracked != trackedAircraft.end() && tracked->second.state.callsign == routeCallsign) {
            tracked->second.route = route;
            labelLayoutDirty = true;
        }
    }

    if (hasLabelLayout) {
        for (const auto& placement : labelLayout) {
            auto tracked = trackedAircraft.find(placement.icao);
            if (tracked == trackedAircraft.end())
                continue;

            tracked->second.labelOffsetX = placement.offsetX;
            tracked->second.labelOffsetY = placement.offsetY;
            tracked->second.lastLabelMove = placement.lastMove;
            tracked->second.hasLabelPlacement = true;
        }
        // Space solves from completion, not start. A dense solve can take
        // longer, and should not immediately queue another copy of itself.
        lastLabelLayout = millis();
    }
}

void AircraftManager::ApplySweepUpdates(
    float sweepAngle,
    bool sweepEnabled,
    unsigned long sweepPeriodMs
)
{
    const unsigned long now = millis();

    if (!sweepEnabled) {
        bool changed = false;
        for (auto tracked = trackedAircraft.begin(); tracked != trackedAircraft.end(); ) {
            if (tracked->second.queuedRemoval) {
                tracked = trackedAircraft.erase(tracked);
                changed = true;
                continue;
            }

            changed |= tracked->second.ApplyQueuedUpdate();
            if (!tracked->second.visibleOnRadar) {
                tracked->second.visibleOnRadar = true;
                changed = true;
            }
            ++tracked;
        }
        if (changed)
            labelLayoutDirty = true;
        hasPreviousSweepAngle = false;
        return;
    }

    if (!hasPreviousSweepAngle) {
        previousSweepAngle = sweepAngle;
        lastSweepFrameAt = now;
        hasPreviousSweepAngle = true;
        return;
    }

    const bool fullRevolutionElapsed =
        sweepPeriodMs > 0 && now - lastSweepFrameAt >= sweepPeriodMs;
    float sweptAngle = sweepAngle - previousSweepAngle;
    if (sweptAngle < 0.0f)
        sweptAngle += TWO_PI;

    auto refreshSweepBearing = [this](TrackedAircraft& current) {
        const auto [displayLat, displayLon] = current.GetRadarDisplayPosition();
        const auto [screenX, screenY] = ProjectCoordinateToScreen(displayLat, displayLon);
        current.radarSweepBearing = std::atan2(
            static_cast<float>(screenY - (SCREEN_SIZE_DIV_2 - 1)),
            static_cast<float>(screenX - (SCREEN_SIZE_DIV_2 - 1))
        );
        if (current.radarSweepBearing < 0.0f)
            current.radarSweepBearing += TWO_PI;
        current.hasRadarSweepBearing = true;
    };

    for (auto tracked = trackedAircraft.begin(); tracked != trackedAircraft.end(); ) {
        TrackedAircraft& current = tracked->second;
        if (!current.hasRadarSweepBearing)
            refreshSweepBearing(current);

        // A target that moves slightly ahead of the beam after being latched
        // must not be caught a second time during the same revolution.
        const unsigned long sweepRearmMs = (sweepPeriodMs * 3UL) / 4UL;
        if (!fullRevolutionElapsed && current.visibleOnRadar &&
            current.lastRadarSweepUpdateAt != 0 &&
            now - current.lastRadarSweepUpdateAt < sweepRearmMs) {
            ++tracked;
            continue;
        }

        float angleFromPrevious = current.radarSweepBearing - previousSweepAngle;
        if (angleFromPrevious < 0.0f)
            angleFromPrevious += TWO_PI;
        const bool sweepCrossedAircraft =
            fullRevolutionElapsed ||
            (angleFromPrevious > 0.0f && angleFromPrevious <= sweptAngle);
        if (!sweepCrossedAircraft) {
            ++tracked;
            continue;
        }

        if (current.queuedRemoval) {
            tracked = trackedAircraft.erase(tracked);
        } else {
            current.ApplyQueuedUpdate();
            current.LatchRadarDisplayPosition();
            refreshSweepBearing(current);
            current.visibleOnRadar = true;
            current.lastRadarSweepUpdateAt = now;
            ++tracked;
        }
        sweepAppliedAircraftChanges = true;
    }

    const bool completedRevolution = sweepAngle < previousSweepAngle;
    if ((completedRevolution || fullRevolutionElapsed) && sweepAppliedAircraftChanges) {
        // Re-run the global placement solver once for the completed sweep,
        // never once per aircraft.
        labelLayoutDirty = true;
        sweepAppliedAircraftChanges = false;
    }

    previousSweepAngle = sweepAngle;
    lastSweepFrameAt = now;
}

void AircraftManager::Draw(
    LGFX_Sprite& backbuffer,
    float sweepAngle,
    bool sweepEnabled,
    unsigned long sweepPeriodMs
)
{
    ApplySweepUpdates(sweepAngle, sweepEnabled, sweepPeriodMs);
    DrawRadarCircles(backbuffer);

    backbuffer.setTextSize(1);

    std::vector<RenderAircraft> renderAircraft;
    renderAircraft.reserve(trackedAircraft.size());

    for (auto& [icao, tracked] : trackedAircraft) {
        if (!tracked.visibleOnRadar || tracked.state.onGround)
            continue;

        tracked.Tick();
        const auto [predLat, predLon] = sweepEnabled
            ? tracked.GetRadarDisplayPosition()
            : tracked.GetDisplayPosition();
        auto [x, y] = ProjectCoordinateToScreen(predLat, predLon);

        // The round panel clips corners of the 240x240 sprite. Do not show a
        // data block unless the complete aircraft marker is actually visible.
        constexpr int RADAR_CENTRE = SCREEN_SIZE_DIV_2 - 1;
        const int markerRadius = aircraftMarkerStyle == AircraftMarkerStyle::RadarVector ? 11 : 7;
        const int maxMarkerDistance = SCREEN_SIZE_DIV_2 - 1 - markerRadius;
        const int markerDx = x - RADAR_CENTRE;
        const int markerDy = y - RADAR_CENTRE;
        if (markerDx * markerDx + markerDy * markerDy > maxMarkerDistance * maxMarkerDistance)
            continue;

        renderAircraft.push_back(BuildRenderAircraft(backbuffer, x, y, tracked));
    }

    PlaceAircraftLabels(renderAircraft);

    // Leaders go below text and aircraft symbols. This keeps a displaced label
    // associated with its marker without obscuring either one.
    for (const auto& aircraft : renderAircraft)
        DrawLabelLeader(backbuffer, aircraft);

    for (const auto& aircraft : renderAircraft)
        DrawAircraftInfo(backbuffer, aircraft);

    for (const auto& aircraft : renderAircraft) {
        switch (aircraftMarkerStyle) {
            case AircraftMarkerStyle::RadarVector:
                DrawAircraftRadarVector(backbuffer, aircraft.x, aircraft.y, *aircraft.tracked);
                break;
            case AircraftMarkerStyle::Triangle:
                DrawAircraftTriangle(backbuffer, aircraft.x, aircraft.y, *aircraft.tracked);
                break;
            case AircraftMarkerStyle::Dot:
                backbuffer.fillCircle(aircraft.x, aircraft.y, 3, lgfx::color888(0, 255, 0));
                break;
        }
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

AircraftManager::RenderAircraft AircraftManager::BuildRenderAircraft(
    LGFX_Sprite& backbuffer,
    int x,
    int y,
    TrackedAircraft& tracked
) const
{
    RenderAircraft result;
    result.tracked = &tracked;
    result.x = x;
    result.y = y;
    result.lines[result.lineCount++] = tracked.state.callsign;

    if (displaySpeed && result.lineCount < 4) {
        String speedLabel;
        if (displaySpeedInKnots) {
            const float speedKnots = tracked.state.velocity * 1.94384f;
            speedLabel = String(speedKnots, 0) + "kt";
        } else {
            speedLabel = String(tracked.state.velocity, 1) + "m/s";
        }
        result.lines[result.lineCount++] = speedLabel;
    }

    if (displayAltitude && result.lineCount < 4) {
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

        // Compact climb/descent trend, as used in radar data blocks. Ignore
        // small vertical-rate noise so the indicator does not flicker.
        if (tracked.state.verticalRate > 1.0f)
            altitudeLabel += "^";
        else if (tracked.state.verticalRate < -1.0f)
            altitudeLabel += "v";

        result.lines[result.lineCount++] = altitudeLabel;
    }

    if (displayDestination && !tracked.route.isEmpty() && result.lineCount < 4)
        result.lines[result.lineCount++] = tracked.route;

    const int lineHeight = backbuffer.fontHeight() + 1;
    result.callsignWidth = backbuffer.textWidth(result.lines[0]);
    int labelWidth = 0;
    for (uint8_t i = 0; i < result.lineCount; ++i)
        labelWidth = std::max(labelWidth, static_cast<int>(backbuffer.textWidth(result.lines[i])));

    result.label.width = labelWidth;
    result.label.height = result.lineCount * lineHeight;
    return result;
}

void AircraftManager::PlaceAircraftLabels(std::vector<RenderAircraft>& aircraft)
{
    if (aircraft.empty())
        return;

    const unsigned long now = millis();
    for (auto& current : aircraft) {
        if (current.tracked->hasLabelPlacement) {
            current.label.x = current.x + current.tracked->labelOffsetX;
            current.label.y = current.y + current.tracked->labelOffsetY;
        } else {
            // A close, predictable temporary position is used for the few
            // frames before a new aircraft's first background solve arrives.
            current.label.x = current.x + 8;
            current.label.y = current.y - current.label.height / 2;
        }
    }

    // Never make rendering wait for placement. The display keeps following
    // the last stable offsets while core 0 searches for the next layout.
    if ((labelLayoutDirty || now - lastLabelLayout >= LABEL_LAYOUT_INTERVAL_MS) &&
        ScheduleLabelLayout(aircraft)) {
        labelLayoutDirty = false;
    }
}

void AircraftManager::SolveAircraftLabels(std::vector<RenderAircraft>& aircraft)
{
    if (aircraft.empty())
        return;

    const unsigned long now = millis();

    constexpr int LABEL_GAP = 8;
    const int MARKER_RADIUS = aircraftMarkerStyle == AircraftMarkerStyle::RadarVector ? 11 : 7;
    constexpr int RADAR_CENTRE = SCREEN_SIZE_DIV_2 - 1;
    constexpr int USABLE_RADIUS = SCREEN_SIZE_DIV_2 - 3;
    constexpr uint8_t MAX_CANDIDATES = 25;
    constexpr uint8_t GLOBAL_PASSES = 4;
    constexpr uint8_t PAIR_REPAIR_PASSES = 2;
    constexpr uint8_t PAIR_CANDIDATES = 17;
    constexpr int64_t LEADER_DISTANCE_WEIGHT = 24;
    constexpr int64_t SWITCH_PENALTY = 24000;
    constexpr int64_t SHORTER_LEADER_SWITCH_PENALTY = 12000;
    constexpr int64_t VERTICAL_SIDE_CHANGE_PENALTY = 36000;
    constexpr int64_t COOLDOWN_PENALTY = 90000;
    constexpr unsigned long LABEL_MOVE_COOLDOWN_MS = 1200;
    constexpr int MATERIAL_LEADER_REDUCTION_SQUARED = 14 * 14;

    struct Segment {
        int16_t x1;
        int16_t y1;
        int16_t x2;
        int16_t y2;
    };

    // Costs are compared lexicographically in the operational priority order:
    // legibility and unambiguous ownership first, aesthetics and motion last.
    struct LayoutCost {
        int64_t outside = 0;
        int64_t callsignConflicts = 0;
        int64_t callsignOverlapArea = 0;
        int64_t markerConflicts = 0;
        int64_t markerOverlapArea = 0;
        int64_t labelConflicts = 0;
        int64_t labelOverlapArea = 0;
        int64_t leaderCrossings = 0;
        int64_t leaderLabelCrossings = 0;
        int64_t leaderMarkerCrossings = 0;
        int64_t ownershipAmbiguity = 0;
        int64_t preference = 0;

        void Add(const LayoutCost& other) {
            outside += other.outside;
            callsignConflicts += other.callsignConflicts;
            callsignOverlapArea += other.callsignOverlapArea;
            markerConflicts += other.markerConflicts;
            markerOverlapArea += other.markerOverlapArea;
            labelConflicts += other.labelConflicts;
            labelOverlapArea += other.labelOverlapArea;
            leaderCrossings += other.leaderCrossings;
            leaderLabelCrossings += other.leaderLabelCrossings;
            leaderMarkerCrossings += other.leaderMarkerCrossings;
            ownershipAmbiguity += other.ownershipAmbiguity;
            preference += other.preference;
        }

        bool IsBetterThan(const LayoutCost& other) const {
            if (outside != other.outside) return outside < other.outside;
            if (callsignConflicts != other.callsignConflicts) return callsignConflicts < other.callsignConflicts;
            if (callsignOverlapArea != other.callsignOverlapArea) return callsignOverlapArea < other.callsignOverlapArea;
            if (markerConflicts != other.markerConflicts) return markerConflicts < other.markerConflicts;
            if (markerOverlapArea != other.markerOverlapArea) return markerOverlapArea < other.markerOverlapArea;
            if (labelConflicts != other.labelConflicts) return labelConflicts < other.labelConflicts;
            if (labelOverlapArea != other.labelOverlapArea) return labelOverlapArea < other.labelOverlapArea;
            if (leaderCrossings != other.leaderCrossings) return leaderCrossings < other.leaderCrossings;
            if (leaderLabelCrossings != other.leaderLabelCrossings) return leaderLabelCrossings < other.leaderLabelCrossings;
            if (leaderMarkerCrossings != other.leaderMarkerCrossings) return leaderMarkerCrossings < other.leaderMarkerCrossings;
            if (ownershipAmbiguity != other.ownershipAmbiguity) return ownershipAmbiguity < other.ownershipAmbiguity;
            return preference < other.preference;
        }
    };

    struct CandidateSet {
        LabelBox boxes[MAX_CANDIDATES];
        uint8_t count = 0;
    };

    auto sameBox = [](const LabelBox& a, const LabelBox& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    };

    auto overlapArea = [](const LabelBox& a, const LabelBox& b) -> int {
        const int width = std::max(0, std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x));
        const int height = std::max(0, std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y));
        return width * height;
    };

    auto pointInBox = [](int x, int y, const LabelBox& box) {
        return x >= box.x && x <= box.x + box.width &&
               y >= box.y && y <= box.y + box.height;
    };

    auto orientation = [](int ax, int ay, int bx, int by, int cx, int cy) -> int32_t {
        return static_cast<int32_t>(bx - ax) * (cy - ay) -
               static_cast<int32_t>(by - ay) * (cx - ax);
    };

    auto segmentsIntersect = [&](const Segment& a, const Segment& b) {
        const int32_t o1 = orientation(a.x1, a.y1, a.x2, a.y2, b.x1, b.y1);
        const int32_t o2 = orientation(a.x1, a.y1, a.x2, a.y2, b.x2, b.y2);
        const int32_t o3 = orientation(b.x1, b.y1, b.x2, b.y2, a.x1, a.y1);
        const int32_t o4 = orientation(b.x1, b.y1, b.x2, b.y2, a.x2, a.y2);

        if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) &&
            ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0)))
            return true;

        auto onSegment = [](int ax, int ay, int bx, int by, int px, int py) {
            return px >= std::min(ax, bx) && px <= std::max(ax, bx) &&
                   py >= std::min(ay, by) && py <= std::max(ay, by);
        };
        return (o1 == 0 && onSegment(a.x1, a.y1, a.x2, a.y2, b.x1, b.y1)) ||
               (o2 == 0 && onSegment(a.x1, a.y1, a.x2, a.y2, b.x2, b.y2)) ||
               (o3 == 0 && onSegment(b.x1, b.y1, b.x2, b.y2, a.x1, a.y1)) ||
               (o4 == 0 && onSegment(b.x1, b.y1, b.x2, b.y2, a.x2, a.y2));
    };

    auto segmentIntersectsBox = [&](const Segment& segment, const LabelBox& box) {
        if (pointInBox(segment.x1, segment.y1, box) || pointInBox(segment.x2, segment.y2, box))
            return true;

        const int right = box.x + box.width;
        const int bottom = box.y + box.height;
        const Segment edges[4] = {
            { box.x, box.y, static_cast<int16_t>(right), box.y },
            { static_cast<int16_t>(right), box.y, static_cast<int16_t>(right), static_cast<int16_t>(bottom) },
            { static_cast<int16_t>(right), static_cast<int16_t>(bottom), box.x, static_cast<int16_t>(bottom) },
            { box.x, static_cast<int16_t>(bottom), box.x, box.y }
        };
        for (const auto& edge : edges)
            if (segmentsIntersect(segment, edge))
                return true;
        return false;
    };

    auto makeLeader = [](const RenderAircraft& current, const LabelBox& label) -> Segment {
        const int endX = std::max<int>(label.x, std::min<int>(current.x, label.x + label.width));
        const int endY = std::max<int>(label.y, std::min<int>(current.y, label.y + label.height));
        return {
            current.x,
            current.y,
            static_cast<int16_t>(endX),
            static_cast<int16_t>(endY)
        };
    };

    auto leaderDistanceSquared = [&](const RenderAircraft& current, const LabelBox& label) {
        const Segment leader = makeLeader(current, label);
        const int dx = leader.x2 - leader.x1;
        const int dy = leader.y2 - leader.y1;
        return dx * dx + dy * dy;
    };

    auto centreDistanceSquared = [](const LabelBox& label, const RenderAircraft& current) {
        const int dx = label.x + label.width / 2 - current.x;
        const int dy = label.y + label.height / 2 - current.y;
        return dx * dx + dy * dy;
    };

    auto callsignBox = [](const RenderAircraft& current, const LabelBox& label) {
        LabelBox callsign = label;
        callsign.width = current.callsignWidth;
        callsign.height = current.lineCount > 0 ? label.height / current.lineCount : label.height;
        return callsign;
    };

    auto markerBox = [MARKER_RADIUS](const RenderAircraft& current) -> LabelBox {
        return {
            static_cast<int16_t>(current.x - MARKER_RADIUS),
            static_cast<int16_t>(current.y - MARKER_RADIUS),
            static_cast<int16_t>(MARKER_RADIUS * 2 + 1),
            static_cast<int16_t>(MARKER_RADIUS * 2 + 1)
        };
    };

    std::vector<LabelBox> previousLabels;
    previousLabels.reserve(aircraft.size());
    std::vector<CandidateSet> candidates(aircraft.size());

    static constexpr int8_t DIRECTIONS[8][2] = {
        { 1, 0 }, { -1, 0 }, { 0, -1 }, { 0, 1 },
        { 1, -1 }, { -1, -1 }, { 1, 1 }, { -1, 1 }
    };
    static constexpr int8_t EXTRA_DISTANCE[3] = { 0, 14, 30 };

    for (size_t index = 0; index < aircraft.size(); ++index) {
        auto& current = aircraft[index];
        auto& set = candidates[index];

        const LabelBox previous = {
            static_cast<int16_t>(current.x + current.tracked->labelOffsetX),
            static_cast<int16_t>(current.y + current.tracked->labelOffsetY),
            current.label.width,
            current.label.height
        };
        previousLabels.push_back(previous);

        auto addCandidate = [&](int left, int top) {
            const LabelBox candidate = {
                static_cast<int16_t>(left),
                static_cast<int16_t>(top),
                current.label.width,
                current.label.height
            };
            for (uint8_t i = 0; i < set.count; ++i)
                if (sameBox(set.boxes[i], candidate))
                    return;
            if (set.count < MAX_CANDIDATES)
                set.boxes[set.count++] = candidate;
        };

        if (current.tracked->hasLabelPlacement)
            addCandidate(previous.x, previous.y);

        // Store all near candidates first, then medium and far rings. Pair
        // repair can therefore use a bounded near/medium subset.
        for (const int extra : EXTRA_DISTANCE) {
            for (const auto& direction : DIRECTIONS) {
                int left = current.x - current.label.width / 2;
                int top = current.y - current.label.height / 2;

                if (direction[0] > 0)
                    left = current.x + LABEL_GAP + extra;
                else if (direction[0] < 0)
                    left = current.x - LABEL_GAP - extra - current.label.width;

                if (direction[1] > 0)
                    top = current.y + LABEL_GAP + extra;
                else if (direction[1] < 0)
                    top = current.y - LABEL_GAP - extra - current.label.height;

                addCandidate(left, top);
            }
        }

        current.label = current.tracked->hasLabelPlacement ? previous : set.boxes[0];
    }

    auto unaryCost = [&](size_t index, const LabelBox& candidate) {
        LayoutCost cost;
        const auto& current = aircraft[index];

        const int cornerX[4] = {
            candidate.x, candidate.x + candidate.width,
            candidate.x, candidate.x + candidate.width
        };
        const int cornerY[4] = {
            candidate.y, candidate.y,
            candidate.y + candidate.height, candidate.y + candidate.height
        };
        for (uint8_t corner = 0; corner < 4; ++corner) {
            const int dx = cornerX[corner] - RADAR_CENTRE;
            const int dy = cornerY[corner] - RADAR_CENTRE;
            const int excess = dx * dx + dy * dy - USABLE_RADIUS * USABLE_RADIUS;
            if (excess > 0)
                cost.outside += 1000000 + excess;
        }

        const Segment leader = makeLeader(current, candidate);
        for (size_t otherIndex = 0; otherIndex < aircraft.size(); ++otherIndex) {
            const auto& other = aircraft[otherIndex];
            const LabelBox marker = markerBox(other);
            const int markerArea = overlapArea(candidate, marker);
            if (markerArea > 0) {
                ++cost.markerConflicts;
                cost.markerOverlapArea += markerArea;
            }
            if (otherIndex != index && segmentIntersectsBox(leader, marker))
                ++cost.leaderMarkerCrossings;
        }

        const int ownCentreX = candidate.x + candidate.width / 2;
        const int ownCentreY = candidate.y + candidate.height / 2;
        const int ownDx = ownCentreX - current.x;
        const int ownDy = ownCentreY - current.y;
        const int ownDistance = ownDx * ownDx + ownDy * ownDy;
        for (size_t otherIndex = 0; otherIndex < aircraft.size(); ++otherIndex) {
            if (otherIndex == index)
                continue;
            const int foreignDx = ownCentreX - aircraft[otherIndex].x;
            const int foreignDy = ownCentreY - aircraft[otherIndex].y;
            const int foreignDistance = foreignDx * foreignDx + foreignDy * foreignDy;
            if (foreignDistance + 64 < ownDistance)
                cost.ownershipAmbiguity += ownDistance - foreignDistance;
        }

        const int distanceSquared = leaderDistanceSquared(current, candidate);
        cost.preference += static_cast<int64_t>(distanceSquared) * LEADER_DISTANCE_WEIGHT;

        if (current.tracked->hasLabelPlacement && !sameBox(candidate, previousLabels[index])) {
            int64_t movePenalty = SWITCH_PENALTY;
            const int previousDistance = leaderDistanceSquared(current, previousLabels[index]);
            if (previousDistance - distanceSquared >= MATERIAL_LEADER_REDUCTION_SQUARED)
                movePenalty = SHORTER_LEADER_SWITCH_PENALTY;

            const int previousCentreY = previousLabels[index].y + previousLabels[index].height / 2;
            const int candidateCentreY = candidate.y + candidate.height / 2;
            if ((previousCentreY < current.y && candidateCentreY > current.y) ||
                (previousCentreY > current.y && candidateCentreY < current.y))
                movePenalty += VERTICAL_SIDE_CHANGE_PENALTY;

            if (now - current.tracked->lastLabelMove < LABEL_MOVE_COOLDOWN_MS)
                movePenalty += COOLDOWN_PENALTY;
            cost.preference += movePenalty;
        }

        return cost;
    };

    auto interactionCost = [&](size_t firstIndex, const LabelBox& firstLabel,
                               size_t secondIndex, const LabelBox& secondLabel) {
        LayoutCost cost;
        const auto& first = aircraft[firstIndex];
        const auto& second = aircraft[secondIndex];

        const int firstCallsignArea = overlapArea(callsignBox(first, firstLabel), secondLabel);
        const int secondCallsignArea = overlapArea(firstLabel, callsignBox(second, secondLabel));
        if (firstCallsignArea > 0)
            ++cost.callsignConflicts;
        if (secondCallsignArea > 0)
            ++cost.callsignConflicts;
        cost.callsignOverlapArea += firstCallsignArea + secondCallsignArea;

        const int labelArea = overlapArea(firstLabel, secondLabel);
        if (labelArea > 0)
            ++cost.labelConflicts;
        cost.labelOverlapArea += labelArea;

        const Segment firstLeader = makeLeader(first, firstLabel);
        const Segment secondLeader = makeLeader(second, secondLabel);
        if (segmentsIntersect(firstLeader, secondLeader))
            ++cost.leaderCrossings;
        if (segmentIntersectsBox(firstLeader, secondLabel))
            ++cost.leaderLabelCrossings;
        if (segmentIntersectsBox(secondLeader, firstLabel))
            ++cost.leaderLabelCrossings;
        return cost;
    };

    auto costAgainstLayout = [&](size_t index, const LabelBox& candidate, size_t skippedIndex) {
        LayoutCost cost = unaryCost(index, candidate);
        for (size_t otherIndex = 0; otherIndex < aircraft.size(); ++otherIndex) {
            if (otherIndex == index || otherIndex == skippedIndex)
                continue;
            cost.Add(interactionCost(index, candidate, otherIndex, aircraft[otherIndex].label));
        }
        return cost;
    };

    auto refineGlobally = [&]() {
        bool anyChanged = false;
        for (uint8_t pass = 0; pass < GLOBAL_PASSES; ++pass) {
            bool passChanged = false;
            for (size_t step = 0; step < aircraft.size(); ++step) {
                const size_t index = (pass & 1) ? aircraft.size() - 1 - step : step;
                LabelBox best = aircraft[index].label;
                LayoutCost bestCost = costAgainstLayout(index, best, aircraft.size());

                for (uint8_t candidateIndex = 0; candidateIndex < candidates[index].count; ++candidateIndex) {
                    const LabelBox& candidate = candidates[index].boxes[candidateIndex];
                    const LayoutCost cost = costAgainstLayout(index, candidate, aircraft.size());
                    if (cost.IsBetterThan(bestCost)) {
                        best = candidate;
                        bestCost = cost;
                    }
                }

                if (!sameBox(best, aircraft[index].label)) {
                    aircraft[index].label = best;
                    passChanged = true;
                    anyChanged = true;
                }
            }
            if (!passChanged)
                break;
        }
        return anyChanged;
    };

    auto pairCost = [&](size_t firstIndex, const LabelBox& firstLabel,
                        size_t secondIndex, const LabelBox& secondLabel) {
        LayoutCost cost = unaryCost(firstIndex, firstLabel);
        cost.Add(unaryCost(secondIndex, secondLabel));
        cost.Add(interactionCost(firstIndex, firstLabel, secondIndex, secondLabel));

        for (size_t otherIndex = 0; otherIndex < aircraft.size(); ++otherIndex) {
            if (otherIndex == firstIndex || otherIndex == secondIndex)
                continue;
            cost.Add(interactionCost(firstIndex, firstLabel, otherIndex, aircraft[otherIndex].label));
            cost.Add(interactionCost(secondIndex, secondLabel, otherIndex, aircraft[otherIndex].label));
        }
        return cost;
    };

    refineGlobally();

    // A crossed pair can be a 2-opt trap: moving either label alone is worse
    // because it temporarily occupies the other's slot. Search both labels'
    // near/medium candidates together and commit the repair atomically.
    for (uint8_t repairPass = 0; repairPass < PAIR_REPAIR_PASSES; ++repairPass) {
        bool repaired = false;
        for (size_t firstIndex = 0; firstIndex < aircraft.size(); ++firstIndex) {
            for (size_t secondIndex = firstIndex + 1; secondIndex < aircraft.size(); ++secondIndex) {
                const Segment firstLeader = makeLeader(aircraft[firstIndex], aircraft[firstIndex].label);
                const Segment secondLeader = makeLeader(aircraft[secondIndex], aircraft[secondIndex].label);
                const bool leadersCross = segmentsIntersect(firstLeader, secondLeader);
                const bool ownershipIsSwapped =
                    centreDistanceSquared(aircraft[firstIndex].label, aircraft[secondIndex]) + 64 <
                        centreDistanceSquared(aircraft[firstIndex].label, aircraft[firstIndex]) &&
                    centreDistanceSquared(aircraft[secondIndex].label, aircraft[firstIndex]) + 64 <
                        centreDistanceSquared(aircraft[secondIndex].label, aircraft[secondIndex]);
                if (!leadersCross && !ownershipIsSwapped)
                    continue;

                LabelBox bestFirst = aircraft[firstIndex].label;
                LabelBox bestSecond = aircraft[secondIndex].label;
                LayoutCost bestCost = pairCost(firstIndex, bestFirst, secondIndex, bestSecond);

                const uint8_t firstCount = std::min<uint8_t>(PAIR_CANDIDATES, candidates[firstIndex].count);
                const uint8_t secondCount = std::min<uint8_t>(PAIR_CANDIDATES, candidates[secondIndex].count);
                for (uint8_t firstOption = 0; firstOption <= firstCount; ++firstOption) {
                    const LabelBox& firstCandidate = firstOption == 0
                        ? aircraft[firstIndex].label
                        : candidates[firstIndex].boxes[firstOption - 1];
                    for (uint8_t secondOption = 0; secondOption <= secondCount; ++secondOption) {
                        const LabelBox& secondCandidate = secondOption == 0
                            ? aircraft[secondIndex].label
                            : candidates[secondIndex].boxes[secondOption - 1];
                        const LayoutCost cost = pairCost(
                            firstIndex, firstCandidate,
                            secondIndex, secondCandidate
                        );
                        if (cost.IsBetterThan(bestCost)) {
                            bestFirst = firstCandidate;
                            bestSecond = secondCandidate;
                            bestCost = cost;
                        }
                    }
                }

                if (!sameBox(bestFirst, aircraft[firstIndex].label) ||
                    !sameBox(bestSecond, aircraft[secondIndex].label)) {
                    aircraft[firstIndex].label = bestFirst;
                    aircraft[secondIndex].label = bestSecond;
                    repaired = true;
                }
            }
        }
        if (!repaired)
            break;
        if (repairPass + 1 < PAIR_REPAIR_PASSES)
            refineGlobally();
    }

    for (auto& current : aircraft) {
        const int16_t offsetX = current.label.x - current.x;
        const int16_t offsetY = current.label.y - current.y;
        const bool placementChanged =
            !current.tracked->hasLabelPlacement ||
            current.tracked->labelOffsetX != offsetX ||
            current.tracked->labelOffsetY != offsetY;

        current.tracked->labelOffsetX = offsetX;
        current.tracked->labelOffsetY = offsetY;
        current.tracked->hasLabelPlacement = true;
        if (placementChanged)
            current.tracked->lastLabelMove = now;
    }
}

void AircraftManager::DrawAircraftInfo(LGFX_Sprite& backbuffer, const RenderAircraft& aircraft) const
{
    const int lineHeight = backbuffer.fontHeight() + 1;
    backbuffer.setTextSize(1);

    for (uint8_t line = 0; line < aircraft.lineCount; ++line) {
        // Radar tags use the identity line as the visual anchor. Supporting
        // values stay quieter so dense groups remain readable.
        const uint8_t brightness = line == 0 ? 210 : 120;
        backbuffer.setTextColor(lgfx::color888(0, brightness, 0));
        backbuffer.drawString(aircraft.lines[line], aircraft.label.x, aircraft.label.y + lineHeight * line);
    }
}

void AircraftManager::DrawLabelLeader(LGFX_Sprite& backbuffer, const RenderAircraft& aircraft) const
{
    const int closestX = std::max<int>(
        aircraft.label.x,
        std::min<int>(aircraft.x, aircraft.label.x + aircraft.label.width)
    );
    const int closestY = std::max<int>(
        aircraft.label.y,
        std::min<int>(aircraft.y, aircraft.label.y + aircraft.label.height)
    );
    const int dx = closestX - aircraft.x;
    const int dy = closestY - aircraft.y;
    const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));

    // Even a nearby tag gets a very short association line, matching radar
    // data-block conventions without adding measurement/reference graphics.
    constexpr float MARKER_CLEARANCE = 6.0f;
    if (distance <= MARKER_CLEARANCE)
        return;

    const int startX = aircraft.x + static_cast<int>(dx * MARKER_CLEARANCE / distance);
    const int startY = aircraft.y + static_cast<int>(dy * MARKER_CLEARANCE / distance);
    backbuffer.drawLine(startX, startY, closestX, closestY, lgfx::color888(0, 180, 0));
}

void AircraftManager::ResolveNextDestination()
{
    constexpr unsigned long LOOKUP_INTERVAL_MS = 1000;

    if (!displayDestination)
        return;

    const unsigned long now = millis();
    // Placement gets first use of the shared worker whenever it is due.
    // Route lookups can wait; the visual solution should not.
    if (labelLayoutDirty || now - lastLabelLayout >= LABEL_LAYOUT_INTERVAL_MS)
        return;

    if (now - lastDestinationLookup < LOOKUP_INTERVAL_MS)
        return;

    for (auto& [icao, tracked] : trackedAircraft) {
        if (!tracked.visibleOnRadar || tracked.state.onGround ||
            tracked.state.callsign.isEmpty() || tracked.destinationLookupAttempted)
            continue;

        bool hasSafeCallsignCharacter = false;
        for (size_t i = 0; i < tracked.state.callsign.length(); ++i) {
            const char c = tracked.state.callsign[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                hasSafeCallsignCharacter = true;
                break;
            }
        }

        if (!hasSafeCallsignCharacter) {
            tracked.destinationLookupAttempted = true;
            return;
        }

        if (ScheduleNetworkJob(
                NetworkJobType::ResolveDestination,
                icao,
                tracked.state.callsign)) {
            tracked.destinationLookupAttempted = true;
            lastDestinationLookup = now;
        }
        return;
    }
}

void AircraftManager::DrawAircraftRadarVector(
    LGFX_Sprite& backbuffer,
    int x,
    int y,
    const TrackedAircraft& tracked
) const
{
    const float dx = std::sin(radians(tracked.state.trueTrack));
    const float dy = -std::cos(radians(tracked.state.trueTrack));

    // A compact controller-style position block with a forward track vector.
    // Draw the vector first so it visually begins at the edge of the block.
    constexpr float VECTOR_LENGTH = 10.0f;
    const int vectorEndX = x + static_cast<int>(std::round(dx * VECTOR_LENGTH));
    const int vectorEndY = y + static_cast<int>(std::round(dy * VECTOR_LENGTH));
    const auto markerColor = lgfx::color888(0, 255, 0);
    backbuffer.drawLine(x, y, vectorEndX, vectorEndY, markerColor);
    backbuffer.fillRect(x - 2, y - 1, 5, 3, markerColor);
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
