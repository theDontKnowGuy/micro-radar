#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <map>
#include <vector>

#include "models/TrackedAircraft.h"
#include "ConfigurationWebServer.h"
#include "OpenSkyAuthTokenHandler.h"
#include "LGFX.h"

class AircraftManager
{
private:
    enum class NetworkJobType : uint8_t {
        None,
        FetchAircraft,
        FetchWind,
        ResolveDestination,
        SolveLabels
    };

    enum class AircraftMarkerStyle : uint8_t {
        RadarVector,
        Triangle,
        Dot
    };

    struct LabelBox {
        int16_t x = 0;
        int16_t y = 0;
        int16_t width = 0;
        int16_t height = 0;
    };

    struct RenderAircraft {
        TrackedAircraft* tracked = nullptr;
        int16_t x = 0;
        int16_t y = 0;
        String lines[4];
        uint8_t lineCount = 0;
        int16_t callsignWidth = 0;
        LabelBox label;
    };

    struct LabelLayoutResult {
        String icao;
        int16_t offsetX = 0;
        int16_t offsetY = 0;
        unsigned long lastMove = 0;
    };

    double lat = 0.0;
    double lon = 0.0;
    double rad = 0.2;
    std::map<String, TrackedAircraft> trackedAircraft;
    String openskyClientId;
    String openskyClientSecret;

    bool displaySpeed = true;
    bool displaySpeedInKnots = true;
    bool displayAltitude = true;
    bool displayAltitudeInFeet = true;
    bool displayDestination = false;
    bool displayWind = false;
    AircraftMarkerStyle aircraftMarkerStyle = AircraftMarkerStyle::RadarVector;

    unsigned long fetchInterval = 0;
    unsigned long lastFetch = 0;
    bool hasScheduledFetch = false;
    unsigned long lastWindFetch = 0;
    unsigned long lastWindUpdate = 0;
    bool hasScheduledWindFetch = false;
    String windLabel;
    unsigned long lastDestinationLookup = 0;
    unsigned long lastLabelLayout = 0;
    bool labelLayoutDirty = true;
    float previousSweepAngle = 0.0f;
    unsigned long lastSweepFrameAt = 0;
    bool hasPreviousSweepAngle = false;
    bool sweepAppliedAircraftChanges = false;

    SemaphoreHandle_t networkStateMutex = nullptr;
    TaskHandle_t networkTaskHandle = nullptr;
    bool networkBusy = false;
    NetworkJobType networkJob = NetworkJobType::None;
    String networkJobIcao;
    String networkJobCallsign;

    std::vector<Aircraft> completedAircraftFetch;
    bool aircraftFetchReady = false;
    String completedRouteIcao;
    String completedRouteCallsign;
    String completedRoute;
    bool routeLookupReady = false;
    String completedWindLabel;
    bool windFetchReady = false;
    std::vector<RenderAircraft> labelLayoutJobAircraft;
    std::vector<TrackedAircraft> labelLayoutJobTracked;
    std::vector<LabelLayoutResult> completedLabelLayout;
    bool labelLayoutReady = false;

    ConfigurationWebServer& configServer;
    OpenSkyAuthTokenHandler& authHandler;
    HttpRequestManager& http;

    void DrawRadarCircles(LGFX_Sprite& backbuffer) const;
    std::pair<int, int> ProjectCoordinateToScreen(float predLat, float predLon) const;
    void ApplySweepUpdates(float sweepAngle, bool sweepEnabled, unsigned long sweepPeriodMs);
    RenderAircraft BuildRenderAircraft(LGFX_Sprite& backbuffer, int x, int y, TrackedAircraft& tracked) const;
    void PlaceAircraftLabels(std::vector<RenderAircraft>& aircraft);
    void SolveAircraftLabels(std::vector<RenderAircraft>& aircraft);
    void DrawAircraftInfo(LGFX_Sprite& backbuffer, const RenderAircraft& aircraft) const;
    void DrawLabelLeader(LGFX_Sprite& backbuffer, const RenderAircraft& aircraft) const;
    void DrawWindInfo(LGFX_Sprite& backbuffer) const;
    void DrawAircraftRadarVector(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const;
    void DrawAircraftTriangle(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const;
    void ResolveNextDestination();
    bool ScheduleNetworkJob(NetworkJobType job, const String& icao = "", const String& callsign = "");
    bool ScheduleLabelLayout(const std::vector<RenderAircraft>& aircraft);
    void ConsumeNetworkResults();
    void RunAircraftFetch();
    void RunWindFetch();
    void RunDestinationLookup(const String& icao, const String& callsign);
    void NetworkTaskLoop();
    static void NetworkTaskEntry(void* context);

public:
    AircraftManager(ConfigurationWebServer& config, OpenSkyAuthTokenHandler& auth, HttpRequestManager& httpManager)
        : configServer(config), authHandler(auth), http(httpManager)
    {
    }
    ~AircraftManager() = default;

    void Initialise();
    void Update();
    void Draw(
        LGFX_Sprite& backbuffer,
        float sweepAngle,
        bool sweepEnabled,
        unsigned long sweepPeriodMs
    );
};
