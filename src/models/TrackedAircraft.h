#pragma once

#include "Aircraft.h"

struct TrackedAircraft {
    Aircraft state;
    unsigned long lastSeen;
    String route;
    bool destinationLookupAttempted = false;

    // Label placement is kept relative to the aircraft so small position
    // changes do not make the label jump between otherwise equal choices.
    int16_t labelOffsetX = 0;
    int16_t labelOffsetY = 0;
    bool hasLabelPlacement = false;
    unsigned long lastLabelMove = 0;

    // Network snapshots are revealed when the radar sweep reaches this
    // aircraft instead of changing every target on the same frame.
    Aircraft queuedState{};
    unsigned long queuedStateReceivedAt = 0;
    bool hasQueuedState = false;
    bool queuedRemoval = false;
    bool visibleOnRadar = true;
    float radarDisplayLat = 0.0f;
    float radarDisplayLon = 0.0f;
    bool hasRadarDisplayPosition = false;
    float radarSweepBearing = 0.0f;
    bool hasRadarSweepBearing = false;
    unsigned long lastRadarSweepUpdateAt = 0;

    // blending state
    float blendFromLat = 0.0f;
    float blendFromLon = 0.0f;
    float blendAlpha = 1.0f;  // 1.0 = blend complete, no interpolation active

    unsigned long lastTick = 0;

    // first appearance, no blend needed
    TrackedAircraft(const Aircraft& ac, unsigned long now)
        : state(ac), lastSeen(now),
        blendFromLat(ac.latitude),
        blendFromLon(ac.longitude),
        blendAlpha(1.0f),
        lastTick(now) {
    }

    // subsequent update — blend from current visual position
    void Update(const Aircraft& newState, unsigned long now) {
        // capture visual position at moment of update before switching state
        auto [curLat, curLon] = GetDisplayPosition();
        blendFromLat = curLat;
        blendFromLon = curLon;
        blendAlpha = 0.0f;  // restart blend

        if (state.callsign != newState.callsign) {
            route = "";
            destinationLookupAttempted = false;
        }

        state = newState;
        lastSeen = now;
    }

    void QueueUpdate(const Aircraft& newState, unsigned long now) {
        queuedState = newState;
        queuedStateReceivedAt = now;
        hasQueuedState = true;
        queuedRemoval = false;
    }

    bool ApplyQueuedUpdate() {
        if (!hasQueuedState)
            return false;

        Update(queuedState, queuedStateReceivedAt);
        hasQueuedState = false;
        return true;
    }

    void QueueRemoval() {
        hasQueuedState = false;
        queuedRemoval = true;
    }

    void LatchRadarDisplayPosition() {
        const auto [predictedLat, predictedLon] = PredictPosition();
        radarDisplayLat = predictedLat;
        radarDisplayLon = predictedLon;
        hasRadarDisplayPosition = true;
    }

    std::pair<float, float> GetRadarDisplayPosition() const {
        if (hasRadarDisplayPosition)
            return { radarDisplayLat, radarDisplayLon };
        return GetDisplayPosition();
    }

    void Tick() {
        unsigned long now = millis();
        float deltaSeconds = (now - lastTick) / 1000.0f;
        lastTick = now;

        const float blendSpeed = 0.15f; // lower = slower, higher = faster
        blendAlpha = min(blendAlpha + deltaSeconds * blendSpeed, 1.0f);
    }

    std::pair<float, float> GetDisplayPosition() const {
        auto [deadLat, deadLon] = PredictPosition();

        if (blendAlpha >= 1.0f)
            return { deadLat, deadLon };

        // ease-in-out for smoother feel
        float t = blendAlpha * blendAlpha * (3.0f - 2.0f * blendAlpha);

        return {
            blendFromLat + t * (deadLat - blendFromLat),
            blendFromLon + t * (deadLon - blendFromLon)
        };
    }

    std::pair<float, float> PredictPosition() const {
        float dataAgeOnArrival = 0.0f;
        if (state.timePosition > 0 && state.lastContact > 0)
            dataAgeOnArrival = (float)(state.lastContact - state.timePosition);

        float localElapsed = (millis() - lastSeen) / 1000.0f;
        float dt = localElapsed + dataAgeOnArrival;

        float headingRad = radians(state.trueTrack);
        const float latMetersPerDeg = 111320.0f;
        float deltaLat = (state.velocity * dt * cos(headingRad)) / latMetersPerDeg;
        float deltaLon = (state.velocity * dt * sin(headingRad)) / (latMetersPerDeg * cos(radians(state.latitude)));

        return { state.latitude + deltaLat, state.longitude + deltaLon };
    }
};
