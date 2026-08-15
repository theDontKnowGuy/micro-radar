#include "AircraftManager.h"

#include "Diagnostics.h"
#include "DisplayConfig.h" // SCREEN_SIZE / SCREEN_SIZE_DIV_2, per selected panel
#include "ui/PanelTrim.h"

#include <ctime>

constexpr unsigned long LABEL_LAYOUT_INTERVAL_MS = 1000;
constexpr unsigned long WIND_FETCH_INTERVAL_MS = 15UL * 60UL * 1000UL;
constexpr unsigned long WIND_RETRY_INTERVAL_MS = 60UL * 1000UL;
constexpr unsigned long WIND_STALE_INTERVAL_MS = 60UL * 60UL * 1000UL;
// The two rim labels -- the wind report at the top of the face, the location
// name at the bottom. Drawn in a real proportional face rather than the 6x8
// bitmap they used to share with the aircraft blocks, because at 6x8 a stem is
// one pixel wide, and a one-pixel stem is the one thing no amount of care can
// turn by three degrees and leave readable. See PanelTrim; FreeSansBold9pt7b is
// 22 rows tall and puts two pixels in a stem, which survives it.
constexpr int RIM_LABEL_HEIGHT = 22;

// What the label solver keeps clear for one, which has to cover the longest run
// either carries: "Ben Gurion Airport" measures 163 at this size and a full
// wind report 158.
constexpr int RIM_LABEL_WIDTH = 170;

// How far short of the outer range ring a label has to stop. The ring is drawn
// on the outermost row the raster has, so a label measured against the full
// radius is a label touching it -- which is what the first attempt at this did,
// at both ends of the face.
constexpr int RIM_LABEL_CLEARANCE = 6;

constexpr int WIND_LABEL_WIDTH = RIM_LABEL_WIDTH;
constexpr int WIND_LABEL_HEIGHT = RIM_LABEL_HEIGHT;
constexpr int WIND_LABEL_X = (SCREEN_SIZE - WIND_LABEL_WIDTH) / 2;

// Ten rows lower than the old bitmap label sat, because the topmost row of a
// block is the narrowest part of it on a round face and this face is now taller
// and wider. Allowing for the clearance above, y=16 offers 128 pixels against
// the 158 a wind report wants, and y=26 offers 160.
constexpr int WIND_LABEL_Y = 26;

constexpr int LOCATION_LABEL_WIDTH = RIM_LABEL_WIDTH;
constexpr int LOCATION_LABEL_HEIGHT = RIM_LABEL_HEIGHT;
constexpr int LOCATION_LABEL_X = (SCREEN_SIZE - LOCATION_LABEL_WIDTH) / 2;

// Anchored by its bottom row, for the mirror of the reason the wind is anchored
// by its top one. At 344, where the old label ended, the face offers 118 pixels
// against the 163 a full-length name wants; at 331 it offers 164. Everything
// above -- the update notice, and the clock above that -- follows this up the
// face, which is why the clock now sits 24 rows higher than it once did.
constexpr int LOCATION_LABEL_BOTTOM = 331;
constexpr int LOCATION_LABEL_Y = LOCATION_LABEL_BOTTOM - LOCATION_LABEL_HEIGHT;
// Shown only while a firmware release is waiting for the owner to confirm it.
// Sits above the location name rather than below: both panels are round, so the
// rows past the location label fall outside the visible circle.
constexpr int UPDATE_LABEL_WIDTH = 84;
constexpr int UPDATE_LABEL_HEIGHT = 11;
constexpr int UPDATE_LABEL_X = (SCREEN_SIZE - UPDATE_LABEL_WIDTH) / 2;
constexpr int UPDATE_LABEL_Y = LOCATION_LABEL_Y - UPDATE_LABEL_HEIGHT - 3;

// The clock. Drawn in LovyanGFX's seven-segment face, which is 48 rows tall
// with 32-wide digits and a 12-wide colon -- 216 for "00:00:00" -- scaled up
// from there. The scale is applied in 16.16 fixed point by the font renderer,
// so a fractional one is exact rather than rounded to whole pixels.
constexpr unsigned long TIMEZONE_FETCH_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
constexpr unsigned long TIMEZONE_RETRY_INTERVAL_MS = 60UL * 1000UL;
// Change the scale and nothing else: everything below follows from it, and the
// static_asserts further down re-check the placement against the round face at
// whatever size that leaves. Rounded up rather than truncated, so the reserved
// region is never a pixel narrower than what is actually drawn in it. Smaller
// than the two-field clock this replaced -- fitting a third field on the round
// face cost it some size.
constexpr int CLOCK_FONT_HEIGHT = 48;   // Font7, unscaled
constexpr int CLOCK_FONT_WIDTH = 216;   // "00:00:00" in Font7, unscaled
constexpr float CLOCK_TEXT_SCALE = 1.2f;
constexpr int CLOCK_DIGIT_HEIGHT = static_cast<int>(CLOCK_FONT_HEIGHT * CLOCK_TEXT_SCALE) + 1;
constexpr int CLOCK_DIGITS_WIDTH = static_cast<int>(CLOCK_FONT_WIDTH * CLOCK_TEXT_SCALE) + 1;

// The three figures' centres in Font7's unscaled glyph grid -- two digits each,
// either side of a colon -- used to work out which one the sweep beam is over.
// Hours and minutes read the same unscaled either way; hard-coded here rather
// than derived from CLOCK_FONT_WIDTH because they are a property of the glyph
// layout, not of the string's total width.
constexpr int CLOCK_HOUR_CENTRE_UNSCALED = 32;
constexpr int CLOCK_MINUTE_CENTRE_UNSCALED = 108;
constexpr int CLOCK_SECOND_CENTRE_UNSCALED = 184;

// The clock is drawn onto a cleared plate rather than over whatever is already
// on the face -- the label solver only weighs its region, and aircraft markers
// are not placed by the solver at all, so at any moment something can be under
// the digits. The margin is what keeps the two apart: with only the drawn
// extents cleared, a data block ending one pixel outside them still reads as
// touching the time.
constexpr int CLOCK_CLEAR_PADDING = 4;

// The digits are centred on the face; nothing hangs off their side any more,
// so the reserved label is exactly their own box.
constexpr int CLOCK_LABEL_HEIGHT = CLOCK_DIGIT_HEIGHT;
constexpr int CLOCK_LABEL_WIDTH = CLOCK_DIGITS_WIDTH;
constexpr int CLOCK_LABEL_X = (SCREEN_SIZE - CLOCK_DIGITS_WIDTH) / 2;
constexpr int CLOCK_LABEL_Y = UPDATE_LABEL_Y - CLOCK_LABEL_HEIGHT - 6;

// Both constraints on the placement, checked here because neither is visible
// by inspection and one of them only bites some of the time: the digits must
// clear the update notice, which is on screen only while a release is waiting,
// and every corner of the block must fall inside the round face, which crops
// hardest at exactly the rows this sits in. Squared distances rather than a
// radius, so it stays integer arithmetic a compiler can do.
constexpr int CLOCK_FACE_CENTRE = SCREEN_SIZE_DIV_2 - 1;
constexpr int CLOCK_CORNER_DX = CLOCK_LABEL_X + CLOCK_DIGITS_WIDTH - CLOCK_FACE_CENTRE;
constexpr int CLOCK_CORNER_DY = CLOCK_LABEL_Y + CLOCK_LABEL_HEIGHT - CLOCK_FACE_CENTRE;

static_assert(
    CLOCK_LABEL_Y + CLOCK_LABEL_HEIGHT < UPDATE_LABEL_Y,
    "The clock overlaps the update notice; move it up or shrink the digits"
);
static_assert(
    CLOCK_CORNER_DX * CLOCK_CORNER_DX + CLOCK_CORNER_DY * CLOCK_CORNER_DY <
        CLOCK_FACE_CENTRE * CLOCK_FACE_CENTRE,
    "The clock digits fall outside the round face; move them up or shrink them"
);

// Each figure's centre on the backbuffer, in the same frame the corner check
// above uses. Kept as the (dx, dy) offset from CLOCK_FACE_CENTRE rather than
// the raw screen position: that is what atan2 wants, to turn a figure's
// position into the bearing the sweep beam has to cross to update it.
constexpr int CLOCK_HOUR_DX =
    CLOCK_LABEL_X + static_cast<int>(CLOCK_HOUR_CENTRE_UNSCALED * CLOCK_TEXT_SCALE) - CLOCK_FACE_CENTRE;
constexpr int CLOCK_MINUTE_DX =
    CLOCK_LABEL_X + static_cast<int>(CLOCK_MINUTE_CENTRE_UNSCALED * CLOCK_TEXT_SCALE) - CLOCK_FACE_CENTRE;
constexpr int CLOCK_SECOND_DX =
    CLOCK_LABEL_X + static_cast<int>(CLOCK_SECOND_CENTRE_UNSCALED * CLOCK_TEXT_SCALE) - CLOCK_FACE_CENTRE;
constexpr int CLOCK_FIGURE_DY = CLOCK_LABEL_Y + CLOCK_DIGIT_HEIGHT / 2 - CLOCK_FACE_CENTRE;

// Why the face is empty, when it is empty because the radar cannot get the data
// rather than because there is nothing flying. Two lines: what is wrong on top,
// and which part of it underneath, because "no aircraft data" alone does not
// tell an owner whether to check their key, their router or the calendar.
//
// Above the centre rather than stacked under the update notice with everything
// else: the bottom of the face already carries three rows in the space the
// clock does not, and this is the one label that is only ever on screen when the
// radar has nothing else to draw. Sixteen characters of the default 6x8 face
// measure 96 and the longest reason 144, so the width covers both.
constexpr int FETCH_NOTICE_LINE_HEIGHT = 8;   // the default font, at size 1
constexpr int FETCH_NOTICE_LINE_GAP = 4;
constexpr int FETCH_NOTICE_WIDTH = 150;
constexpr int FETCH_NOTICE_HEIGHT = 2 * FETCH_NOTICE_LINE_HEIGHT + FETCH_NOTICE_LINE_GAP;
constexpr int FETCH_NOTICE_X = (SCREEN_SIZE - FETCH_NOTICE_WIDTH) / 2;
constexpr int FETCH_NOTICE_Y = SCREEN_SIZE_DIV_2 - 44;

// How many fetches in a row have to come back with nothing before the panel says
// so. One failure is a router blinking; three at the fetch interval is a minute
// of an empty face, which is long enough that the owner is already wondering.
constexpr unsigned int FETCH_FAILURES_BEFORE_NOTICE = 3;

// The same two constraints the clock is held to, for the same reason: neither is
// visible by inspection, and the rows this sits in are shared with two labels
// that move whenever their own sizing is touched.
static_assert(
    FETCH_NOTICE_Y > WIND_LABEL_Y + WIND_LABEL_HEIGHT,
    "The fetch notice overlaps the wind report; move it down or shrink the rim labels"
);
static_assert(
    FETCH_NOTICE_Y + FETCH_NOTICE_HEIGHT < CLOCK_LABEL_Y - CLOCK_CLEAR_PADDING,
    "The fetch notice runs into the clock plate; move it up or shrink the digits"
);

// Before NTP answers, the system clock reads from the start of 1970. Anything
// earlier than this is a clock that has not been set rather than one that is
// merely wrong, and the radar shows nothing at all rather than a plausible
// time that happens to be false.
constexpr time_t CLOCK_SYNCED_AFTER = 1700000000; // 2023-11-14

// Dimmer than the aircraft green: the clock is furniture, not a target.
constexpr int CLOCK_COLOR_R = 00;
constexpr int CLOCK_COLOR_G = 220;
constexpr int CLOCK_COLOR_B = 0;

#include <ArduinoJson.h>
#include <algorithm>
#include <set>

namespace {

bool IsSuccessful(const HttpResult& result)
{
    return result.success && result.statusCode >= 200 && result.statusCode < 300;
}

// Every fetch on the worker task reported its failures with the same dozen
// lines of Serial.print. One copy, named by what was being fetched.
void LogRequestFailure(const char* what, const HttpResult& result)
{
    Serial.printf("[WARN] %s request failed", what);
    if (result.statusCode != 0)
        Serial.printf(" (HTTP %d)", result.statusCode);
    if (!result.errorMessage.isEmpty())
        Serial.printf(": %s", result.errorMessage.c_str());
    Serial.println();
}

void LogParseFailure(const char* what, const DeserializationError& error, const char* missing)
{
    Serial.printf("[WARN] %s response parse failed: %s\n",
                  what, error ? error.c_str() : missing);
}

// OpenSky sends the ICAO 24-bit address in lower case. Upper case is how it is
// written everywhere it is read as an identifier, and it also stops a six digit
// hex address from being mistaken for a lower case callsign at label size.
// An empty address is left empty: there is nothing truthful to put there, and
// an aircraft without one is not tracked in the first place.
String IcaoDisplayLabel(const String& icao24)
{
    String label = icao24;
    label.toUpperCase();
    return label;
}

// Half the width the round face offers `dy` rows from its centre, less whatever
// clearance the caller wants kept between its text and the rim. Same chord
// StatusScreen works out for its own text, and needed here for the same reason:
// the rim labels sit where the face is narrowing fastest, so what fits is a
// question about their distance from the middle rather than about SCREEN_SIZE.
int FaceHalfWidthAt(int dy, int clearance)
{
    const int radius = CLOCK_FACE_CENTRE - clearance;
    const int squared = radius * radius - dy * dy;
    return squared <= 0 ? 0 : static_cast<int>(sqrtf(static_cast<float>(squared)));
}

// Sets the largest face a rim label can be drawn in without running into the
// curve, measured against the block's outer row -- the top one for the label
// above the centre, the bottom one for the label below it, since that is the
// edge nearer the rim in each case.
//
// A ladder rather than a fixed choice because the location name is whatever its
// owner typed. Eighteen characters of "Ben Gurion Airport" fits the proportional
// face; eighteen of something wider might not, and a name that runs off the
// side of a round screen is worse than a small one that does not.
void SetRimLabelFont(LGFX_Sprite& backbuffer, const char* text, int outerRowY)
{
    backbuffer.setTextSize(1);
    backbuffer.setFont(&fonts::FreeSansBold9pt7b);

    const int available = 2 * FaceHalfWidthAt(abs(outerRowY - CLOCK_FACE_CENTRE),
                                              RIM_LABEL_CLEARANCE);
    if (backbuffer.textWidth(text) <= available)
        return;

    backbuffer.setFont(&fonts::Font0);
}

// Blanks the given block of the face plus a margin all round it, so that what
// is drawn into it afterwards has nothing but background behind or beside it.
// Takes the drawn extents and adds the margin itself: every caller wants the
// same margin, and one that had to add it would also have to remember to take
// it off the origin.
//
// Turned with the digits it carries, and centred on a position turned the same
// way. Both matter: an upright plate under level digits gives the trim away
// wherever a range ring crosses it, and a plate centred where the digits were
// before their own position was turned sits to one side of them.
void ClearClockPlate(LGFX_Sprite& backbuffer, int x, int y, int width, int height)
{
    PanelTrim::FillTurnedPlate(
        backbuffer,
        x + width / 2,
        y + height / 2,
        width + 2 * CLOCK_CLEAR_PADDING,
        height + 2 * CLOCK_CLEAR_PADDING,
        lgfx::color888(0, 0, 0)
    );
}

// The angle from the face centre to one clock figure, normalised to the same
// [0, TWO_PI) range the sweep angle itself is measured in -- see the aircraft
// bearings ApplySweepUpdates works out the same way, just from a fixed offset
// instead of a projected lat/lon.
float ClockFigureBearing(int dx, int dy)
{
    float bearing = std::atan2(static_cast<float>(dy), static_cast<float>(dx));
    if (bearing < 0.0f)
        bearing += TWO_PI;
    return bearing;
}

}  // namespace

void AircraftManager::Initialise()
{
    // get centre point + radius
    lat = configServer.GetStoredString("latitude").toDouble();
    lon = configServer.GetStoredString("longitude").toDouble();
    rad = configServer.GetStoredString("radius").toDouble();

    // ProjectCoordinateToScreen divides by the radius, so a stored zero puts
    // every aircraft at an infinite screen coordinate. /save rejects one now,
    // but a radar configured by firmware that did not could already hold one.
    if (!(rad > 0.0) || rad > 2.5) {
        Serial.println("[WARN] Stored radar radius is unusable; falling back to 1.0 degrees");
        rad = 1.0;
    }

    openskyClientId = configServer.GetStoredString("opensky-id");
    openskyClientSecret = configServer.GetStoredString("opensky-secret");

    // configuration
    const String renderSpeed = configServer.GetStoredString("speed");
    const String speedUnit = configServer.GetStoredString("speed-unit");
    const String renderAltitude = configServer.GetStoredString("altitude");
    const String altitudeUnit = configServer.GetStoredString("altitude-unit");
    const String renderDestination = configServer.GetStoredString("destination");
    const String renderWind = configServer.GetStoredString("wind");
    const String renderGroundTraffic = configServer.GetStoredString("ground-traffic");
    const String renderClock = configServer.GetStoredString("clock");
    const String markerStyle = configServer.GetStoredString("aircraft-marker");
    locationNameLabel = configServer.GetStoredString("location-name");
    locationNameLabel.trim();
    if (locationNameLabel.length() > 18)
        locationNameLabel.remove(18);
    if (!renderSpeed.isEmpty()) displaySpeed = renderSpeed == "true";
    if (!speedUnit.isEmpty()) displaySpeedInKnots = speedUnit == "knots";
    if (!renderAltitude.isEmpty()) displayAltitude = renderAltitude == "true";
    if (!altitudeUnit.isEmpty()) displayAltitudeInFeet = altitudeUnit == "feet" || altitudeUnit == "kft";
    if (!renderDestination.isEmpty()) displayDestination = renderDestination == "true";
    if (!renderWind.isEmpty()) displayWind = renderWind == "true";
    if (!renderGroundTraffic.isEmpty()) displayGroundTraffic = renderGroundTraffic == "true";
    if (!renderClock.isEmpty()) displayClock = renderClock == "true";
    if (markerStyle == "triangle")
        aircraftMarkerStyle = AircraftMarkerStyle::Triangle;
    else if (markerStyle == "dot")
        aircraftMarkerStyle = AircraftMarkerStyle::Dot;
    else
        aircraftMarkerStyle = AircraftMarkerStyle::RadarVector;

    // calculate how often we can call OpenSky API before being rate limited.
    // One allowance to size this against, because credentials are required to
    // get this far -- setup() holds the radar on the configuration screen until
    // a pair has been stored. Nothing is asked of the auth server here either:
    // the interval no longer depends on whether a token can be had right now,
    // which is what used to pin a radar to the slow rate for the rest of its
    // run when the very first token request happened to fail.
    //
    // The reserve is not a rounding allowance. A radar spending 3997 of 4000
    // credits has three left for everything the arithmetic does not model: a
    // reboot, which fetches immediately and then keeps its own clock; a saved
    // setting, which is another reboot; a release installing itself, which is a
    // third. Any two of those in a day and the last fetches before midnight are
    // refused -- which is a blank face for the rest of the evening, from a
    // budget that was only ever notionally full.
    constexpr int MS_PER_DAY = 24 * 60 * 60 * 1000;
    constexpr int AUTHED_CREDITS_PER_DAY = 4000;
    constexpr int CREDIT_RESERVE = 200;

    fetchInterval = MS_PER_DAY / (AUTHED_CREDITS_PER_DAY - CREDIT_RESERVE);

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

    // A radar OpenSky has refused for the day waits rather than carrying on at
    // the normal interval: every request made while the credits are gone is one
    // more that would have counted against them once they came back.
    const bool holdingOffAfterRefusal =
        fetchBackoffUntil != 0 && static_cast<long>(now - fetchBackoffUntil) < 0;

    if ((!hasScheduledFetch || now - lastFetch >= fetchInterval) &&
        !holdingOffAfterRefusal &&
        ScheduleNetworkJob(NetworkJobType::FetchAircraft)) {
        lastFetch = now;
        hasScheduledFetch = true;
    }

    if (displayWind) {
        const unsigned long windInterval =
            windLabel.isEmpty() ? WIND_RETRY_INTERVAL_MS : WIND_FETCH_INTERVAL_MS;
        if ((!hasScheduledWindFetch || now - lastWindFetch >= windInterval) &&
            ScheduleNetworkJob(NetworkJobType::FetchWind)) {
            lastWindFetch = now;
            hasScheduledWindFetch = true;
        }
    }

    // Re-read every few hours rather than once: this is also how the radar
    // finds out that summer time has started or ended where it is pointed.
    if (displayClock) {
        const unsigned long timezoneInterval =
            hasUtcOffset.load() ? TIMEZONE_FETCH_INTERVAL_MS : TIMEZONE_RETRY_INTERVAL_MS;
        if ((!hasScheduledTimezoneFetch || now - lastTimezoneFetch >= timezoneInterval) &&
            ScheduleNetworkJob(NetworkJobType::FetchTimezone)) {
            lastTimezoneFetch = now;
            hasScheduledTimezoneFetch = true;
        }
    }

    ResolveNextDestination();
}

void AircraftManager::SuspendNetworkTask()
{
    if (networkTaskHandle == nullptr)
        return;

    // Suspending can catch the worker mid-request and orphan its socket. That
    // is acceptable here and nowhere else: the caller is about to either reboot
    // into new firmware or reboot after a failed one, so nothing outlives it.
    vTaskSuspend(networkTaskHandle);

    // Stop Update() from queueing further work if it runs again before the
    // reboot -- with the worker suspended those jobs would never be picked up
    // and networkBusy would stay latched.
    if (networkStateMutex != nullptr && xSemaphoreTake(networkStateMutex, portMAX_DELAY) == pdTRUE) {
        networkBusy = true;
        xSemaphoreGive(networkStateMutex);
    }
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

        switch (job) {
            case NetworkJobType::FetchAircraft:
                RunAircraftFetch();
                break;

            case NetworkJobType::FetchWind:
                RunWindFetch();
                break;

            case NetworkJobType::FetchTimezone:
                RunTimezoneFetch();
                break;

            case NetworkJobType::ResolveDestination:
                RunDestinationLookup(icao, callsign);
                break;

            case NetworkJobType::SolveLabels: {
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

                PublishNetworkResult([&] {
                    completedLabelLayout.swap(results);
                    labelLayoutReady = true;
                });
                break;
            }

            case NetworkJobType::None:
                // Woken with nothing to do. Still has to hand the worker back,
                // or the claim taken by the scheduler stays latched forever.
                PublishNetworkResult([] {});
                break;
        }
    }
}

bool AircraftManager::TryClaimNetworkWorker()
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
    return true;
}

void AircraftManager::CommitNetworkJob(NetworkJobType job, const String& icao, const String& callsign)
{
    networkJob = job;
    networkJobIcao = icao;
    networkJobCallsign = callsign;
    xSemaphoreGive(networkStateMutex);
    xTaskNotifyGive(networkTaskHandle);
}

bool AircraftManager::ScheduleNetworkJob(NetworkJobType job, const String& icao, const String& callsign)
{
    if (!TryClaimNetworkWorker())
        return false;

    CommitNetworkJob(job, icao, callsign);
    return true;
}

bool AircraftManager::ScheduleLabelLayout(const std::vector<RenderAircraft>& aircraft)
{
    if (aircraft.empty() || !TryClaimNetworkWorker())
        return false;

    labelLayoutJobAircraft = aircraft;
    labelLayoutJobTracked.clear();
    labelLayoutJobTracked.reserve(aircraft.size());
    for (const auto& current : aircraft)
        labelLayoutJobTracked.push_back(*current.tracked);
    for (size_t i = 0; i < labelLayoutJobAircraft.size(); ++i)
        labelLayoutJobAircraft[i].tracked = &labelLayoutJobTracked[i];

    CommitNetworkJob(NetworkJobType::SolveLabels);
    return true;
}

void AircraftManager::RunAircraftFetch()
{
    // Set by every path below that ends without aircraft, in the words the panel
    // draws. Empty means the fetch worked, which is also how the notice clears.
    String failure;
    unsigned long backoffMs = 0;

    const String token = authHandler.GetValidToken(openskyClientId, openskyClientSecret);

    // No unauthenticated fallback. OpenSky's anonymous allowance is counted per
    // public address rather than per device, so a radar spending it at the
    // authenticated interval would empty it in an hour -- taking every other
    // device behind the same address down with it -- and then be refused for
    // the rest of the day. setup() will not start the radar without credentials
    // stored, so an empty token here means the stored pair was rejected; the
    // token handler is already backing off, and this lets the fetch pass.
    //
    // The reason comes from the token handler rather than being invented here:
    // it is the only thing that knows whether the pair was refused or the auth
    // server could not be reached, and those two want different things done
    // about them.
    if (token.isEmpty()) {
        failure = authHandler.LastError();
        if (failure.isEmpty())
            failure = "OpenSky login failed";

        ReportFetchStatus(failure);
        PublishNetworkResult([&] {
            completedFetchFailure = failure;
            completedFetchFailures = fetchFailureStreak;
            completedFetchBackoffMs = 0;
            fetchStatusReady = true;
        });
        return;
    }

    const std::vector<std::pair<String, String>> headers = {
        { "Authorization", "Bearer " + token }
    };

    // Positions are dead-reckoned between fetches, so an aircraft just outside
    // the face when the snapshot is taken can belong on screen well before the
    // next one arrives. Asking for a margin lets it enter at the rim instead of
    // appearing out of nowhere once it is already inside.
    //
    // Capped at the half-width that keeps the box inside OpenSky's 25 square
    // degree band: past that a request costs 2 credits instead of 1, which
    // would halve the refresh rate to buy a wider margin.
    constexpr double MAX_REQUEST_HALF_WIDTH = 2.5;
    constexpr double REQUEST_MARGIN = 1.15;
    const double requestRad = std::min(rad * REQUEST_MARGIN, MAX_REQUEST_HALF_WIDTH);

    // Six decimals, not String()'s default two. The stored radius has six, and
    // at the smallest selectable radius (3 km) rounding each edge to 0.01
    // degrees asks for a box ~19% narrower per axis than the one being drawn --
    // so the outer ring of the face could never be populated.
    constexpr unsigned int COORDINATE_DECIMALS = 6;
    const HttpResult result = http.Get(
        "https://opensky-network.org/api/states/all",
        {
            {"lamin", String(lat - requestRad, COORDINATE_DECIMALS)},
            {"lamax", String(lat + requestRad, COORDINATE_DECIMALS)},
            {"lomin", String(lon - requestRad, COORDINATE_DECIMALS)},
            {"lomax", String(lon + requestRad, COORDINATE_DECIMALS)}
        },
        headers
    );

    // Long enough that a radar refused for the day is not spending the next
    // day's credits finding that out, short enough that one recovers by itself
    // within the hour of whatever cleared it.
    constexpr unsigned long RATE_LIMIT_BACKOFF_MS = 15UL * 60UL * 1000UL;

    std::vector<Aircraft> fetchedAircraft;
    bool fetchSucceeded = false;
    if (!IsSuccessful(result)) {
        LogRequestFailure("OpenSky API", result);

        // Split by what the owner would have to do about it. A 429 is the one
        // failure the radar itself causes, and the one it can make worse: the
        // credits are gone for the day, and asking again every twenty seconds
        // is how it stays that way.
        if (result.statusCode == 429) {
            failure = "OpenSky quota used up";
            backoffMs = RATE_LIMIT_BACKOFF_MS;
        } else if (result.statusCode == 401 || result.statusCode == 403) {
            failure = "OpenSky login rejected";
        } else if (result.statusCode != 0) {
            char message[32];
            snprintf(message, sizeof(message), "OpenSky error HTTP %d", result.statusCode);
            failure = message;
        } else {
            failure = "OpenSky unreachable";
        }
    } else {
        JsonDocument doc;
        const DeserializationError error = deserializeJson(doc, result.response);
        if (!error && doc["states"].is<JsonArray>()) {
            fetchedAircraft = JsonParser::ParseArray<Aircraft>(doc["states"]);
            fetchSucceeded = true;
        } else {
            LogParseFailure("OpenSky", error, "missing states array");
            failure = "OpenSky sent bad data";
        }
    }

    // OpenSky's box is square (lamin/lamax/lomin/lomax); the face is round and
    // has its own configured radius, and ground traffic can be hidden entirely
    // -- so a fetch that comes back full can still leave most of it undrawn.
    // Run the same round-face + ground-traffic test Draw() applies per marker,
    // once here, so that mismatch shows up in the log without waiting on a
    // render pass to explain it.
    if (fetchSucceeded) {
        constexpr int RADAR_CENTRE = SCREEN_SIZE_DIV_2 - 1;
        constexpr int MAX_MARKER_DISTANCE = SCREEN_SIZE_DIV_2 - 1;
        size_t visibleCount = 0;
        for (const auto& aircraft : fetchedAircraft) {
            const bool groundHidden = aircraft.onGround && !displayGroundTraffic;
            bool inRange = false;
            if (!groundHidden) {
                const auto [x, y] = ProjectCoordinateToScreen(aircraft.latitude, aircraft.longitude);
                const int dx = x - RADAR_CENTRE;
                const int dy = y - RADAR_CENTRE;
                inRange = dx * dx + dy * dy <= MAX_MARKER_DISTANCE * MAX_MARKER_DISTANCE;
            }
            if (inRange)
                ++visibleCount;

            const char* callsign = aircraft.callsign.isEmpty() ? "----" : aircraft.callsign.c_str();
            const char* status = groundHidden ? "ground, hidden" : (inRange ? "in range" : "outside display");
            Serial.printf("[INFO]   %s %-8s %s\n",
                          IcaoDisplayLabel(aircraft.icao24).c_str(), callsign, status);
        }
        Serial.printf("[INFO] OpenSky returned %u aircraft, %u within the round display\n",
                      static_cast<unsigned>(fetchedAircraft.size()),
                      static_cast<unsigned>(visibleCount));
    }

    ReportFetchStatus(failure);
    PublishNetworkResult([&] {
        if (fetchSucceeded) {
            completedAircraftFetch.swap(fetchedAircraft);
            aircraftFetchReady = true;
        }
        completedFetchFailure = failure;
        completedFetchFailures = fetchFailureStreak;
        completedFetchBackoffMs = backoffMs;
        fetchStatusReady = true;
    });
}

void AircraftManager::ReportFetchStatus(const String& failure)
{
    // The dashboard is not a log tail. A radar with a rejected key fails every
    // fetch for as long as it is switched on, and posting that at the fetch
    // interval would bury everything else it has to say -- so a failure is
    // reported when it starts, when it changes into a different one, and then
    // once an hour for as long as it lasts.
    constexpr unsigned long REPEAT_REPORT_INTERVAL_MS = 60UL * 60UL * 1000UL;
    const unsigned long now = millis();

    if (failure.isEmpty()) {
        // Worth an event rather than a warning, and worth sending at all: on the
        // dashboard this is the line that says when a unit came back, which is
        // the other half of knowing when it went away.
        if (fetchFailureStreak >= FETCH_FAILURES_BEFORE_NOTICE)
            Diagnostics::Event("aircraft", "fetches recovered after %u failures",
                               fetchFailureStreak);
        fetchFailureStreak = 0;
        reportedFetchFailure = "";
        return;
    }

    ++fetchFailureStreak;

    // Unsigned subtraction, like every other interval on this class: it is the
    // form that stays correct across the millis() wrap.
    if (failure == reportedFetchFailure &&
        now - lastFetchFailureReport < REPEAT_REPORT_INTERVAL_MS)
        return;

    Diagnostics::Warn("no aircraft data: %s (%u fetches in a row)",
                      failure.c_str(),
                      fetchFailureStreak);
    reportedFetchFailure = failure;
    lastFetchFailureReport = now;
}

void AircraftManager::RunWindFetch()
{
    const HttpResult result = http.Get(
        "https://api.open-meteo.com/v1/forecast",
        {
            {"latitude", String(lat, 6)},
            {"longitude", String(lon, 6)},
            {"current", "wind_speed_10m,wind_direction_10m,wind_gusts_10m"},
            {"wind_speed_unit", "kn"}
        }
    );

    String fetchedWindLabel;
    if (!IsSuccessful(result)) {
        LogRequestFailure("Wind API", result);
    } else {
        JsonDocument doc;
        const DeserializationError error = deserializeJson(doc, result.response);
        const JsonVariant currentWind = doc["current"];
        const JsonVariant speedValue = currentWind["wind_speed_10m"];
        const JsonVariant directionValue = currentWind["wind_direction_10m"];
        const JsonVariant gustValue = currentWind["wind_gusts_10m"];

        if (!error && !currentWind.isNull() &&
            !speedValue.isNull() && !directionValue.isNull()) {
            const float speedValueKnots = speedValue.as<float>();
            const float directionValueDegrees = directionValue.as<float>();
            const float gustValueKnots = gustValue.isNull()
                ? speedValueKnots
                : gustValue.as<float>();

            const int speedKnots = std::clamp(
                static_cast<int>(std::lround(speedValueKnots)),
                0,
                999
            );
            const int gustKnots = std::clamp(
                static_cast<int>(std::lround(gustValueKnots)),
                0,
                999
            );

            char label[32];
            if (speedKnots == 0) {
                snprintf(label, sizeof(label), "WND 00000KT");
            } else {
                int directionDegrees =
                    static_cast<int>(std::lround(directionValueDegrees / 10.0f)) * 10;
                directionDegrees %= 360;
                if (directionDegrees <= 0)
                    directionDegrees += 360;

                if (gustKnots >= speedKnots + 10) {
                    snprintf(
                        label,
                        sizeof(label),
                        "WND %03d%02dG%02dKT",
                        directionDegrees,
                        speedKnots,
                        gustKnots
                    );
                } else {
                    snprintf(
                        label,
                        sizeof(label),
                        "WND %03d%02dKT",
                        directionDegrees,
                        speedKnots
                    );
                }
            }
            fetchedWindLabel = label;
        } else {
            LogParseFailure("Wind", error, "missing current wind fields");
        }
    }

    PublishNetworkResult([&] {
        if (!fetchedWindLabel.isEmpty()) {
            completedWindLabel = fetchedWindLabel;
            windFetchReady = true;
        }
    });
}

void AircraftManager::RunTimezoneFetch()
{
    // The same host the wind comes from, asked for the one field the clock
    // needs. `timezone=auto` is what makes the answer the zone the coordinates
    // fall in; `current=is_day` is the smallest reading that will persuade the
    // API to answer at all. The offset is for right now, so summer time is
    // already in it -- there is nothing here for the firmware to work out.
    const HttpResult result = http.Get(
        "https://api.open-meteo.com/v1/forecast",
        {
            {"latitude", String(lat, 6)},
            {"longitude", String(lon, 6)},
            {"current", "is_day"},
            {"timezone", "auto"}
        }
    );

    bool fetchedOffset = false;
    long offsetSeconds = 0;
    if (!IsSuccessful(result)) {
        LogRequestFailure("Timezone API", result);
    } else {
        JsonDocument doc;
        const DeserializationError error = deserializeJson(doc, result.response);
        const JsonVariant offsetValue = doc["utc_offset_seconds"];

        // A whole-day offset either way covers every real zone; anything
        // outside it is a malformed answer rather than a remote island.
        if (!error && !offsetValue.isNull()) {
            const long candidate = offsetValue.as<long>();
            if (candidate > -86400 && candidate < 86400) {
                offsetSeconds = candidate;
                fetchedOffset = true;
            }
        }

        if (!fetchedOffset)
            LogParseFailure("Timezone", error, "missing or implausible utc_offset_seconds");
    }

    PublishNetworkResult([&] {
        if (fetchedOffset) {
            completedUtcOffsetSeconds = offsetSeconds;
            timezoneFetchReady = true;
        }
    });
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
        if (IsSuccessful(result)) {
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

    PublishNetworkResult([&] {
        completedRouteIcao = icao;
        completedRouteCallsign = callsign;
        completedRoute = route;
        routeLookupReady = true;
    });
}

void AircraftManager::ConsumeNetworkResults()
{
    std::vector<Aircraft> fetchedAircraft;
    bool hasAircraftFetch = false;
    String fetchFailure;
    unsigned int fetchFailures = 0;
    unsigned long fetchBackoffMs = 0;
    bool hasFetchStatus = false;
    String routeIcao;
    String routeCallsign;
    String route;
    bool hasRouteLookup = false;
    String fetchedWindLabel;
    bool hasWindFetch = false;
    long fetchedUtcOffsetSeconds = 0;
    bool hasTimezoneFetch = false;
    std::vector<LabelLayoutResult> labelLayout;
    bool hasLabelLayout = false;

    if (networkStateMutex != nullptr &&
        xSemaphoreTake(networkStateMutex, 0) == pdTRUE) {
        if (aircraftFetchReady) {
            fetchedAircraft.swap(completedAircraftFetch);
            aircraftFetchReady = false;
            hasAircraftFetch = true;
        }
        if (fetchStatusReady) {
            fetchFailure = completedFetchFailure;
            fetchFailures = completedFetchFailures;
            fetchBackoffMs = completedFetchBackoffMs;
            fetchStatusReady = false;
            hasFetchStatus = true;
        }
        if (routeLookupReady) {
            routeIcao = completedRouteIcao;
            routeCallsign = completedRouteCallsign;
            route = completedRoute;
            routeLookupReady = false;
            hasRouteLookup = true;
        }
        if (windFetchReady) {
            fetchedWindLabel = completedWindLabel;
            completedWindLabel = "";
            windFetchReady = false;
            hasWindFetch = true;
        }
        if (timezoneFetchReady) {
            fetchedUtcOffsetSeconds = completedUtcOffsetSeconds;
            timezoneFetchReady = false;
            hasTimezoneFetch = true;
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
        std::set<String> presentIcaos;
        for (auto& aircraft : fetchedAircraft) {
            presentIcaos.insert(aircraft.icao24);

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

        // Looked up rather than rescanned: the previous pass walked the whole
        // fetch for every tracked aircraft, which is quadratic in the busiest
        // airspace -- exactly where the render loop has least time to spare.
        for (auto& [icao, tracked] : trackedAircraft)
            if (presentIcaos.count(icao) == 0)
                tracked.QueueRemoval();
    }

    if (hasFetchStatus) {
        // The panel stays quiet through a single dropped fetch -- the previous
        // aircraft are still on the face and still being dead-reckoned, so
        // saying anything at that point would be wrong more often than right.
        const String notice = fetchFailures >= FETCH_FAILURES_BEFORE_NOTICE ? fetchFailure : String();
        if (notice != fetchFailureMessage) {
            fetchFailureMessage = notice;
            fetchNoticeVisible.store(!fetchFailureMessage.isEmpty());
            // The notice is a region the label solver has to route around, so
            // its appearing or clearing is a reason to solve again.
            labelLayoutDirty = true;
        }

        if (fetchBackoffMs == 0) {
            fetchBackoffUntil = 0;
        } else {
            // Zero is "not backing off", so a deadline that lands there on the
            // millis() wrap is moved a millisecond rather than read as none.
            fetchBackoffUntil = millis() + fetchBackoffMs;
            if (fetchBackoffUntil == 0)
                fetchBackoffUntil = 1;
        }
    }

    if (hasRouteLookup && !route.isEmpty()) {
        auto tracked = trackedAircraft.find(routeIcao);
        if (tracked != trackedAircraft.end() && tracked->second.state.callsign == routeCallsign) {
            tracked->second.route = route;
            labelLayoutDirty = true;
        }
    }

    if (hasWindFetch) {
        windLabel = fetchedWindLabel;
        lastWindUpdate = millis();
    }

    if (hasTimezoneFetch) {
        utcOffsetSeconds = fetchedUtcOffsetSeconds;
        // The clock is a reserved region for the label solver, and it only
        // becomes one once there is a time to show.
        if (!hasUtcOffset.load()) {
            hasUtcOffset.store(true);
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

void AircraftManager::DrawBackground(LGFX_Sprite& backbuffer, float sweepAngle, bool sweepEnabled)
{
    DrawRadarCircles(backbuffer);
    ApplyClockSweepUpdate(sweepAngle, sweepEnabled);
    DrawClock(backbuffer);
}

void AircraftManager::Draw(
    LGFX_Sprite& backbuffer,
    float sweepAngle,
    bool sweepEnabled,
    unsigned long sweepPeriodMs
)
{
    ApplySweepUpdates(sweepAngle, sweepEnabled, sweepPeriodMs);

    backbuffer.setTextSize(1);

    // A 240 px display cannot present an unbounded number of readable data
    // blocks. Keeping a small, fixed label working set also bounds the heap
    // used by RenderAircraft copies and the label solver in dense airspace.
    // Markers for aircraft outside this set are still drawn below.
    constexpr size_t MAX_LABELED_AIRCRAFT = 24;
    std::vector<RenderAircraft> renderAircraft;
    renderAircraft.reserve(std::min(trackedAircraft.size(), MAX_LABELED_AIRCRAFT));

    constexpr int RADAR_CENTRE = SCREEN_SIZE_DIV_2 - 1;
    // The cutoff is the face itself, not the face less a marker's width. Insetting
    // it by the marker radius kept whole glyphs inside the rim, but it also meant
    // the outer 11 px of the radar -- 6% of the configured radius, and the ring
    // an arriving aircraft crosses first -- could never hold a target. A marker
    // straddling the rim is clipped by the sprite, which is what a radar looks
    // like; an aircraft that silently pops into being a third of the way in is not.
    const int maxMarkerDistance = SCREEN_SIZE_DIV_2 - 1;
    auto getVisibleMarkerPosition = [&](const TrackedAircraft& tracked, int& x, int& y) {
        if (!tracked.visibleOnRadar || (tracked.state.onGround && !displayGroundTraffic))
            return false;

        const auto [predLat, predLon] = sweepEnabled
            ? tracked.GetRadarDisplayPosition()
            : tracked.GetDisplayPosition();
        const auto projected = ProjectCoordinateToScreen(predLat, predLon);
        x = projected.first;
        y = projected.second;

        const int markerDx = x - RADAR_CENTRE;
        const int markerDy = y - RADAR_CENTRE;
        return markerDx * markerDx + markerDy * markerDy <=
            maxMarkerDistance * maxMarkerDistance;
    };

    for (auto& [icao, tracked] : trackedAircraft) {
        if (!tracked.visibleOnRadar)
            continue;

        // Ticked before the marker position is read: Tick() is what advances
        // the blend towards a newly arrived position, and the marker loop
        // below has no non-const access to do it. Doing this after the read
        // left taxiing aircraft parked at wherever they were one update ago.
        tracked.Tick();
        int x = 0;
        int y = 0;
        if (!getVisibleMarkerPosition(tracked, x, y))
            continue;

        const int markerDx = x - RADAR_CENTRE;
        const int markerDy = y - RADAR_CENTRE;
        const int distanceSquared = markerDx * markerDx + markerDy * markerDy;

        if (renderAircraft.size() < MAX_LABELED_AIRCRAFT) {
            renderAircraft.push_back(BuildRenderAircraft(backbuffer, x, y, tracked));
            continue;
        }

        // Prefer labels nearest the radar centre. They are the most relevant
        // targets and have the least room for labels outside the round panel.
        auto farthest = std::max_element(
            renderAircraft.begin(),
            renderAircraft.end(),
            [](const RenderAircraft& first, const RenderAircraft& second) {
                const int firstDx = first.x - RADAR_CENTRE;
                const int firstDy = first.y - RADAR_CENTRE;
                const int secondDx = second.x - RADAR_CENTRE;
                const int secondDy = second.y - RADAR_CENTRE;
                return firstDx * firstDx + firstDy * firstDy <
                    secondDx * secondDx + secondDy * secondDy;
            }
        );
        const int farthestDx = farthest->x - RADAR_CENTRE;
        const int farthestDy = farthest->y - RADAR_CENTRE;
        if (distanceSquared < farthestDx * farthestDx + farthestDy * farthestDy)
            *farthest = BuildRenderAircraft(backbuffer, x, y, tracked);
    }

    PlaceAircraftLabels(renderAircraft);

    // Leaders go below text and aircraft symbols. This keeps a displaced label
    // associated with its marker without obscuring either one.
    for (const auto& aircraft : renderAircraft)
        DrawLabelLeader(backbuffer, aircraft);

    for (const auto& aircraft : renderAircraft)
        DrawAircraftInfo(backbuffer, aircraft);

    // Draw every in-range aircraft marker, including targets whose data blocks
    // were omitted from the bounded label set.
    for (const auto& [icao, tracked] : trackedAircraft) {
        int x = 0;
        int y = 0;
        if (!getVisibleMarkerPosition(tracked, x, y))
            continue;

        // Ground traffic gets one flat pip regardless of the chosen style. It
        // has no meaningful track to point a vector at while it is manoeuvring
        // on a taxiway, and drawing it dimmer than the airborne green keeps an
        // airport from reading as the busiest patch of sky on the display.
        if (tracked.state.onGround) {
            backbuffer.fillCircle(x, y, 2, lgfx::color888(0, 90, 0));
            continue;
        }

        switch (aircraftMarkerStyle) {
            case AircraftMarkerStyle::RadarVector:
                DrawAircraftRadarVector(backbuffer, x, y, tracked);
                break;
            case AircraftMarkerStyle::Triangle:
                DrawAircraftTriangle(backbuffer, x, y, tracked);
                break;
            case AircraftMarkerStyle::Dot:
                backbuffer.fillCircle(x, y, 3, lgfx::color888(0, 255, 0));
                break;
        }
    }

    DrawWindInfo(backbuffer);
    DrawLocationInfo(backbuffer);
    DrawUpdateNotice(backbuffer);
    DrawFetchNotice(backbuffer);
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
    // The callsign is a free-text Flight ID the crew types in, and plenty of
    // aircraft transmit nothing at all in it -- state operators and parked
    // airliners especially. Line 0 is the one the label solver protects from
    // overlap, so leaving it empty spends that protection on a blank row and
    // hangs the remaining lines off a marker with nothing naming it. The ICAO
    // 24-bit address is always present and unique to the airframe, so it stands
    // in: an identifier that is never wrong, rather than no identifier at all.
    result.lines[result.lineCount++] = tracked.state.callsign.isEmpty()
        ? IcaoDisplayLabel(tracked.state.icao24)
        : tracked.state.callsign;

    // A parked or taxiing aircraft's reported speed and barometric altitude are
    // ground noise, not flight data -- a data block for one shows only its
    // identity and route, the two lines that are still true while it sits at
    // a gate.
    if (!tracked.state.onGround && displaySpeed && result.lineCount < 4) {
        String speedLabel;
        if (displaySpeedInKnots) {
            const float speedKnots = tracked.state.velocity * 1.94384f;
            speedLabel = String(speedKnots, 0) + "kt";
        } else {
            speedLabel = String(tracked.state.velocity, 1) + "m/s";
        }
        result.lines[result.lineCount++] = speedLabel;
    }

    if (!tracked.state.onGround && displayAltitude && result.lineCount < 4) {
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

    // Discrete candidate local search: iterated coordinate descent followed by
    // pairwise 2-opt repair for obstructed or ownership-swapped label pairs.
    const unsigned long now = millis();

    constexpr int LABEL_GAP = 8;
    const int MARKER_RADIUS = aircraftMarkerStyle == AircraftMarkerStyle::RadarVector ? 11 : 7;
    constexpr int RADAR_CENTRE = SCREEN_SIZE_DIV_2 - 1;
    constexpr int USABLE_RADIUS = SCREEN_SIZE_DIV_2 - 3;
    constexpr uint8_t MAX_CANDIDATES = 33;
    constexpr uint8_t GLOBAL_PASSES = 4;
    constexpr uint8_t PAIR_REPAIR_PASSES = 2;
    // Squared distance makes the far ring progressively more expensive. A
    // pair of far leaders still costs less than a real crossing, but a small
    // overlap in secondary text no longer sends a label across the screen.
    constexpr int64_t LEADER_DISTANCE_WEIGHT = 64;
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

    // Screen containment and readable callsigns are hard priorities. The
    // remaining visual defects share a weighted score so a tiny secondary
    // overlap cannot force a disproportionately long leader.
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

        int64_t VisualPenalty() const {
            return
                leaderMarkerCrossings * 900000 +
                leaderCrossings * 750000 +
                leaderLabelCrossings * 550000 +
                markerConflicts * 500000 +
                markerOverlapArea * 1200 +
                labelConflicts * 90000 +
                labelOverlapArea * 350 +
                ownershipAmbiguity * 48 +
                preference;
        }

        bool IsBetterThan(const LayoutCost& other) const {
            if (outside != other.outside) return outside < other.outside;
            if (callsignConflicts != other.callsignConflicts) return callsignConflicts < other.callsignConflicts;
            if (callsignOverlapArea != other.callsignOverlapArea) return callsignOverlapArea < other.callsignOverlapArea;
            const int64_t visualPenalty = VisualPenalty();
            const int64_t otherVisualPenalty = other.VisualPenalty();
            if (visualPenalty != otherVisualPenalty) return visualPenalty < otherVisualPenalty;
            if (leaderMarkerCrossings != other.leaderMarkerCrossings) return leaderMarkerCrossings < other.leaderMarkerCrossings;
            if (leaderCrossings != other.leaderCrossings) return leaderCrossings < other.leaderCrossings;
            if (leaderLabelCrossings != other.leaderLabelCrossings) return leaderLabelCrossings < other.leaderLabelCrossings;
            if (markerConflicts != other.markerConflicts) return markerConflicts < other.markerConflicts;
            if (markerOverlapArea != other.markerOverlapArea) return markerOverlapArea < other.markerOverlapArea;
            if (labelConflicts != other.labelConflicts) return labelConflicts < other.labelConflicts;
            if (labelOverlapArea != other.labelOverlapArea) return labelOverlapArea < other.labelOverlapArea;
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

    // The pair-repair search can run for several seconds in dense airspace.
    // radar-network has a higher priority than IDLE0, so remaining runnable
    // throughout that search prevents IDLE0 from servicing the task watchdog.
    // Periodically block for one tick instead of merely yielding: taskYIELD()
    // only offers the CPU to tasks at the same priority.
    constexpr unsigned long IDLE_SERVICE_INTERVAL_MS = 10;
    constexpr uint16_t WORK_ITEMS_PER_IDLE_CHECK = 128;
    unsigned long lastIdleServiceAt = millis();
    uint16_t workItemsSinceIdleCheck = 0;
    auto serviceIdleTask = [&]() {
        if (++workItemsSinceIdleCheck < WORK_ITEMS_PER_IDLE_CHECK)
            return;

        workItemsSinceIdleCheck = 0;
        const unsigned long currentTime = millis();
        if (currentTime - lastIdleServiceAt < IDLE_SERVICE_INTERVAL_MS)
            return;

        vTaskDelay(1);
        lastIdleServiceAt = millis();
    };

    static constexpr int8_t DIRECTIONS[8][2] = {
        { 1, 0 }, { -1, 0 }, { 0, -1 }, { 0, 1 },
        { 1, -1 }, { -1, -1 }, { 1, 1 }, { -1, 1 }
    };
    static constexpr int8_t TANGENTIAL_SHIFT[2] = { 14, 28 };
    constexpr int RADIAL_FALLBACK_EXTRA = 18;

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

        auto addDirectionalCandidate = [&](int horizontal, int vertical, int extra) {
            int left = current.x - current.label.width / 2;
            int top = current.y - current.label.height / 2;

            if (horizontal > 0)
                left = current.x + LABEL_GAP + extra;
            else if (horizontal < 0)
                left = current.x - LABEL_GAP - extra - current.label.width;

            if (vertical > 0)
                top = current.y + LABEL_GAP + extra;
            else if (vertical < 0)
                top = current.y - LABEL_GAP - extra - current.label.height;

            addCandidate(left, top);
        };

        // Start with the eight immediately adjacent positions.
        for (const auto& direction : DIRECTIONS)
            addDirectionalCandidate(direction[0], direction[1], 0);

        // Slide along each side while retaining the minimum radial gap. These
        // candidates separate crowded edge labels without long diagonal
        // leaders; for most multi-line labels the leader remains only 8 px.
        const int centredLeft = current.x - current.label.width / 2;
        const int centredTop = current.y - current.label.height / 2;
        for (const int shift : TANGENTIAL_SHIFT) {
            addCandidate(current.x + LABEL_GAP, centredTop - shift);
            addCandidate(current.x + LABEL_GAP, centredTop + shift);
            addCandidate(current.x - LABEL_GAP - current.label.width, centredTop - shift);
            addCandidate(current.x - LABEL_GAP - current.label.width, centredTop + shift);
            addCandidate(centredLeft - shift, current.y - LABEL_GAP - current.label.height);
            addCandidate(centredLeft + shift, current.y - LABEL_GAP - current.label.height);
            addCandidate(centredLeft - shift, current.y + LABEL_GAP);
            addCandidate(centredLeft + shift, current.y + LABEL_GAP);
        }

        // Keep one modest radial ring as a fallback for genuinely blocked
        // clusters. It replaces the old 30 px far ring.
        for (const auto& direction : DIRECTIONS)
            addDirectionalCandidate(
                direction[0],
                direction[1],
                RADIAL_FALLBACK_EXTRA
            );

        current.label = current.tracked->hasLabelPlacement ? previous : set.boxes[0];
    }

    auto unaryCost = [&](size_t index, const LabelBox& candidate) {
        serviceIdleTask();

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

        auto addReservedLabelCost = [&](const LabelBox& reservedBox) {
            const int reservedCallsignArea = overlapArea(
                callsignBox(current, candidate),
                reservedBox
            );
            if (reservedCallsignArea > 0)
                ++cost.callsignConflicts;
            cost.callsignOverlapArea += reservedCallsignArea;

            const int reservedLabelArea = overlapArea(candidate, reservedBox);
            if (reservedLabelArea > 0)
                ++cost.labelConflicts;
            cost.labelOverlapArea += reservedLabelArea;

            if (segmentIntersectsBox(leader, reservedBox))
                ++cost.leaderLabelCrossings;
        };

        if (displayWind) {
            const LabelBox windBox = {
                WIND_LABEL_X,
                WIND_LABEL_Y,
                WIND_LABEL_WIDTH,
                WIND_LABEL_HEIGHT
            };
            addReservedLabelCost(windBox);
        }

        if (displayClock && hasUtcOffset.load()) {
            // The margin is reserved along with the digits. Anything the solver
            // still puts here is going to be cut by the plate when the clock
            // draws over it, and a label chopped down the middle is worse than
            // one moved: better that the cost sees the whole plate.
            const LabelBox clockBox = {
                CLOCK_LABEL_X - CLOCK_CLEAR_PADDING,
                CLOCK_LABEL_Y - CLOCK_CLEAR_PADDING,
                CLOCK_LABEL_WIDTH + 2 * CLOCK_CLEAR_PADDING,
                CLOCK_LABEL_HEIGHT + 2 * CLOCK_CLEAR_PADDING
            };
            addReservedLabelCost(clockBox);
        }

        if (!locationNameLabel.isEmpty()) {
            const LabelBox locationBox = {
                LOCATION_LABEL_X,
                LOCATION_LABEL_Y,
                LOCATION_LABEL_WIDTH,
                LOCATION_LABEL_HEIGHT
            };
            addReservedLabelCost(locationBox);
        }

        if (updateNoticeVisible) {
            const LabelBox updateBox = {
                UPDATE_LABEL_X,
                UPDATE_LABEL_Y,
                UPDATE_LABEL_WIDTH,
                UPDATE_LABEL_HEIGHT
            };
            addReservedLabelCost(updateBox);
        }

        // Reserved even though the face is usually empty when this is up: a
        // radar whose fetches have started failing still has its last set of
        // aircraft on screen, dead-reckoned, and they are exactly what the
        // notice explaining their staleness should not be drawn on top of.
        if (fetchNoticeVisible) {
            const LabelBox fetchBox = {
                FETCH_NOTICE_X,
                FETCH_NOTICE_Y,
                FETCH_NOTICE_WIDTH,
                FETCH_NOTICE_HEIGHT
            };
            addReservedLabelCost(fetchBox);
        }

        for (size_t otherIndex = 0; otherIndex < aircraft.size(); ++otherIndex) {
            if (otherIndex == index)
                continue;

            const auto& other = aircraft[otherIndex];
            const LabelBox marker = markerBox(other);
            const int markerArea = overlapArea(candidate, marker);
            if (markerArea > 0) {
                ++cost.markerConflicts;
                cost.markerOverlapArea += markerArea;
            }
            if (segmentIntersectsBox(leader, marker))
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
        serviceIdleTask();

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

    // An obstructed pair can be a 2-opt trap: moving either label alone is
    // worse because it temporarily occupies the other's slot. Search both
    // labels' complete candidate sets and commit the repair atomically.
    for (uint8_t repairPass = 0; repairPass < PAIR_REPAIR_PASSES; ++repairPass) {
        bool repaired = false;
        for (size_t firstIndex = 0; firstIndex < aircraft.size(); ++firstIndex) {
            for (size_t secondIndex = firstIndex + 1; secondIndex < aircraft.size(); ++secondIndex) {
                const Segment firstLeader = makeLeader(aircraft[firstIndex], aircraft[firstIndex].label);
                const Segment secondLeader = makeLeader(aircraft[secondIndex], aircraft[secondIndex].label);
                const bool connectionIsObstructed =
                    segmentsIntersect(firstLeader, secondLeader) ||
                    segmentIntersectsBox(firstLeader, markerBox(aircraft[secondIndex])) ||
                    segmentIntersectsBox(secondLeader, markerBox(aircraft[firstIndex])) ||
                    segmentIntersectsBox(firstLeader, aircraft[secondIndex].label) ||
                    segmentIntersectsBox(secondLeader, aircraft[firstIndex].label);
                const bool ownershipIsSwapped =
                    centreDistanceSquared(aircraft[firstIndex].label, aircraft[secondIndex]) + 64 <
                        centreDistanceSquared(aircraft[firstIndex].label, aircraft[firstIndex]) &&
                    centreDistanceSquared(aircraft[secondIndex].label, aircraft[firstIndex]) + 64 <
                        centreDistanceSquared(aircraft[secondIndex].label, aircraft[secondIndex]);
                if (!connectionIsObstructed && !ownershipIsSwapped)
                    continue;

                LabelBox bestFirst = aircraft[firstIndex].label;
                LabelBox bestSecond = aircraft[secondIndex].label;
                LayoutCost bestCost = pairCost(firstIndex, bestFirst, secondIndex, bestSecond);

                const uint8_t firstCount = candidates[firstIndex].count;
                const uint8_t secondCount = candidates[secondIndex].count;
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

void AircraftManager::DrawWindInfo(LGFX_Sprite& backbuffer) const
{
    if (!displayWind || windLabel.isEmpty() || lastWindUpdate == 0 ||
        millis() - lastWindUpdate >= WIND_STALE_INTERVAL_MS)
        return;

    // Measured against its top row: this block sits above the centre, so that
    // is the edge nearer the rim and the one the curve crops first.
    SetRimLabelFont(backbuffer, windLabel.c_str(), WIND_LABEL_Y);
    backbuffer.setTextColor(lgfx::color888(0, 150, 0));
    PanelTrim::DrawTurnedText(
        backbuffer,
        PanelTrim::TextSlot::Wind,
        windLabel.c_str(),
        SCREEN_SIZE_DIV_2,
        WIND_LABEL_Y + WIND_LABEL_HEIGHT / 2
    );

    // Everything else on the face draws in the default face at the default
    // scale, and both are state on the sprite.
    backbuffer.setFont(&fonts::Font0);
    backbuffer.setTextSize(1);
}

void AircraftManager::ApplyClockSweepUpdate(float sweepAngle, bool sweepEnabled)
{
    if (!displayClock || !hasUtcOffset.load()) {
        hasPreviousClockSweepAngle = false;
        hasLatchedClock = false;
        return;
    }

    const time_t utcNow = time(nullptr);
    if (utcNow < CLOCK_SYNCED_AFTER) {
        hasPreviousClockSweepAngle = false;
        hasLatchedClock = false;
        return;
    }

    // gmtime_r on a shifted timestamp rather than localtime_r on a real one:
    // the C library's idea of local time is whatever is in TZ, and TZ cannot
    // name the zone the radar is pointed at without a database this chip does
    // not carry.
    const time_t localNow = utcNow + utcOffsetSeconds;
    struct tm parts = {};
    gmtime_r(&localNow, &parts);

    auto latchAll = [&]() {
        latchedClockHour = parts.tm_hour;
        latchedClockMinute = parts.tm_min;
        latchedClockSecond = parts.tm_sec;
        hasLatchedClock = true;
    };

    if (!sweepEnabled) {
        // No beam to wait for a figure to be under: with the sweep off the
        // clock just tracks live time, the way it did before the sweep began
        // gating it.
        latchAll();
        hasPreviousClockSweepAngle = false;
        return;
    }

    if (!hasPreviousClockSweepAngle) {
        // First frame with a synced clock and a running sweep: paint every
        // figure once so the face is not blank for however long the beam takes
        // to reach all three, then let the beam take over from here.
        latchAll();
        previousClockSweepAngle = sweepAngle;
        hasPreviousClockSweepAngle = true;
        return;
    }

    float sweptAngle = sweepAngle - previousClockSweepAngle;
    if (sweptAngle < 0.0f)
        sweptAngle += TWO_PI;

    // Each figure's bearing from the face centre. Worked out once: the
    // geometry behind it is fixed at compile time, only the trig is not.
    static const float HOUR_BEARING = ClockFigureBearing(CLOCK_HOUR_DX, CLOCK_FIGURE_DY);
    static const float MINUTE_BEARING = ClockFigureBearing(CLOCK_MINUTE_DX, CLOCK_FIGURE_DY);
    static const float SECOND_BEARING = ClockFigureBearing(CLOCK_SECOND_DX, CLOCK_FIGURE_DY);

    auto sweptPast = [&](float bearing) {
        float angleFromPrevious = bearing - previousClockSweepAngle;
        if (angleFromPrevious < 0.0f)
            angleFromPrevious += TWO_PI;
        return angleFromPrevious > 0.0f && angleFromPrevious <= sweptAngle;
    };

    if (sweptPast(HOUR_BEARING))
        latchedClockHour = parts.tm_hour;
    if (sweptPast(MINUTE_BEARING))
        latchedClockMinute = parts.tm_min;
    if (sweptPast(SECOND_BEARING))
        latchedClockSecond = parts.tm_sec;

    previousClockSweepAngle = sweepAngle;
}

void AircraftManager::DrawClock(LGFX_Sprite& backbuffer) const
{
    if (!displayClock || !hasUtcOffset.load() || !hasLatchedClock)
        return;

    char digits[9];
    snprintf(digits, sizeof(digits), "%02d:%02d:%02d",
             latchedClockHour, latchedClockMinute, latchedClockSecond);

    backbuffer.setFont(&fonts::Font7);
    backbuffer.setTextSize(CLOCK_TEXT_SCALE);

    // The reserved box rather than the drawn extents: with the AM/PM suffix
    // gone, "00:00:00" is the only shape this ever takes, so the two are the
    // same size now. Same black the frame starts from, so the radar rings
    // crossing here are cut as well -- the point is a clock that reads at a
    // glance, and a ring running through the digits costs that as much as a
    // callsign does.
    ClearClockPlate(backbuffer, CLOCK_LABEL_X, CLOCK_LABEL_Y, CLOCK_DIGITS_WIDTH, CLOCK_DIGIT_HEIGHT);

    backbuffer.setTextColor(lgfx::color888(CLOCK_COLOR_R, CLOCK_COLOR_G, CLOCK_COLOR_B));

    // Both the digits and the plate cleared for them above go through the same
    // turn, so they stay square with each other however far out of true the
    // panel is mounted.
    PanelTrim::DrawTurnedText(
        backbuffer,
        PanelTrim::TextSlot::ClockDigits,
        digits,
        CLOCK_LABEL_X + CLOCK_DIGITS_WIDTH / 2,
        CLOCK_LABEL_Y + CLOCK_DIGIT_HEIGHT / 2
    );

    // Everything else on the face draws in the default font at the default
    // scale, and both are global state on the sprite.
    backbuffer.setFont(&fonts::Font0);
    backbuffer.setTextSize(1);
}

void AircraftManager::DrawLocationInfo(LGFX_Sprite& backbuffer) const
{
    if (locationNameLabel.isEmpty())
        return;

    // Measured against its bottom row, for the mirror of the reason the wind is
    // measured against its top one.
    SetRimLabelFont(backbuffer, locationNameLabel.c_str(), LOCATION_LABEL_BOTTOM);
    backbuffer.setTextColor(lgfx::color888(100, 100, 100));
    PanelTrim::DrawTurnedText(
        backbuffer,
        PanelTrim::TextSlot::Location,
        locationNameLabel.c_str(),
        SCREEN_SIZE_DIV_2,
        LOCATION_LABEL_Y + LOCATION_LABEL_HEIGHT / 2
    );

    backbuffer.setFont(&fonts::Font0);
    backbuffer.setTextSize(1);
}

void AircraftManager::DrawUpdateNotice(LGFX_Sprite& backbuffer) const
{
    if (!updateNoticeVisible)
        return;

    // Amber rather than the display's green: this is the one thing on screen
    // that is asking for a decision, and it should not read as radar data.
    // Kept to two words because the label has to fit inside the circle without
    // crowding the location name under it.
    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(220, 150, 0));
    backbuffer.drawCentreString(
        "Update available",
        SCREEN_SIZE_DIV_2,
        UPDATE_LABEL_Y
    );
}

void AircraftManager::DrawFetchNotice(LGFX_Sprite& backbuffer) const
{
    if (fetchFailureMessage.isEmpty())
        return;

    // Amber, on the same reasoning as the update notice: neither is radar data,
    // and the green is what says something is being tracked. That matters more
    // here than there -- this label is on screen precisely when nothing is.
    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(220, 150, 0));
    backbuffer.drawCentreString(
        "NO AIRCRAFT DATA",
        SCREEN_SIZE_DIV_2,
        FETCH_NOTICE_Y
    );

    // Dimmer than the line above it: the top line is what is wrong, this one is
    // the detail, and an owner reading the face from across a room should get
    // them in that order.
    backbuffer.setTextColor(lgfx::color888(150, 100, 0));
    backbuffer.drawCentreString(
        fetchFailureMessage.c_str(),
        SCREEN_SIZE_DIV_2,
        FETCH_NOTICE_Y + FETCH_NOTICE_LINE_HEIGHT + FETCH_NOTICE_LINE_GAP
    );
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
        if (!tracked.visibleOnRadar ||
            tracked.state.callsign.isEmpty() || tracked.destinationLookupAttempted)
            continue;

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
