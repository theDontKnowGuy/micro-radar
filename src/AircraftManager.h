#pragma once

#include <atomic>
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
        FetchTimezone,
        ResolveDestination,
        SolveLabels
    };

    enum class ClockFormat : uint8_t {
        TwentyFourHour,
        TwelveHour
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
    bool displayClock = false;
    // Aircraft OpenSky reports as on_ground. Off by default: it is the setting
    // that changes most with where the radar is pointed, and a face centred on
    // an airport fills with parked aircraft the moment it is on.
    bool displayGroundTraffic = false;
    ClockFormat clockFormat = ClockFormat::TwentyFourHour;
    String locationNameLabel;
    AircraftMarkerStyle aircraftMarkerStyle = AircraftMarkerStyle::RadarVector;

    unsigned long fetchInterval = 0;
    unsigned long lastFetch = 0;
    bool hasScheduledFetch = false;

    // Render-loop side of the fetch health below. The message is what the panel
    // draws, empty while the fetches are landing; the deadline holds the next
    // one off after OpenSky has said the day's credits are gone, because
    // carrying on at the normal interval is what keeps a 429 coming.
    String fetchFailureMessage;
    unsigned long fetchBackoffUntil = 0;

    // Read by the label-layout worker, which reserves the notice's region so a
    // data block is not placed under it, and written by the render loop. Atomic
    // for the same reason updateNoticeVisible is.
    std::atomic<bool> fetchNoticeVisible{false};

    // Worker-task side, touched only from RunAircraftFetch and what it calls.
    // The streak decides when the panel speaks up; the other two keep a stuck
    // radar from posting the same line to the dashboard every twenty seconds.
    unsigned int fetchFailureStreak = 0;
    String reportedFetchFailure;
    unsigned long lastFetchFailureReport = 0;
    unsigned long lastWindFetch = 0;
    unsigned long lastWindUpdate = 0;
    bool hasScheduledWindFetch = false;
    String windLabel;

    // The clock keeps system time in UTC and adds this at draw time. newlib on
    // this chip has no timezone database, so an IANA name is of no use to it;
    // the offset for the configured coordinates -- summer time already applied
    // -- is fetched from the same weather API the wind comes from.
    // Written by the render loop when a fetch lands, read by the label-layout
    // worker -- which reserves the clock's region only once there is a time to
    // put in it -- as well as by the draw pass. Atomic for the same reason
    // updateNoticeVisible is. The offset beside it is render-loop only.
    long utcOffsetSeconds = 0;
    std::atomic<bool> hasUtcOffset{false};
    unsigned long lastTimezoneFetch = 0;
    bool hasScheduledTimezoneFetch = false;
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
    // Published by every aircraft fetch, whether or not it produced aircraft --
    // a fetch that failed has something to say and no aircraft to say it with,
    // which is the whole reason an empty face used to be unreadable.
    String completedFetchFailure;
    unsigned int completedFetchFailures = 0;
    unsigned long completedFetchBackoffMs = 0;
    bool fetchStatusReady = false;
    String completedRouteIcao;
    String completedRouteCallsign;
    String completedRoute;
    bool routeLookupReady = false;
    String completedWindLabel;
    bool windFetchReady = false;
    long completedUtcOffsetSeconds = 0;
    bool timezoneFetchReady = false;
    std::vector<RenderAircraft> labelLayoutJobAircraft;
    std::vector<TrackedAircraft> labelLayoutJobTracked;
    std::vector<LabelLayoutResult> completedLabelLayout;
    bool labelLayoutReady = false;

    // Written by the render loop, read by the label-layout worker as well as by
    // the draw pass, so it is atomic rather than a plain bool.
    std::atomic<bool> updateNoticeVisible{false};

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
    void DrawClock(LGFX_Sprite& backbuffer) const;
    void DrawLocationInfo(LGFX_Sprite& backbuffer) const;
    void DrawUpdateNotice(LGFX_Sprite& backbuffer) const;
    void DrawFetchNotice(LGFX_Sprite& backbuffer) const;
    void DrawAircraftRadarVector(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const;
    void DrawAircraftTriangle(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const;
    void ResolveNextDestination();

    // Takes the single background worker if it is idle. On a true return the
    // caller holds networkStateMutex and must hand it back through
    // CommitNetworkJob(); on false nothing is held.
    bool TryClaimNetworkWorker();
    void CommitNetworkJob(NetworkJobType job, const String& icao = "", const String& callsign = "");

    // Runs on the worker task: publishes a finished job's results and marks the
    // worker idle in one critical section, so the render loop can never observe
    // a free worker with its results not yet visible.
    template<typename Publish>
    void PublishNetworkResult(Publish publish)
    {
        if (networkStateMutex == nullptr ||
            xSemaphoreTake(networkStateMutex, portMAX_DELAY) != pdTRUE)
            return;

        publish();
        networkBusy = false;
        xSemaphoreGive(networkStateMutex);
    }

    bool ScheduleNetworkJob(NetworkJobType job, const String& icao = "", const String& callsign = "");
    bool ScheduleLabelLayout(const std::vector<RenderAircraft>& aircraft);
    void ConsumeNetworkResults();
    void RunAircraftFetch();

    // Runs on the worker task at the end of every aircraft fetch. Counts the
    // streak and decides whether this failure is worth another line on the
    // diagnostics dashboard; an empty argument means the fetch worked.
    void ReportFetchStatus(const String& failure);
    void RunWindFetch();
    void RunTimezoneFetch();
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

    // Stops the background network worker for good. Called before a firmware
    // download so the OTA transfer is not competing with an OpenSky fetch for
    // heap -- two TLS sessions at once is exactly the pressure that produced
    // "SSL - Memory allocation failed" on this board. There is no resume,
    // because the only paths out of an install are a reboot or a failure that
    // is followed by one.
    void SuspendNetworkTask();

    // Puts a small "Update available" line above the location name, for the
    // manual-update mode where a found release waits for the owner. Cheap
    // enough to set every frame; aircraft labels route around it once set.
    void ShowUpdateNotice(bool visible) { updateNoticeVisible.store(visible); }

    void Draw(
        LGFX_Sprite& backbuffer,
        float sweepAngle,
        bool sweepEnabled,
        unsigned long sweepPeriodMs
    );
};
