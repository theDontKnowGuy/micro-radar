#include "ConfigurationWebServer.h"
#include <ESPmDNS.h>

// HTML stored in flash
// %PLACEHOLDER% tokens are substituted at serve time by the template processor.
// Do not put literal percent signs inside CONFIG_HTML (including CSS percentages),
// because ESPAsyncWebServer will interpret the text between them as a placeholder.
static const char CONFIG_HTML[] PROGMEM = R"(
<html>
    <head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>Configure Micro Radar</title>
        <script src="https://cdn.jsdelivr.net/npm/@tailwindcss/browser@4.3.0"></script>
        <style>
            .display-options {
                display: grid;
                grid-template-columns: repeat(auto-fit, minmax(145px, 1fr));
                gap: .65rem;
            }
            .display-option {
                display: flex;
                align-items: center;
                justify-content: space-between;
                gap: .75rem;
                padding: .7rem .8rem;
                border: 1px solid rgb(0 128 0);
                background: rgb(3 25 15);
                cursor: pointer;
            }
            .display-option:hover {
                border-color: rgb(34 197 94);
                background: rgb(5 35 20);
            }
            .display-option input {
                width: 1.1rem;
                height: 1.1rem;
                accent-color: rgb(34 197 94);
                flex: none;
            }
            .location-action {
                display: flex;
                align-items: center;
                gap: .75rem;
                flex-wrap: wrap;
            }
            .place-search {
                display: grid;
                grid-template-columns: 1fr auto;
                gap: .65rem;
            }
            .place-results {
                grid-column: 1 / -1;
                min-height: 7rem;
            }
            @media (max-width: 520px) {
                .place-search {
                    grid-template-columns: 1fr;
                }
                .place-results {
                    grid-column: 1;
                }
            }
        </style>
    </head>
    <body class="font-mono bg-gray-900 text-green-500 min-h-screen p-4 sm:p-0 text-md sm:text-sm">
        <fieldset class="border border-green-500 p-5 w-full max-w-2xl mx-auto sm:m-10">
            <legend class="px-2">Configure Micro Radar</legend>

            <form id="cfg" action="/save" method="POST" class="flex flex-col gap-4 sm:gap-2">

                <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                    <label class="flex flex-col sm:flex-row gap-2 flex-1">
                        <span>Latitude:</span>
                        <input
                            name="latitude"
                            id="latitude"
                            type="number"
                            min="-90"
                            step="0.000001"
                            max="90"
                            value='%LATITUDE%'
                            class="border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>

                    <label class="flex flex-col sm:flex-row gap-2 flex-1">
                        <span>Longitude:</span>
                        <input
                            name="longitude"
                            id="longitude"
                            type="number"
                            min="-180"
                            step="0.000001"
                            max="180"
                            value='%LONGITUDE%'
                            class="border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                </div>

                <div class="location-action">
                    <button
                        id="use-location"
                        type="button"
                        class="border border-green-500 bg-green-950 px-3 py-2 cursor-pointer hover:bg-green-900">
                        Use my current location
                    </button>
                    <span id="location-result" class="text-sm text-green-400" aria-live="polite"></span>
                </div>

                <fieldset class="border border-green-800 p-3">
                    <legend class="px-2 text-green-400">Find a known place</legend>
                    <div class="place-search">
                        <input
                            id="place-query"
                            type="search"
                            placeholder="Airport, city, landmark, or address"
                            autocomplete="off"
                            class="border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base">
                        <button
                            id="search-places"
                            type="button"
                            class="border border-green-500 bg-green-950 px-3 py-2 cursor-pointer hover:bg-green-900">
                            Search
                        </button>
                        <select
                            id="place-results"
                            class="place-results border border-green-700 bg-gray-900 px-2 py-2 text-green-400"
                            size="5"
                            hidden
                            aria-label="Place search results"></select>
                    </div>
                    <div id="place-result" class="mt-2 text-sm text-green-400" aria-live="polite"></div>
                    <div class="mt-2 text-xs text-green-700">
                        Search data &copy;
                        <a href="https://www.openstreetmap.org/copyright" target="_blank" rel="noopener"
                           class="underline">OpenStreetMap contributors</a>
                    </div>
                    <details class="mt-2 text-xs text-green-700">
                        <summary class="cursor-pointer">Place search provider</summary>
                        <input
                            id="geocoder-url"
                            name="geocoder-url"
                            value="%GEOCODER_URL%"
                            aria-label="Nominatim-compatible place search URL"
                            class="mt-2 border border-green-800 bg-gray-900 w-full px-2 py-1">
                    </details>
                </fieldset>

                <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                    <span>Radius (in &deg;):</span>
                    <input
                        name="radius"
                        type="number"
                        min="0.000001"
                        step="0.000001"
                        max="2.499999"
                        value='%RADIUS%'
                        class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                </label>

                <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                    <span>OpenSkyAPI Client ID:</span>
                    <input
                        name="opensky-id"
                        value='%OPENSKY_ID%'
                        class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                </label>

                <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                    <span>OpenSkyAPI Client Secret:</span>
                    <input
                        name="opensky-secret"
                        value='%OPENSKY_SECRET%'
                        class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                </label>

                <fieldset class="border border-green-800 p-3">
                    <legend class="px-2 text-green-400">Display</legend>
                    <div class="display-options">
                    <label class="display-option">
                        <span>Radar sweep:</span>
                        <input
                            name="scanline"
                            type="checkbox"
                            %SCANLINE%
                            >
                    </label>
                    <label class="display-option">
                        <span>Speed:</span>
                        <input
                            name="speed"
                            type="checkbox"
                            %SPEED%
                            >
                    </label>
                    <label class="display-option">
                        <span>Altitude:</span>
                        <input
                            name="altitude"
                            type="checkbox"
                            %ALTITUDE%
                            >
                    </label>
                    <label class="display-option">
                        <span>Destination:</span>
                        <input
                            name="destination"
                            type="checkbox"
                            %DESTINATION%
                            >
                    </label>
                    <label class="display-option">
                        <span>Directional Aircraft:</span>
                        <input
                            name="triangle"
                            type="checkbox"
                            %TRIANGLE%
                            >
                    </label>
                    <label class="display-option">
                        <span>Speed units:</span>
                        <select
                            name="speed-unit"
                            class="border border-green-600 bg-gray-900 px-2 py-1 text-green-400">
                            <option value="knots" %SPEED_KNOTS_SELECTED%>Knots</option>
                            <option value="meters-second" %SPEED_MS_SELECTED%>m/s</option>
                        </select>
                    </label>
                    <label class="display-option">
                        <span>Altitude units:</span>
                        <select
                            name="altitude-unit"
                            class="border border-green-600 bg-gray-900 px-2 py-1 text-green-400">
                            <option value="feet" %ALTITUDE_FEET_SELECTED%>Feet</option>
                            <option value="meters" %ALTITUDE_METERS_SELECTED%>Metres</option>
                        </select>
                    </label>
                    </div>
                </fieldset>

                <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                    <input
                        type="submit"
                        value="Save"
                        class="bg-green-500 text-black mt-4 px-4 py-3 text-lg sm:text-base sm:px-2 sm:py-0 self-start cursor-pointer">

                        <div id="result" class="mt-4 px-1 sm:px-10"></div>
                </div>
            </form>
        </fieldset>

        <script>
            const locationButton = document.getElementById('use-location');
            const locationResult = document.getElementById('location-result');
            const placeQuery = document.getElementById('place-query');
            const placeSearchButton = document.getElementById('search-places');
            const placeResults = document.getElementById('place-results');
            const placeResult = document.getElementById('place-result');
            const placeSearchCache = new Map();
            let lastPlaceSearchAt = 0;

            function fillLocation(latitude, longitude, message) {
                document.getElementById('latitude').value = Number(latitude).toFixed(6);
                document.getElementById('longitude').value = Number(longitude).toFixed(6);
                locationResult.textContent = message + ' Press Save to apply.';
            }

            function useApproximateLocation() {
                locationResult.textContent = 'Finding approximate network location...';
                fetch('https://ipwho.is/', { referrerPolicy: 'no-referrer' })
                    .then(function(response) {
                        if (!response.ok)
                            throw new Error('Location service returned ' + response.status);
                        return response.json();
                    })
                    .then(function(data) {
                        if (data.success === false || typeof data.latitude !== 'number' || typeof data.longitude !== 'number')
                            throw new Error(data.message || 'No coordinates returned');

                        const area = [data.city, data.region].filter(Boolean).join(', ');
                        fillLocation(
                            data.latitude,
                            data.longitude,
                            'Approximate network location filled' + (area ? ' (' + area + ').' : '.')
                        );
                    })
                    .catch(function() {
                        locationResult.textContent =
                            'Approximate location unavailable. Enter coordinates manually.';
                    });
            }

            if (!window.isSecureContext) {
                locationButton.textContent = 'Use approximate location';
                locationResult.textContent =
                    'Exact browser location requires HTTPS; this uses your network/IP location.';
            }

            locationButton.addEventListener('click', function() {
                if (!window.isSecureContext) {
                    useApproximateLocation();
                    return;
                }

                const result = document.getElementById('location-result');
                if (!navigator.geolocation) {
                    result.textContent = 'Location is not supported by this browser.';
                    return;
                }

                result.textContent = 'Finding location...';
                navigator.geolocation.getCurrentPosition(
                    function(position) {
                        fillLocation(
                            position.coords.latitude,
                            position.coords.longitude,
                            'Precise browser location filled.'
                        );
                    },
                    function(error) {
                        const messages = {
                            1: 'Location permission was denied.',
                            2: 'Your location could not be determined.',
                            3: 'Location request timed out.'
                        };
                        result.textContent = messages[error.code] || 'Location unavailable.';
                    },
                    { enableHighAccuracy: true, timeout: 10000, maximumAge: 300000 }
                );
            });

            function showPlaceResults(results) {
                placeResults.replaceChildren();

                results.forEach(function(place) {
                    const option = document.createElement('option');
                    option.textContent = place.display_name;
                    option.dataset.latitude = place.lat;
                    option.dataset.longitude = place.lon;
                    placeResults.appendChild(option);
                });

                placeResults.hidden = results.length === 0;
                placeResult.textContent = results.length
                    ? 'Select the intended place from the results.'
                    : 'No matching places found.';
            }

            function searchPlaces() {
                const query = placeQuery.value.trim();
                if (query.length < 2) {
                    placeResult.textContent = 'Enter at least two characters.';
                    return;
                }

                const cacheKey = query.toLocaleLowerCase();
                if (placeSearchCache.has(cacheKey)) {
                    showPlaceResults(placeSearchCache.get(cacheKey));
                    return;
                }

                const now = Date.now();
                if (now - lastPlaceSearchAt < 1000) {
                    placeResult.textContent = 'Please wait a second before searching again.';
                    return;
                }
                lastPlaceSearchAt = now;

                let searchUrl;
                try {
                    searchUrl = new URL(document.getElementById('geocoder-url').value);
                } catch (_) {
                    placeResult.textContent = 'The place search provider URL is invalid.';
                    return;
                }

                searchUrl.searchParams.set('q', query);
                searchUrl.searchParams.set('format', 'jsonv2');
                searchUrl.searchParams.set('limit', '5');
                searchUrl.searchParams.set('addressdetails', '0');

                placeSearchButton.disabled = true;
                placeResult.textContent = 'Searching places...';

                fetch(searchUrl.toString(), { referrerPolicy: 'strict-origin-when-cross-origin' })
                    .then(function(response) {
                        if (!response.ok)
                            throw new Error('Place search returned ' + response.status);
                        return response.json();
                    })
                    .then(function(results) {
                        if (!Array.isArray(results))
                            throw new Error('Unexpected place search response');
                        placeSearchCache.set(cacheKey, results);
                        showPlaceResults(results);
                    })
                    .catch(function() {
                        placeResults.hidden = true;
                        placeResult.textContent =
                            'Place search unavailable. Check internet access or the provider URL.';
                    })
                    .finally(function() {
                        placeSearchButton.disabled = false;
                    });
            }

            placeSearchButton.addEventListener('click', searchPlaces);
            placeQuery.addEventListener('keydown', function(event) {
                if (event.key === 'Enter') {
                    event.preventDefault();
                    searchPlaces();
                }
            });
            placeResults.addEventListener('change', function() {
                const selected = placeResults.options[placeResults.selectedIndex];
                if (!selected)
                    return;
                fillLocation(
                    selected.dataset.latitude,
                    selected.dataset.longitude,
                    'Selected place coordinates filled.'
                );
                placeResult.textContent = selected.textContent;
            });

            document.getElementById('cfg').addEventListener('submit', function(e) {
                e.preventDefault();
                fetch(this.action, { method: 'POST', body: new FormData(this) })
                    .then(r => r.text())
                    .then(html => document.getElementById('result').innerHTML = html);
            });
        </script>
    </body>
</html>
)";

void ConfigurationWebServer::EnsureDefaults() {
    prefs.begin("config", false);

    auto ensureKey = [this](const char* key, const char* defaultValue) {
        const String current = prefs.getString(key, "__MISSING__");
        if (current == "__MISSING__") {
            prefs.putString(key, defaultValue);
        }
    };

    ensureKey("latitude", "");
    ensureKey("longitude", "");
    ensureKey("radius", "1.0");
    ensureKey("opensky-id", "");
    ensureKey("opensky-secret", "");
    ensureKey("geocoder-url", "https://nominatim.openstreetmap.org/search");
    ensureKey("scanline", "true");
    ensureKey("speed", "true");
    ensureKey("speed-unit", "knots");
    ensureKey("altitude", "true");
    ensureKey("altitude-unit", "feet");
    ensureKey("destination", "false");
    ensureKey("triangle", "true");

    prefs.end();
}

void ConfigurationWebServer::Initialise() {
    EnsureDefaults();

    // start mDNS and check result
    if (!MDNS.begin("microradar")) {
        Serial.println("[WARN] Failed to start mDNS. Continuing without mDNS...");
    }

    // Handle visit to config web server
    server.on("/", HTTP_GET, [&](AsyncWebServerRequest* request) {
        Serial.println("[GET] Handling request to config web server...");

        // read all values up front so the processor lambda can capture by value
        prefs.begin("config", false);
        const String latitude = prefs.getString("latitude", "");
        const String longitude = prefs.getString("longitude", "");
        const String radius = prefs.getString("radius", "1.0");
        const String openskyClientId = prefs.getString("opensky-id", "");
        String openskySecret = prefs.getString("opensky-secret", "");
        const String geocoderUrl = prefs.getString("geocoder-url", "https://nominatim.openstreetmap.org/search");
        const String scanlineEnabled = prefs.getString("scanline", "true");
        const String speedEnabled = prefs.getString("speed", "true");
        const String speedUnit = prefs.getString("speed-unit", "knots");
        const String altitudeEnabled = prefs.getString("altitude", "true");
        const String altitudeUnit = prefs.getString("altitude-unit", "feet");
        const String destinationEnabled = prefs.getString("destination", "false");
        const String triangleEnabled = prefs.getString("triangle", "true");
        prefs.end();

        // mask secret before sending to client
        for (size_t i = 0; i < openskySecret.length(); ++i) {
            openskySecret[i] = '*';
        }

        // template processor called once per %PLACEHOLDER% token found in CONFIG_HTML.
        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html",
            (const uint8_t*)CONFIG_HTML, sizeof(CONFIG_HTML) - 1,
            [latitude, longitude, radius, openskyClientId, openskySecret, geocoderUrl, scanlineEnabled, speedEnabled, speedUnit, altitudeEnabled, altitudeUnit, destinationEnabled, triangleEnabled]
            (const String& var) -> String {
                if (var == "LATITUDE")       return latitude;
                if (var == "LONGITUDE")      return longitude;
                if (var == "RADIUS")         return radius;
                if (var == "OPENSKY_ID")     return openskyClientId;
                if (var == "OPENSKY_SECRET") return openskySecret;
                if (var == "GEOCODER_URL")   return geocoderUrl;
                if (var == "SCANLINE")       return scanlineEnabled == "true" ? "checked" : "";
                if (var == "SPEED")          return speedEnabled == "true" ? "checked" : "";
                if (var == "SPEED_KNOTS_SELECTED") return speedUnit == "knots" ? "selected" : "";
                if (var == "SPEED_MS_SELECTED") return speedUnit == "meters-second" ? "selected" : "";
                if (var == "ALTITUDE")       return altitudeEnabled == "true" ? "checked" : "";
                if (var == "ALTITUDE_FEET_SELECTED") return altitudeUnit == "feet" || altitudeUnit == "kft" ? "selected" : "";
                if (var == "ALTITUDE_METERS_SELECTED") return altitudeUnit == "meters" ? "selected" : "";
                if (var == "DESTINATION")    return destinationEnabled == "true" ? "checked" : "";
                if (var == "TRIANGLE")       return triangleEnabled == "true" ? "checked" : "";
                return "";
            }
        );
        request->send(response);
        }
    );

    // Handle save submission to web server
    server.on("/save", HTTP_POST, [&](AsyncWebServerRequest* request) {
        Serial.println("[POST] Handling form submission to config web server...");

        // safe parameter retrieval helper lambda
        auto TrySaveParam = [request, this](const char* paramName) {
            const auto* param = request->getParam(paramName, true);
            if (param == nullptr)
                return false;

            prefs.putString(paramName, param->value());
            return true;
            };

        prefs.begin("config", false);

        TrySaveParam("latitude");
        TrySaveParam("longitude");
        TrySaveParam("radius");
        TrySaveParam("opensky-id");
        TrySaveParam("geocoder-url");
        TrySaveParam("speed-unit");
        TrySaveParam("altitude-unit");

        const auto* param = request->getParam("opensky-secret", true);
        if (param != nullptr) {
            const String& secret = param->value();
            if (secret.indexOf('*') == -1) { // Special handling for secret: don't overwrite with masked value
                prefs.putString("opensky-secret", secret);
            }
        }

        prefs.putString("scanline", request->hasParam("scanline", true) ? "true" : "false");
        prefs.putString("triangle", request->hasParam("triangle", true) ? "true" : "false");
        prefs.putString("speed", request->hasParam("speed", true) ? "true" : "false");
        prefs.putString("altitude", request->hasParam("altitude", true) ? "true" : "false");
        prefs.putString("destination", request->hasParam("destination", true) ? "true" : "false");
        prefs.end();

        request->send(200, "text/html", "Saved - restarting device...");
        ESP.restart();
        }
    );

    server.begin();
}

const String ConfigurationWebServer::GetStoredString(const char* key)
{
    EnsureDefaults();
    prefs.begin("config", false);
    const String value = prefs.getString(key, "");
    prefs.end();
    return value;
}
