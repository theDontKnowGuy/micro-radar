#include "ConfigurationWebServer.h"
#include <ESPmDNS.h>

static String EscapeHtmlAttribute(const String& value) {
    String escaped;
    escaped.reserve(value.length());
    for (size_t i = 0; i < value.length(); ++i) {
        switch (value[i]) {
            case '&': escaped += F("&amp;"); break;
            case '<': escaped += F("&lt;"); break;
            case '>': escaped += F("&gt;"); break;
            case '"': escaped += F("&quot;"); break;
            case '\'': escaped += F("&#39;"); break;
            default: escaped += value[i]; break;
        }
    }
    return escaped;
}

// HTML stored in flash
// %PLACEHOLDER% tokens are substituted at serve time by the template processor.
// Do not put literal percent signs inside CONFIG_HTML (including CSS percentages),
// because ESPAsyncWebServer will interpret the text between them as a placeholder.
static const char CONFIG_HTML[] PROGMEM = R"(
<!doctype html>
<html lang="en">
    <head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>Configure Micro Radar</title>
        <style>
            :root {
                color-scheme: dark;
                --page: rgb(5 13 11);
                --panel: rgb(10 24 20);
                --section: rgb(12 31 25);
                --control: rgb(8 21 18);
                --control-hover: rgb(13 39 29);
                --line: rgb(34 197 94 / .24);
                --line-strong: rgb(74 222 128 / .55);
                --text: rgb(209 250 229);
                --muted: rgb(110 170 139);
                --green: rgb(74 222 128);
                --green-strong: rgb(34 197 94);
            }
            * {
                box-sizing: border-box;
            }
            html {
                background: var(--page);
            }
            body {
                margin: 0;
                min-height: 100vh;
                padding: 1.5rem 0;
                background:
                    radial-gradient(circle at 140px 70px, rgb(34 197 94 / .11), transparent 430px),
                    linear-gradient(145deg, rgb(4 12 10), rgb(8 20 17));
                color: var(--text);
                font-family:
                    Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont,
                    "Segoe UI", sans-serif;
                font-size: 14px;
                line-height: 1.45;
            }
            button,
            input,
            select {
                font: inherit;
            }
            input:not([type="checkbox"]),
            select {
                min-width: 0;
                border: 1px solid var(--line);
                border-radius: .7rem;
                outline: none;
                background: var(--control);
                color: var(--text);
                padding: .65rem .75rem;
                transition:
                    border-color 140ms ease,
                    background-color 140ms ease,
                    box-shadow 140ms ease;
            }
            input:not([type="checkbox"]):hover,
            select:hover {
                background: var(--control-hover);
            }
            input:not([type="checkbox"]):focus,
            select:focus {
                border-color: var(--green);
                box-shadow: 0 0 0 3px rgb(34 197 94 / .14);
            }
            input::placeholder {
                color: rgb(110 170 139 / .55);
            }
            button {
                border: 1px solid var(--line-strong);
                border-radius: .7rem;
                background: rgb(20 83 45 / .5);
                color: var(--green);
                padding: .65rem .9rem;
                cursor: pointer;
                transition:
                    transform 120ms ease,
                    border-color 120ms ease,
                    background-color 120ms ease;
            }
            button:hover {
                border-color: var(--green);
                background: rgb(22 101 52 / .72);
                transform: translateY(-1px);
            }
            button:focus-visible {
                outline: none;
                box-shadow: 0 0 0 3px rgb(34 197 94 / .16);
            }
            a {
                color: rgb(74 222 128 / .72);
            }
            h2,
            p {
                margin-top: 0;
            }
            .config-panel {
                width: min(1040px, calc(100vw - 2rem));
                margin: 0 auto;
                padding: 1.35rem;
                border: 1px solid var(--line);
                border-radius: 1.4rem;
                background: rgb(8 22 18 / .94);
                box-shadow:
                    0 24px 70px rgb(0 0 0 / .38),
                    inset 0 1px rgb(255 255 255 / .035);
            }
            .config-panel > legend {
                padding: .15rem .65rem;
                border: 1px solid var(--line);
                border-radius: 999px;
                background: rgb(8 22 18);
                color: var(--green);
                font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
                font-size: 1.05rem;
                font-weight: 650;
                letter-spacing: .025em;
            }
            .config-intro {
                margin: .1rem 0 .2rem;
                color: var(--muted);
                font-size: .84rem;
            }
            .config-form {
                display: flex;
                flex-direction: column;
                gap: 1rem;
            }
            .config-section {
                min-width: 0;
                border: 1px solid var(--line);
                border-radius: 1rem;
                padding: 1rem;
                background:
                    linear-gradient(145deg, rgb(15 40 31 / .82), rgb(9 27 22 / .9));
                box-shadow: inset 0 1px rgb(255 255 255 / .025);
            }
            .config-section > legend {
                padding: .15rem .55rem;
                border-radius: 999px;
                background: rgb(10 28 22);
                color: var(--green);
                font-weight: 650;
            }
            .section-note {
                margin: 0 0 .85rem;
                color: var(--muted);
                font-size: .8rem;
            }
            .config-section fieldset {
                min-width: 0;
                border: 1px solid var(--line);
                border-radius: .85rem;
                background: rgb(5 20 15 / .55);
            }
            .config-section fieldset > legend {
                padding: 0 .4rem;
                color: var(--muted);
            }
            .coordinate-grid {
                display: grid;
                grid-template-columns: repeat(2, minmax(0, 1fr));
                gap: .9rem;
            }
            .field-row {
                display: grid;
                grid-template-columns: max-content minmax(0, 1fr);
                align-items: center;
                gap: .75rem;
            }
            .field-row input {
                width: auto;
                min-width: 0;
            }
            .credentials {
                display: grid;
                gap: .75rem;
            }
            .display-options {
                display: grid;
                grid-template-columns: repeat(2, minmax(0, 1fr));
                gap: 1rem;
            }
            .display-group {
                min-width: 0;
                border: 1px solid var(--line);
                border-radius: .9rem;
                background: rgb(5 20 15 / .74);
                padding: .9rem;
                box-shadow:
                    0 8px 24px rgb(0 0 0 / .12),
                    inset 0 1px rgb(255 255 255 / .025);
            }
            .display-group-title {
                margin: 0 0 .15rem;
                color: var(--text);
                font-size: .98rem;
                font-weight: 650;
            }
            .display-group-note {
                margin: 0 0 .75rem;
                color: var(--muted);
                font-size: .78rem;
                min-height: 2.25rem;
            }
            .display-option {
                display: flex;
                align-items: center;
                justify-content: space-between;
                gap: .75rem;
                min-height: 3.35rem;
                margin-top: .5rem;
                padding: .55rem .7rem;
                border: 1px solid rgb(34 197 94 / .13);
                border-radius: .72rem;
                background: rgb(10 34 25 / .68);
                transition:
                    border-color 120ms ease,
                    background-color 120ms ease;
            }
            .display-option > label {
                min-width: 0;
            }
            .display-toggle {
                display: flex;
                align-items: center;
                justify-content: space-between;
                gap: .75rem;
                flex: 1;
                cursor: pointer;
            }
            .display-option:hover {
                border-color: var(--line);
                background: rgb(13 45 32 / .78);
            }
            .display-toggle input[type="checkbox"] {
                position: relative;
                width: 2.55rem;
                height: 1.4rem;
                margin: 0;
                border: 1px solid rgb(110 170 139 / .35);
                border-radius: 999px;
                appearance: none;
                background: rgb(30 55 45);
                cursor: pointer;
                flex: none;
                transition:
                    border-color 140ms ease,
                    background-color 140ms ease;
            }
            .display-toggle input[type="checkbox"]::after {
                position: absolute;
                top: .17rem;
                left: .18rem;
                width: .92rem;
                height: .92rem;
                border-radius: 999px;
                background: rgb(167 202 184);
                box-shadow: 0 1px 4px rgb(0 0 0 / .45);
                content: "";
                transition:
                    transform 140ms ease,
                    background-color 140ms ease;
            }
            .display-toggle input[type="checkbox"]:checked {
                border-color: var(--green);
                background: var(--green-strong);
            }
            .display-toggle input[type="checkbox"]:checked::after {
                background: rgb(240 253 244);
                transform: translateX(1.12rem);
            }
            .display-toggle input[type="checkbox"]:focus-visible {
                outline: none;
                box-shadow: 0 0 0 3px rgb(34 197 94 / .18);
            }
            .display-option select {
                min-width: 0;
                max-width: 14rem;
                padding: .55rem .65rem;
            }
            .display-option .aircraft-marker-select {
                width: 13.75rem;
            }
            .display-option .sweep-period-select {
                width: 13.75rem;
            }
            .display-option.is-disabled select {
                opacity: .4;
                cursor: not-allowed;
            }
            .location-action {
                display: flex;
                align-items: center;
                gap: .8rem;
                flex-wrap: wrap;
            }
            .place-search {
                display: grid;
                grid-template-columns: 1fr auto;
                gap: .7rem;
            }
            .place-search input {
                width: auto;
            }
            .place-results {
                grid-column: 1 / -1;
                width: auto;
                min-height: 7rem;
            }
            #place-result,
            #location-result {
                color: var(--muted);
            }
            details {
                color: var(--muted);
            }
            details summary {
                cursor: pointer;
            }
            details input {
                width: auto;
                display: block;
            }
            .save-row {
                display: flex;
                align-items: center;
                gap: 1rem;
                min-height: 3rem;
            }
            .save-button {
                border: 1px solid var(--green);
                border-radius: .75rem;
                background:
                    linear-gradient(145deg, rgb(74 222 128), rgb(34 197 94));
                color: rgb(3 24 14);
                font-weight: 750;
                padding: .7rem 1.25rem;
                cursor: pointer;
                box-shadow: 0 8px 22px rgb(34 197 94 / .18);
                transition:
                    transform 120ms ease,
                    filter 120ms ease;
            }
            .save-button:hover {
                filter: brightness(1.08);
                transform: translateY(-1px);
            }
            #result {
                color: var(--green);
            }
            .mt-2 {
                margin-top: .5rem;
            }
            .mt-3 {
                margin-top: .75rem;
            }
            .text-xs {
                font-size: .75rem;
            }
            .text-sm {
                font-size: .85rem;
            }
            .underline {
                text-decoration: underline;
            }
            @media (max-width: 820px) {
                .config-panel {
                    width: min(1040px, calc(100vw - 1.25rem));
                }
                .display-options {
                    grid-template-columns: minmax(0, 1fr);
                }
                .display-group-note {
                    min-height: 0;
                }
            }
            @media (max-width: 520px) {
                body {
                    padding: .5rem 0;
                }
                .config-panel {
                    width: min(1040px, calc(100vw - .75rem));
                    padding: .8rem;
                    border-radius: 1rem;
                }
                .config-section {
                    padding: .75rem;
                    border-radius: .8rem;
                }
                .coordinate-grid {
                    grid-template-columns: minmax(0, 1fr);
                }
                .field-row {
                    grid-template-columns: minmax(0, 1fr);
                    gap: .3rem;
                }
                .place-search {
                    grid-template-columns: 1fr;
                }
                .place-results {
                    grid-column: 1;
                }
                .display-option-select {
                    align-items: stretch;
                    flex-direction: column;
                    gap: .35rem;
                }
                .display-option .aircraft-marker-select,
                .display-option .sweep-period-select {
                    width: auto;
                    max-width: none;
                }
                .display-option:not(.display-option-select) {
                    gap: .55rem;
                }
                .save-row {
                    align-items: flex-start;
                    flex-direction: column;
                    gap: .35rem;
                }
            }
        </style>
    </head>
    <body>
        <fieldset class="config-panel">
            <legend>Configure Micro Radar</legend>

            <p class="config-intro">Set radar coverage, aircraft data, and display preferences.</p>
            <form id="cfg" action="/save" method="POST" class="config-form">

                <fieldset class="config-section">
                    <legend>Radar coverage</legend>
                    <div class="coordinate-grid">
                    <label class="field-row">
                        <span>Latitude:</span>
                        <input
                            name="latitude"
                            id="latitude"
                            type="number"
                            min="-90"
                            step="0.000001"
                            max="90"
                            value='%LATITUDE%'>
                    </label>

                    <label class="field-row">
                        <span>Longitude:</span>
                        <input
                            name="longitude"
                            id="longitude"
                            type="number"
                            min="-180"
                            step="0.000001"
                            max="180"
                            value='%LONGITUDE%'>
                    </label>
                    </div>

                    <div class="location-action mt-3">
                    <button
                        id="use-location"
                        type="button">
                        Use my current location
                    </button>
                    <span id="location-result" class="text-sm" aria-live="polite"></span>
                    </div>

                    <fieldset class="mt-3">
                    <legend>Find a known place</legend>
                    <div class="place-search">
                        <input
                            id="place-query"
                            type="search"
                            placeholder="Airport, city, landmark, or address"
                            autocomplete="off">
                        <button
                            id="search-places"
                            type="button">
                            Search
                        </button>
                        <select
                            id="place-results"
                            class="place-results"
                            size="5"
                            hidden
                            aria-label="Place search results"></select>
                    </div>
                    <div id="place-result" class="mt-2 text-sm" aria-live="polite"></div>
                    <div class="mt-2 text-xs">
                        Search data &copy;
                        <a href="https://www.openstreetmap.org/copyright" target="_blank" rel="noopener"
                           class="underline">OpenStreetMap contributors</a>
                    </div>
                    <details class="mt-2 text-xs">
                        <summary>Place search provider</summary>
                        <input
                            id="geocoder-url"
                            name="geocoder-url"
                            value="%GEOCODER_URL%"
                            aria-label="Nominatim-compatible place search URL"
                            class="mt-2">
                    </details>
                    </fieldset>

                    <label class="field-row mt-3">
                    <span>Radius (in &deg;):</span>
                    <input
                        name="radius"
                        type="number"
                        min="0.000001"
                        step="0.000001"
                        max="2.499999"
                        value='%RADIUS%'>
                    </label>
                </fieldset>

                <fieldset class="config-section">
                    <legend>OpenSky connection</legend>
                    <p class="section-note">Optional credentials increase API access limits.</p>
                    <div class="credentials">
                    <label class="field-row">
                    <span>Client ID:</span>
                    <input
                        name="opensky-id"
                        autocomplete="off"
                        spellcheck="false"
                        value='%OPENSKY_ID%'>
                    </label>

                    <label class="field-row">
                    <span>Client secret:</span>
                    <input
                        name="opensky-secret"
                        type="password"
                        autocomplete="off"
                        spellcheck="false"
                        value='%OPENSKY_SECRET%'>
                    </label>
                    </div>
                </fieldset>

                <fieldset class="config-section">
                    <legend>Display</legend>
                    <div class="display-options">
                        <section class="display-group" aria-labelledby="radar-appearance-title">
                            <h2 id="radar-appearance-title" class="display-group-title">Radar appearance</h2>
                            <p class="display-group-note">Sweep behavior and target presentation.</p>

                            <div class="display-option">
                                <label class="display-toggle" for="scanline">
                                    <span>Animated radar sweep</span>
                                    <input id="scanline" name="scanline" type="checkbox" %SCANLINE%>
                                </label>
                            </div>

                            <div class="display-option display-option-select">
                                <label for="sweep-period">Sweep speed</label>
                                <select
                                    id="sweep-period"
                                    name="sweep-period"
                                    class="sweep-period-select"
                                    aria-label="Radar sweep speed">
                                    <option value="2" %SWEEP_2_SELECTED%>Very fast &middot; 2 s/rev</option>
                                    <option value="5" %SWEEP_5_SELECTED%>Fast &middot; 5 s/rev</option>
                                    <option value="10" %SWEEP_10_SELECTED%>Balanced &middot; 10 s/rev</option>
                                    <option value="18" %SWEEP_18_SELECTED%>Classic &middot; 18 s/rev</option>
                                    <option value="30" %SWEEP_30_SELECTED%>Slow &middot; 30 s/rev</option>
                                </select>
                            </div>

                            <div class="display-option display-option-select">
                                <label for="aircraft-marker">Aircraft symbol</label>
                                <select
                                    id="aircraft-marker"
                                    name="aircraft-marker"
                                    class="aircraft-marker-select"
                                    aria-label="Aircraft symbol">
                                    <option value="radar" %MARKER_RADAR_SELECTED%>Radar block + vector</option>
                                    <option value="triangle" %MARKER_TRIANGLE_SELECTED%>Aircraft triangle</option>
                                    <option value="dot" %MARKER_DOT_SELECTED%>Simple dot</option>
                                </select>
                            </div>

                            <div class="display-option">
                                <label class="display-toggle" for="wind">
                                    <span>Show center surface wind</span>
                                    <input id="wind" name="wind" type="checkbox" %WIND%>
                                </label>
                            </div>
                        </section>

                        <section class="display-group" aria-labelledby="aircraft-labels-title">
                            <h2 id="aircraft-labels-title" class="display-group-title">Aircraft labels</h2>
                            <p class="display-group-note">Callsigns are always shown. Choose the additional lines below.</p>

                            <div class="display-option">
                                <label class="display-toggle" for="speed">
                                    <span>Show speed</span>
                                    <input id="speed" name="speed" type="checkbox" %SPEED%>
                                </label>
                                <select id="speed-unit" name="speed-unit" aria-label="Speed units">
                                    <option value="knots" %SPEED_KNOTS_SELECTED%>Knots</option>
                                    <option value="meters-second" %SPEED_MS_SELECTED%>m/s</option>
                                </select>
                            </div>

                            <div class="display-option">
                                <label class="display-toggle" for="altitude">
                                    <span>Show altitude</span>
                                    <input id="altitude" name="altitude" type="checkbox" %ALTITUDE%>
                                </label>
                                <select id="altitude-unit" name="altitude-unit" aria-label="Altitude units">
                                    <option value="feet" %ALTITUDE_FEET_SELECTED%>Feet</option>
                                    <option value="meters" %ALTITUDE_METERS_SELECTED%>Metres</option>
                                </select>
                            </div>

                            <div class="display-option">
                                <label class="display-toggle" for="destination">
                                    <span>Show route when available</span>
                                    <input id="destination" name="destination" type="checkbox" %DESTINATION%>
                                </label>
                            </div>
                        </section>
                    </div>
                </fieldset>

                <div class="save-row">
                    <input
                        type="submit"
                        value="Save"
                        class="save-button">

                    <div id="result" aria-live="polite"></div>
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

            function bindDependentSelect(toggleId, selectId) {
                const toggle = document.getElementById(toggleId);
                const select = document.getElementById(selectId);
                const row = select.closest('.display-option');

                function syncSelectState() {
                    select.disabled = !toggle.checked;
                    row.classList.toggle('is-disabled', !toggle.checked);
                }

                toggle.addEventListener('change', syncSelectState);
                syncSelectState();
            }

            bindDependentSelect('scanline', 'sweep-period');
            bindDependentSelect('speed', 'speed-unit');
            bindDependentSelect('altitude', 'altitude-unit');

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
        if (!prefs.isKey(key))
            prefs.putString(key, defaultValue);
    };

    ensureKey("latitude", "");
    ensureKey("longitude", "");
    ensureKey("radius", "1.0");
    ensureKey("opensky-id", "");
    ensureKey("opensky-secret", "");
    ensureKey("geocoder-url", "https://nominatim.openstreetmap.org/search");
    ensureKey("scanline", "true");
    ensureKey("sweep-period", "5");
    if (!prefs.isKey("aircraft-marker")) {
        const String marker = prefs.isKey("triangle")
            ? (prefs.getString("triangle", "true") == "true" ? "triangle" : "dot")
            : "radar";
        prefs.putString("aircraft-marker", marker);
    }
    ensureKey("speed", "true");
    ensureKey("speed-unit", "knots");
    ensureKey("altitude", "true");
    ensureKey("altitude-unit", "feet");
    ensureKey("destination", "false");
    ensureKey("wind", "false");

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
        prefs.begin("config", true);
        const String latitude = EscapeHtmlAttribute(prefs.getString("latitude", ""));
        const String longitude = EscapeHtmlAttribute(prefs.getString("longitude", ""));
        const String radius = EscapeHtmlAttribute(prefs.getString("radius", "1.0"));
        const String openskyClientId = EscapeHtmlAttribute(prefs.getString("opensky-id", ""));
        String openskySecret = prefs.getString("opensky-secret", "");
        const String geocoderUrl = EscapeHtmlAttribute(
            prefs.getString("geocoder-url", "https://nominatim.openstreetmap.org/search")
        );
        const String scanlineEnabled = prefs.getString("scanline", "true");
        const String sweepPeriod = prefs.getString("sweep-period", "5");
        const String speedEnabled = prefs.getString("speed", "true");
        const String speedUnit = prefs.getString("speed-unit", "knots");
        const String altitudeEnabled = prefs.getString("altitude", "true");
        const String altitudeUnit = prefs.getString("altitude-unit", "feet");
        const String destinationEnabled = prefs.getString("destination", "false");
        const String windEnabled = prefs.getString("wind", "false");
        const String aircraftMarker = prefs.getString("aircraft-marker", "radar");
        prefs.end();

        // mask secret before sending to client
        for (size_t i = 0; i < openskySecret.length(); ++i) {
            openskySecret[i] = '*';
        }

        // template processor called once per %PLACEHOLDER% token found in CONFIG_HTML.
        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html",
            (const uint8_t*)CONFIG_HTML, sizeof(CONFIG_HTML) - 1,
            [latitude, longitude, radius, openskyClientId, openskySecret, geocoderUrl, scanlineEnabled, sweepPeriod, speedEnabled, speedUnit, altitudeEnabled, altitudeUnit, destinationEnabled, windEnabled, aircraftMarker]
            (const String& var) -> String {
                if (var == "LATITUDE")       return latitude;
                if (var == "LONGITUDE")      return longitude;
                if (var == "RADIUS")         return radius;
                if (var == "OPENSKY_ID")     return openskyClientId;
                if (var == "OPENSKY_SECRET") return openskySecret;
                if (var == "GEOCODER_URL")   return geocoderUrl;
                if (var == "SCANLINE")       return scanlineEnabled == "true" ? "checked" : "";
                if (var == "SWEEP_2_SELECTED") return sweepPeriod == "2" ? "selected" : "";
                if (var == "SWEEP_5_SELECTED") return sweepPeriod == "5" ? "selected" : "";
                if (var == "SWEEP_10_SELECTED") return sweepPeriod == "10" ? "selected" : "";
                if (var == "SWEEP_18_SELECTED") return sweepPeriod == "18" ? "selected" : "";
                if (var == "SWEEP_30_SELECTED") return sweepPeriod == "30" ? "selected" : "";
                if (var == "SPEED")          return speedEnabled == "true" ? "checked" : "";
                if (var == "SPEED_KNOTS_SELECTED") return speedUnit == "knots" ? "selected" : "";
                if (var == "SPEED_MS_SELECTED") return speedUnit == "meters-second" ? "selected" : "";
                if (var == "ALTITUDE")       return altitudeEnabled == "true" ? "checked" : "";
                if (var == "ALTITUDE_FEET_SELECTED") return altitudeUnit == "feet" || altitudeUnit == "kft" ? "selected" : "";
                if (var == "ALTITUDE_METERS_SELECTED") return altitudeUnit == "meters" ? "selected" : "";
                if (var == "DESTINATION")    return destinationEnabled == "true" ? "checked" : "";
                if (var == "WIND")           return windEnabled == "true" ? "checked" : "";
                if (var == "MARKER_RADAR_SELECTED") return aircraftMarker == "radar" ? "selected" : "";
                if (var == "MARKER_TRIANGLE_SELECTED") return aircraftMarker == "triangle" ? "selected" : "";
                if (var == "MARKER_DOT_SELECTED") return aircraftMarker == "dot" ? "selected" : "";
                return "";
            }
        );
        // Configuration changes should be visible immediately after flashing
        // or saving; never let the browser reuse an older embedded page.
        response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
        response->addHeader("Pragma", "no-cache");
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

        const auto* sweepPeriodParam = request->getParam("sweep-period", true);
        if (sweepPeriodParam != nullptr) {
            const String sweepPeriod = sweepPeriodParam->value();
            if (sweepPeriod == "2" || sweepPeriod == "5" || sweepPeriod == "10" ||
                sweepPeriod == "18" || sweepPeriod == "30")
                prefs.putString("sweep-period", sweepPeriod);
        }

        const auto* markerParam = request->getParam("aircraft-marker", true);
        if (markerParam != nullptr) {
            const String marker = markerParam->value();
            if (marker == "radar" || marker == "triangle" || marker == "dot")
                prefs.putString("aircraft-marker", marker);
        }

        const auto* param = request->getParam("opensky-secret", true);
        if (param != nullptr) {
            const String& secret = param->value();
            if (secret.indexOf('*') == -1) { // Special handling for secret: don't overwrite with masked value
                prefs.putString("opensky-secret", secret);
            }
        }

        prefs.putString("scanline", request->hasParam("scanline", true) ? "true" : "false");
        prefs.putString("speed", request->hasParam("speed", true) ? "true" : "false");
        prefs.putString("altitude", request->hasParam("altitude", true) ? "true" : "false");
        prefs.putString("destination", request->hasParam("destination", true) ? "true" : "false");
        prefs.putString("wind", request->hasParam("wind", true) ? "true" : "false");
        prefs.end();

        request->send(200, "text/html", "Saved - restarting device...");

        // Deliberately not ESP.restart() here: this runs on the AsyncTCP task,
        // and resetting mid-frame leaves SPI DMA transfers in flight, which
        // corrupts memory during the next boot. The main loop picks this up and
        // restarts once the display is idle. See ConfigurationWebServer.h.
        restartRequested.store(true);
        }
    );

    server.begin();
}

String ConfigurationWebServer::GetStoredString(const char* key)
{
    prefs.begin("config", true);
    const String value = prefs.getString(key, "");
    prefs.end();
    return value;
}
