#include "ConfigurationWebServer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <ESPmDNS.h>
#include <WiFi.h>

#include "DisplayConfig.h"
#include "FirmwareVersion.h"
#include "WebLogo.h"

// The radar answers every name on the setup hotspot, which is what makes a
// phone open the page by itself instead of reporting no internet.
static const IPAddress SetupPortalIp(192, 168, 4, 1);

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

// Status messages carry version numbers, URLs and error text from the updater,
// any of which can contain a quote or a backslash that would break the JSON the
// configuration page parses.
static String EscapeJsonString(const String& value) {
    String escaped;
    escaped.reserve(value.length());
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        switch (c) {
            case '"': escaped += F("\\\""); break;
            case '\\': escaped += F("\\\\"); break;
            case '\n': escaped += F("\\n"); break;
            case '\r': escaped += F("\\r"); break;
            case '\t': escaped += F("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    escaped += ' ';
                else
                    escaped += c;
                break;
        }
    }
    return escaped;
}

// Reads as a duration rather than a number of milliseconds: this stands in for
// the diagnostics page the Wi-Fi portal used to have, where "how long has it
// been up" is the question actually being asked.
static String FormatUptime(unsigned long milliseconds)
{
    const unsigned long seconds = milliseconds / 1000UL;
    const unsigned long days = seconds / 86400UL;
    const unsigned long hours = (seconds / 3600UL) % 24UL;
    const unsigned long minutes = (seconds / 60UL) % 60UL;

    char formatted[32];
    if (days > 0)
        snprintf(formatted, sizeof(formatted), "%lud %luh %lum", days, hours, minutes);
    else if (hours > 0)
        snprintf(formatted, sizeof(formatted), "%luh %lum", hours, minutes);
    else
        snprintf(formatted, sizeof(formatted), "%lum %lus", minutes, seconds % 60UL);

    return String(formatted);
}

// Everything the page is rendered from, read once per request. The template
// processor runs later, while the response is being written out, so it must not
// be the thing that opens Preferences or asks the Wi-Fi driver anything.
struct PageValues {
    String latitude, longitude, locationName, radius, distanceUnit;
    String openskyClientId, openskySecretPlaceholder, geocoderUrl;
    String scanlineEnabled, sweepPeriod, speedEnabled, speedUnit;
    String altitudeEnabled, altitudeUnit, destinationEnabled, windEnabled;
    String clockEnabled, clockFormat;
    String aircraftMarker, autoUpdate, screenTrim, alignmentTest;
    String insightsKeyPlaceholder, insightsLabel;

    // Wi-Fi section and the mode it is being shown in.
    String setupMode, wifiSsid, wifiPassPlaceholder;
    String networkNote, networkOpen, networkSummary, forgetHidden, saveLabel;
    String netIp, netRssi, netMac, netUptime, netHeap;

    // Tab strip. Hidden on the setup hotspot, where the network is the only
    // thing worth setting and the other two tabs have nothing to offer yet.
    String tabsHidden, openskyHidden;
};

// The radar has no login, so anything on the LAN can already reconfigure it on
// purpose. What this stops is a page on the wider internet doing it without the
// owner's knowledge: a form or fetch aimed at http://radar.local/save from
// an unrelated tab. A browser labels those with an Origin that is not ours.
// Requests carrying no Origin at all (curl, scripts) are left alone -- they are
// not the attack this is here for.
static bool IsCrossSiteRequest(AsyncWebServerRequest* request)
{
    if (!request->hasHeader("Origin"))
        return false;

    const String origin = request->header("Origin");
    const String& host = request->host();
    return origin != "http://" + host && origin != "https://" + host;
}

// Rejects a POST that a browser says came from somewhere else. Returns true
// when the caller should stop.
static bool RejectedAsCrossSite(AsyncWebServerRequest* request)
{
    if (!IsCrossSiteRequest(request))
        return false;

    Serial.print("[WARN] Rejected cross-site request from ");
    Serial.println(request->header("Origin"));
    request->send(403, "text/plain", "Cross-site request rejected");
    return true;
}

// The page constrains these with min/max/step, but the page is not the only
// thing that can POST to /save. A radius of zero is the one that matters: it is
// the divisor in the coordinate projection, so storing it turns every aircraft
// position into infinity on the next boot.
static bool IsNumberInRange(const String& value, double low, double high)
{
    if (value.isEmpty())
        return false;

    char* end = nullptr;
    const double parsed = strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0' || std::isnan(parsed))
        return false;

    return parsed >= low && parsed <= high;
}

// Stored, then handed to the page's `new URL(...)` and fetched by the browser.
// Anything that is not plain http(s) has no business being saved here.
static bool IsHttpUrl(const String& value)
{
    return value.startsWith("http://") || value.startsWith("https://");
}

static String CleanLocationName(const String& value)
{
    String cleaned;
    cleaned.reserve(value.length());

    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        cleaned += (c >= 32) ? c : ' ';
    }

    cleaned.trim();

    if (cleaned.length() > 18)
        cleaned.remove(18);

    return cleaned;
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
        <link rel="icon" type="image/png" href="/logo.png">
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

                /* A fieldset paints its top border through the middle of its
                   legend rather than along the top of its box, so the panel's
                   visible top line is half a legend below where the geometry
                   APIs put the panel. The corner mark has to sit on that line,
                   so it is offset from these two rather than from a measured
                   constant that would drift the moment the legend changes
                   size. Legend height is pinned for the same reason. */
                --legend-height: 2rem;
                --mark-size: 88px;
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
                /* Top padding has to clear the half of the corner mark that
                   sits above the panel, or it is cut off by the viewport. */
                padding: 2.6rem 0 1.5rem;
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
            /* Exists only to be the corner mark's containing block. The panel
               itself cannot be: it is a fieldset, and a fieldset's absolutely
               positioned children measure top:0 from below the legend rather
               than from the border, which drops the mark by exactly the
               legend's height. */
            .config-shell {
                position: relative;
                width: min(1040px, calc(100vw - 2rem));
                margin: 0 auto;
            }
            .config-panel {
                width: auto;
                margin: 0;
                /* Top padding clears the half of the corner mark that hangs
                   inside the panel; it is positioned out of flow, so nothing
                   else moves down to make room for it. */
                padding: 3.2rem 1.35rem 1.35rem;
                border: 1px solid var(--line);
                border-radius: 1.4rem;
                background: rgb(8 22 18 / .94);
                box-shadow:
                    0 24px 70px rgb(0 0 0 / .38),
                    inset 0 1px rgb(255 255 255 / .035);
            }
            .config-panel > legend {
                box-sizing: border-box;
                display: inline-flex;
                align-items: center;
                height: var(--legend-height);
                padding: 0 .65rem;
                border: 1px solid var(--line);
                border-radius: 999px;
                background: rgb(8 22 18);
                color: var(--green);
                font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
                font-size: 1.05rem;
                font-weight: 650;
                letter-spacing: .025em;
            }
            /* Centred on the panel's painted top border, on the corner
               opposite the legend, so half the mark sits above the line. The
               offset lands its middle on the legend's middle, which is where
               that border is drawn -- see the note on --legend-height. Written
               as a calc rather than a translate because a CSS percentage
               cannot appear anywhere in this page: see the note above
               CONFIG_HTML.

               The disc is what hides the border line. The ring is open in the
               middle, so without it the line would run straight through the
               wordmark. The artwork is blue and the rest of the page is green
               phosphor -- rather than recolour someone's logo, the halo around
               it is the page's own accent. */
            .masthead-mark {
                position: absolute;
                top: calc((var(--legend-height) - var(--mark-size)) / 2);
                right: 1.6rem;
                display: block;
                width: var(--mark-size);
                height: var(--mark-size);
                border-radius: 50rem;
                background: rgb(7 19 16);
                box-shadow:
                    0 0 22px rgb(56 130 246 / .22),
                    0 0 46px rgb(34 197 94 / .07);
            }
            .firmware-footer {
                margin: 1.1rem 0 0;
                padding-top: .8rem;
                border-top: 1px solid var(--line);
                color: var(--muted);
                font-size: .78rem;
                line-height: 1.45;
            }
            .firmware-version {
                color: var(--green);
                font-weight: 650;
            }
            .firmware-notes {
                margin: .2rem 0 0;
            }
            .firmware-update {
                margin: .45rem 0 0;
            }
            .project-footer {
                margin: .8rem 0 0;
                padding-top: .8rem;
                border-top: 1px solid var(--line);
                color: var(--muted);
                font-size: .78rem;
                line-height: 1.45;
            }
            .project-credit {
                margin: .2rem 0 0;
            }
            .link-button {
                border: 0;
                padding: 0;
                background: none;
                color: var(--green);
                font: inherit;
                text-decoration: underline;
                cursor: pointer;
            }
            .link-button:hover {
                filter: brightness(1.15);
            }
            .link-button:disabled {
                color: var(--muted);
                text-decoration: none;
                cursor: default;
            }
            .tab-bar {
                display: flex;
                gap: .4rem;
                margin: .75rem 0 1rem;
                padding: .3rem;
                border: 1px solid var(--line);
                border-radius: .9rem;
                background: rgb(5 20 15 / .6);
            }
            .tab-bar[hidden] {
                display: none;
            }
            .tab-button {
                flex: 1;
                border: 1px solid transparent;
                border-radius: .7rem;
                background: none;
                color: var(--muted);
                font-weight: 650;
                padding: .55rem .5rem;
            }
            .tab-button:hover {
                border-color: var(--line);
                background: rgb(13 39 29 / .6);
                transform: none;
            }
            .tab-button[aria-selected="true"] {
                border-color: var(--line-strong);
                background: rgb(20 83 45 / .55);
                color: var(--green);
            }
            .config-form {
                display: flex;
                flex-direction: column;
                gap: 1rem;
            }
            .tab-panel {
                display: flex;
                flex-direction: column;
                gap: 1rem;
            }
            /* display: flex above would otherwise beat the hidden attribute. */
            .tab-panel[hidden] {
                display: none;
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
            /* display: grid above would otherwise beat the hidden attribute. */
            .field-row[hidden] {
                display: none;
            }
            .credentials {
                display: grid;
                gap: .75rem;
            }
            .network-details > summary {
                color: var(--green);
                font-weight: 650;
            }
            .network-grid {
                display: grid;
                gap: .75rem;
            }
            .ssid-row {
                display: grid;
                grid-template-columns: minmax(0, 1fr) auto;
                gap: .7rem;
            }
            .ssid-row select {
                width: auto;
            }
            .network-diagnostics {
                display: grid;
                grid-template-columns: max-content minmax(0, 1fr);
                gap: .2rem .75rem;
                margin: .9rem 0 0;
                color: var(--muted);
                font-size: .78rem;
            }
            .network-diagnostics dd {
                margin: 0;
                color: var(--text);
                font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
            }
            .network-actions {
                display: flex;
                align-items: center;
                gap: 1rem;
                flex-wrap: wrap;
                margin-top: .9rem;
            }
            .is-offline {
                opacity: .45;
                cursor: not-allowed;
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
            .display-option .update-mode-select {
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
            /* The generic input rule above dresses every non-checkbox input as
               a text box, which draws a border and padding around a slider. */
            input[type="range"] {
                width: auto;
                border: 0;
                padding: 0;
                background: none;
                accent-color: var(--green);
            }
            input[type="range"]:hover {
                background: none;
            }
            .radius-row {
                display: grid;
                grid-template-columns: minmax(0, 1fr) auto;
                align-items: center;
                gap: .9rem;
            }
            .radius-readout {
                display: flex;
                align-items: center;
                gap: .6rem;
                color: var(--green);
                font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
                font-weight: 650;
                white-space: nowrap;
            }
            .radius-readout select {
                padding: .4rem .5rem;
            }
            .danger-zone {
                border-color: rgb(248 113 113 / .4);
            }
            .danger-zone > legend {
                color: rgb(252 165 165);
            }
            .danger-action {
                display: flex;
                align-items: center;
                gap: 1rem;
                flex-wrap: wrap;
                margin-top: .75rem;
            }
            .danger-button {
                border-color: rgb(248 113 113 / .6);
                background: rgb(127 29 29 / .45);
                color: rgb(254 202 202);
            }
            .danger-button:hover {
                border-color: rgb(248 113 113);
                background: rgb(153 27 27 / .62);
            }
            .danger-button:disabled {
                opacity: .45;
                cursor: not-allowed;
                transform: none;
            }
            .danger-button:disabled:hover {
                border-color: rgb(248 113 113 / .6);
                background: rgb(127 29 29 / .45);
                transform: none;
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
            .location-name input {
                max-width: 18rem;
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
            /* Sticky because Save applies to all three tabs at once: it has to
               stay in view from wherever the last change was made, or the tab
               that is showing reads as the only thing being saved. */
            .save-row {
                position: sticky;
                bottom: 0;
                z-index: 2;
                display: flex;
                align-items: center;
                gap: 1rem;
                min-height: 3rem;
                margin-top: .25rem;
                padding: .75rem 0;
                border-top: 1px solid var(--line);
                background: rgb(8 22 18 / .97);
            }
            .save-state {
                color: var(--muted);
                font-size: .8rem;
            }
            .save-state.is-dirty {
                color: rgb(250 204 21);
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
                .config-shell {
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
                    padding: 1.6rem 0 .5rem;
                }
                .config-shell {
                    width: min(1040px, calc(100vw - .75rem));
                }
                .config-panel {
                    padding: 2.3rem .8rem .8rem;
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
                .place-search,
                .ssid-row {
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
                .display-option .sweep-period-select,
                .display-option .update-mode-select {
                    width: auto;
                    max-width: none;
                }
                .display-option:not(.display-option-select) {
                    gap: .55rem;
                }
                :root {
                    --mark-size: 58px;
                }
                .masthead-mark {
                    right: .9rem;
                }
                .tab-button {
                    padding: .55rem .25rem;
                    font-size: .9rem;
                }
                .radius-row {
                    grid-template-columns: minmax(0, 1fr);
                }
                .save-row {
                    align-items: flex-start;
                    flex-direction: column;
                    gap: .35rem;
                }
            }
        </style>
    </head>
    <body data-net-mode="%NET_MODE%">
      <div class="config-shell">
        <fieldset class="config-panel">
            <legend>Configure Micro Radar</legend>

            <div class="tab-bar" role="tablist" aria-label="Configuration sections" %TABS_HIDDEN%>
                <button type="button" class="tab-button" role="tab" data-tab="radar"
                        id="tab-radar" aria-controls="panel-radar" aria-selected="true">Radar</button>
                <button type="button" class="tab-button" role="tab" data-tab="setup"
                        id="tab-setup" aria-controls="panel-setup" aria-selected="false">Setup</button>
                <button type="button" class="tab-button" role="tab" data-tab="advanced"
                        id="tab-advanced" aria-controls="panel-advanced" aria-selected="false">Advanced</button>
            </div>

            <!--
                One form across all three tabs, and one Save. The tabs hide their
                panels with the hidden attribute rather than removing them,
                because /save reads every checkbox as "present means on" -- a
                panel taken out of the document would submit as a row of
                switched-off settings. Nothing here may be disabled for the same
                reason, other than the unit selects that already follow their
                own toggle.
            -->
            <form id="cfg" action="/save" method="POST" class="config-form">

                <div class="tab-panel" id="panel-radar" data-panel="radar"
                     role="tabpanel" aria-labelledby="tab-radar">

                    <fieldset class="config-section">
                        <legend>Clock</legend>
                        <div class="display-option">
                            <label class="display-toggle" for="clock">
                                <span>Show local time</span>
                                <input id="clock" name="clock" type="checkbox" %CLOCK%>
                            </label>
                            <select id="clock-format" name="clock-format" aria-label="Clock format">
                                <option value="24h" %CLOCK_24H_SELECTED%>24-hour</option>
                                <option value="12h" %CLOCK_12H_SELECTED%>AM/PM</option>
                            </select>
                        </div>
                    </fieldset>

                    <fieldset class="config-section">
                        <legend>Location</legend>
                        <p class="section-note">
                            Where the radar is centred. Everything on the display is drawn
                            relative to this point.
                        </p>
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
                               class="underline">OpenStreetMap contributors</a>.
                            The provider is on the Advanced tab.
                        </div>
                        </fieldset>

                        <label class="field-row location-name mt-3">
                            <span>Screen name:</span>
                            <input
                                id="location-name"
                                name="location-name"
                                maxlength="18"
                                value="%LOCATION_NAME%"
                                placeholder="e.g. Ben Gurion">
                        </label>
                        <div class="mt-2 text-xs">Shown at the bottom of the radar display. Place search suggests a short name.</div>
                    </fieldset>

                    <fieldset class="config-section">
                        <legend>Coverage</legend>
                        <p class="section-note">
                            How far out the edge of the radar face reaches, measured north to
                            south from the centre. East to west spans the same number of
                            degrees, which is less ground the further you are from the equator.
                        </p>

                        <div class="radius-row">
                            <input
                                id="radius-range"
                                type="range"
                                min="3"
                                max="278"
                                step="1"
                                value="111"
                                aria-label="Coverage radius">
                            <div class="radius-readout">
                                <output id="radius-readout" for="radius-range">111 km</output>
                                <select id="distance-unit" name="distance-unit" aria-label="Distance units">
                                    <option value="km" %DISTANCE_KM_SELECTED%>km</option>
                                    <option value="mi" %DISTANCE_MI_SELECTED%>miles</option>
                                </select>
                            </div>
                        </div>

                        <!--
                            The stored setting is a half-width in degrees, which is
                            what the projection divides by. The slider is the only
                            thing that writes it, so opening the page and saving
                            without touching anything leaves the stored value exactly
                            as it was rather than re-rounding it through kilometres.
                        -->
                        <input type="hidden" id="radius" name="radius" value="%RADIUS%">

                        <details class="mt-3 text-xs">
                            <summary>Set the radius in degrees</summary>
                            <input
                                id="radius-degrees"
                                type="number"
                                min="0.000001"
                                step="0.000001"
                                max="2.499999"
                                class="mt-2"
                                aria-label="Coverage radius in degrees">
                        </details>
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
                </div>

                <div class="tab-panel" id="panel-setup" data-panel="setup"
                     role="tabpanel" aria-labelledby="tab-setup" hidden>

                    <fieldset class="config-section">
                        <legend>Wi-Fi network</legend>
                        <p class="section-note">%NETWORK_NOTE%</p>

                        <details class="network-details" %NETWORK_OPEN%>
                            <summary>%NETWORK_SUMMARY%</summary>
                            <div class="network-grid mt-3">
                                <div class="ssid-row">
                                    <select
                                        id="wifi-ssid"
                                        name="wifi-ssid"
                                        data-current="%WIFI_SSID%"
                                        aria-label="Available networks">
                                        <option value="">Looking for networks...</option>
                                    </select>
                                    <button
                                        id="rescan-wifi"
                                        type="button">
                                        Rescan
                                    </button>
                                </div>

                                <label class="field-row" id="wifi-manual-row" hidden>
                                    <span>Network name:</span>
                                    <input
                                        id="wifi-ssid-manual"
                                        name="wifi-ssid-manual"
                                        maxlength="32"
                                        autocomplete="off"
                                        spellcheck="false"
                                        disabled
                                        placeholder="Hidden or out-of-range network name">
                                </label>

                                <label class="field-row">
                                    <span>Password:</span>
                                    <input
                                        id="wifi-pass"
                                        name="wifi-pass"
                                        type="password"
                                        maxlength="63"
                                        autocomplete="off"
                                        spellcheck="false"
                                        placeholder="%WIFI_PASS_PLACEHOLDER%">
                                </label>

                                <div class="text-xs">
                                    The radar joins 2.4 GHz networks only. Leave the password blank for an
                                    open network, or to keep the one already stored. Pick "Other network"
                                    from the list to type a hidden network's name.
                                </div>
                                <div id="wifi-result" class="text-sm" aria-live="polite"></div>
                            </div>
                        </details>

                        <div class="network-actions">
                            <button type="button" id="forget-wifi" class="link-button" %FORGET_HIDDEN%>
                                Forget this network
                            </button>
                            <span id="network-action-result" class="text-sm" aria-live="polite"></span>
                        </div>
                    </fieldset>

                    <!--
                        Hidden on the setup hotspot: OpenSky cannot be reached
                        from it, the anonymous allowance covers a first boot, and
                        the credentials have to be fetched from a website the
                        phone filling this in cannot open while it is joined to
                        the radar. Hidden rather than removed, so its two fields
                        still submit and keep whatever is already stored.
                    -->
                    <fieldset class="config-section" %OPENSKY_HIDDEN%>
                        <legend>OpenSky connection</legend>
                        <p class="section-note">
                            Where the aircraft come from. Without credentials the radar uses the
                            anonymous allowance, which is refreshed far less often. To get your own:
                            register a free account at
                            <a href="https://opensky-network.org"
                               target="_blank" rel="noopener" class="underline">opensky-network.org</a>,
                            sign in, then Register. Open
                            <a href="https://opensky-network.org/my-opensky/account"
                               target="_blank" rel="noopener" class="underline">Account &rarr; API client</a>
                            and create a new API client.
                        </p>
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
                            placeholder="%OPENSKY_SECRET_PLACEHOLDER%">
                        </label>
                        </div>
                    </fieldset>
                </div>

                <div class="tab-panel" id="panel-advanced" data-panel="advanced"
                     role="tabpanel" aria-labelledby="tab-advanced" hidden>

                    <fieldset class="config-section">
                        <legend>Firmware</legend>
                        <p class="section-note">
                            The radar checks for a new release once an hour. Installing takes about a
                            minute and ends with a restart, so the display is unavailable while it runs.
                        </p>
                        <div class="display-option display-option-select">
                            <label for="auto-update">When a new release is found</label>
                            <select
                                id="auto-update"
                                name="auto-update"
                                class="update-mode-select"
                                aria-label="Firmware update behaviour">
                                <option value="true" %AUTO_UPDATE_ON_SELECTED%>Install automatically</option>
                                <option value="false" %AUTO_UPDATE_OFF_SELECTED%>Ask me first</option>
                            </select>
                        </div>

                        <div class="firmware-footer">
                            <span class="firmware-version">Firmware %FIRMWARE_VERSION%</span>
                            &middot; released %FIRMWARE_RELEASED%
                            <p class="firmware-notes">%FIRMWARE_NOTES%</p>
                            <p class="firmware-update">
                                <button type="button" id="check-update" class="link-button">Check for updates now</button>
                                <button type="button" id="install-update" class="link-button" hidden>Install now</button>
                                <span id="update-status" aria-live="polite"></span>
                            </p>
                        </div>
                    </fieldset>

                    <fieldset class="config-section">
                        <legend>Panel alignment</legend>
                        <p class="section-note">
                            Leave at 0 unless you can see that the display sits crooked in its
                            bezel. Anything other than 0 slows each frame down.
                        </p>
                        <label class="field-row">
                        <span>Rotation trim (in &deg;):</span>
                        <input
                            name="screen-trim"
                            type="number"
                            min="%SCREEN_TRIM_MIN%"
                            step="0.1"
                            max="%SCREEN_TRIM_MAX%"
                            value='%SCREEN_TRIM%'>
                        </label>

                        <div class="display-option">
                            <label class="display-toggle" for="alignment-test">
                                <span>Show alignment pattern instead of the radar</span>
                                <input id="alignment-test" name="alignment-test" type="checkbox" %ALIGNMENT_TEST%>
                            </label>
                        </div>

                        <details class="mt-3 text-xs">
                            <summary>How to use these two</summary>
                            <p class="mt-2">
                                The trim is for a display whose glass sits slightly crooked in its
                                bezel, so the clock digits and text run a little downhill. It turns
                                the whole picture by this many degrees: positive turns it clockwise,
                                so use a negative value if the right-hand side of a horizontal line
                                sits too low. A quarter turn is not this setting.
                            </p>
                            <p>
                                The alignment pattern puts a crosshair, a ring on the outermost
                                pixels, and a degree scale on the display so you can measure the tilt
                                against whatever the radar is mounted in. Set the trim, save, look,
                                repeat &mdash; then clear the box. The radar shows no aircraft while
                                it is ticked.
                            </p>
                        </details>
                    </fieldset>

                    <fieldset class="config-section">
                        <legend>Place search provider</legend>
                        <p class="section-note">
                            The Nominatim-compatible service the Radar tab's place search asks.
                            Change it only to point at your own instance.
                        </p>
                        <input
                            id="geocoder-url"
                            name="geocoder-url"
                            value="%GEOCODER_URL%"
                            aria-label="Nominatim-compatible place search URL">
                    </fieldset>

                    <fieldset class="config-section">
                        <legend>Remote diagnostics</legend>
                        <p class="section-note">
                            Off unless a key is entered below. When it is on, this radar sends crash
                            reports and error messages to
                            <a href="https://insights.espressif.com"
                               target="_blank" rel="noopener" class="underline">ESP Insights</a>
                            so whoever looks after it can see why it misbehaved without taking it
                            apart. Along with those it reports free memory, uptime, Wi-Fi signal
                            strength, and this network's name and IP address. It does not send your
                            Wi-Fi password, your OpenSky credentials, or the location the radar is
                            centred on. Clear the key to stop all of it.
                        </p>
                        <div class="credentials">
                        <label class="field-row">
                        <span>Insights auth key:</span>
                        <input
                            name="insights-key"
                            type="password"
                            autocomplete="off"
                            spellcheck="false"
                            placeholder="%INSIGHTS_KEY_PLACEHOLDER%">
                        </label>

                        <label class="field-row">
                        <span>Label:</span>
                        <input
                            name="insights-label"
                            autocomplete="off"
                            maxlength="48"
                            placeholder="e.g. kitchen shelf"
                            value='%INSIGHTS_LABEL%'>
                        </label>
                        </div>
                        <p class="section-note mt-3">
                            The label is only there so this radar is recognisable on the dashboard,
                            which otherwise lists it as %NET_MAC%.
                        </p>

                        <div class="display-option">
                            <label class="display-toggle" for="insights-clear">
                                <span>Turn reporting off and forget the key</span>
                                <input id="insights-clear" name="insights-clear" type="checkbox">
                            </label>
                        </div>
                        <p class="section-note mt-3">
                            An empty key box means &ldquo;keep the stored key&rdquo;, so this box is how
                            the key is actually removed. Takes effect on the restart that saving does.
                        </p>
                    </fieldset>

                    <fieldset class="config-section">
                        <legend>This radar</legend>
                        <dl class="network-diagnostics">
                            <dt>Address</dt><dd>%NET_IP%</dd>
                            <dt>Signal</dt><dd>%NET_RSSI%</dd>
                            <dt>MAC</dt><dd>%NET_MAC%</dd>
                            <dt>Uptime</dt><dd>%NET_UPTIME%</dd>
                            <dt>Free memory</dt><dd>%NET_HEAP%</dd>
                        </dl>
                        <div class="network-actions">
                            <button type="button" id="restart-radar" class="link-button">Restart radar</button>
                            <span id="restart-result" class="text-sm" aria-live="polite"></span>
                        </div>
                    </fieldset>

                    <fieldset class="config-section danger-zone">
                        <legend>Factory reset</legend>
                        <p class="section-note">
                            Erases every setting on this page &mdash; network, location, OpenSky
                            credentials, display choices, panel trim and the diagnostics key
                            &mdash; and restarts into the setup hotspot as if the radar had just
                            been flashed. The installed firmware version is not affected. There is
                            no undo.
                        </p>
                        <div class="display-option">
                            <label class="display-toggle" for="factory-reset-confirm">
                                <span>Yes, erase all settings on this radar</span>
                                <input id="factory-reset-confirm" type="checkbox">
                            </label>
                        </div>
                        <div class="danger-action">
                            <button type="button" id="factory-reset" class="danger-button" disabled>
                                Erase and restart
                            </button>
                            <span id="factory-reset-result" class="text-sm" aria-live="polite"></span>
                        </div>
                    </fieldset>
                </div>

                <div class="save-row">
                    <input
                        type="submit"
                        value="%SAVE_LABEL%"
                        class="save-button">

                    <span id="save-state" class="save-state" aria-live="polite"></span>
                    <div id="result" aria-live="polite"></div>
                </div>
            </form>

            <div class="project-footer">
                <span class="firmware-version">Firmware %FIRMWARE_VERSION%</span>
                &middot;
                <a href="https://github.com/thedontknowguy/micro-radar" target="_blank" rel="noopener"
                   class="underline">Micro Radar on GitHub</a>
                <p class="project-credit">
                    A fork of the original
                    <a href="https://github.com/AnthonySturdy/micro-radar" target="_blank" rel="noopener"
                       class="underline">Micro Radar</a>
                    by Anthony Sturdy. Full credit to Anthony for the original project, firmware,
                    enclosure, and design.
                </p>
            </div>
        </fieldset>

        <!--
            The mark the panel itself shows at boot, so the page someone opens
            next is recognisably the same device. A sibling of the panel rather
            than a child of it: it is positioned against the shell, and coming
            after the panel in source order is what puts it on top of the border
            it is there to sit on. Served from /logo.png rather than inlined --
            it is the one asset here worth caching, and it would otherwise be
            re-sent with every reload. Decorative, so the alt text is empty:
            the legend on the opposite corner already names the page.
        -->
        <img class="masthead-mark" src="/logo.png" width="88" height="88"
             alt="" aria-hidden="true">
      </div>

        <script>
            // Setup mode means this page is being served from the radar's own
            // hotspot, so nothing on the far side of the internet is reachable.
            const setupMode = document.body.dataset.netMode === 'setup';

            const tabButtons = Array.from(document.querySelectorAll('.tab-button'));
            const tabPanels = Array.from(document.querySelectorAll('.tab-panel'));

            function showTab(name) {
                tabPanels.forEach(function(panel) {
                    panel.hidden = panel.dataset.panel !== name;
                });
                tabButtons.forEach(function(button) {
                    button.setAttribute(
                        'aria-selected',
                        button.dataset.tab === name ? 'true' : 'false'
                    );
                });
            }

            tabButtons.forEach(function(button) {
                button.addEventListener('click', function() {
                    showTab(button.dataset.tab);
                    // Replaced rather than pushed: the tabs are one page, and
                    // filling the history with them turns Back into a way of
                    // walking sideways instead of leaving.
                    history.replaceState(null, '', '#' + button.dataset.tab);
                });
            });

            // On the hotspot there is nothing to choose between: the network is
            // the only thing worth setting before the radar can reach anything.
            const requestedTab = location.hash.replace('#', '');
            showTab(
                setupMode
                    ? 'setup'
                    : (tabButtons.some(b => b.dataset.tab === requestedTab) ? requestedTab : 'radar')
            );

            const locationButton = document.getElementById('use-location');
            const locationResult = document.getElementById('location-result');
            const placeQuery = document.getElementById('place-query');
            const placeSearchButton = document.getElementById('search-places');
            const placeResults = document.getElementById('place-results');
            const placeResult = document.getElementById('place-result');
            const locationNameInput = document.getElementById('location-name');
            const placeSearchCache = new Map();
            let lastPlaceSearchAt = 0;
            const maxLocationNameLength = 18;

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
            bindDependentSelect('clock', 'clock-format');

            // The radar stores its coverage as a half-width in degrees, because
            // that is what the projection divides by. Degrees are not a distance
            // anyone can picture, so the field people actually touch is a slider
            // in kilometres or miles and the stored value is derived from it.
            // North-south only: the projection applies the same number of
            // degrees to longitude, which is less ground away from the equator.
            const KM_PER_DEGREE = 111.32;
            const MI_PER_KM = 0.621371;
            const MIN_RADIUS_DEGREES = 0.000001;
            const MAX_RADIUS_DEGREES = 2.499999;

            const radiusField = document.getElementById('radius');
            const radiusRange = document.getElementById('radius-range');
            const radiusReadout = document.getElementById('radius-readout');
            const radiusDegrees = document.getElementById('radius-degrees');
            const distanceUnit = document.getElementById('distance-unit');

            function inMiles() {
                return distanceUnit.value === 'mi';
            }

            function distanceFromDegrees(degrees) {
                const km = degrees * KM_PER_DEGREE;
                return inMiles() ? km * MI_PER_KM : km;
            }

            function degreesFromDistance(distance) {
                const km = inMiles() ? distance / MI_PER_KM : distance;
                return Math.min(
                    MAX_RADIUS_DEGREES,
                    Math.max(MIN_RADIUS_DEGREES, km / KM_PER_DEGREE)
                );
            }

            function storedDegrees() {
                const degrees = Number(radiusField.value);
                return degrees > 0 && degrees <= MAX_RADIUS_DEGREES ? degrees : 1;
            }

            // Reads the stored degrees rather than the slider position, so a
            // value that does not land exactly on a whole kilometre is shown and
            // kept as it is. Only an actual drag rewrites the stored value.
            function showRadius() {
                const degrees = storedDegrees();
                const distance = distanceFromDegrees(degrees);

                radiusRange.max = String(Math.floor(distanceFromDegrees(MAX_RADIUS_DEGREES)));
                radiusRange.value = String(Math.round(distance));
                radiusReadout.textContent =
                    (distance < 10 ? distance.toFixed(1) : String(Math.round(distance)))
                    + (inMiles() ? ' miles' : ' km');
                radiusDegrees.value = degrees.toFixed(6);
            }

            radiusRange.addEventListener('input', function() {
                radiusField.value = degreesFromDistance(Number(radiusRange.value)).toFixed(6);
                showRadius();
            });

            // Switching units only changes how the same coverage is written out.
            distanceUnit.addEventListener('change', showRadius);

            radiusDegrees.addEventListener('change', function() {
                const typed = Number(radiusDegrees.value);
                if (!(typed >= MIN_RADIUS_DEGREES && typed <= MAX_RADIUS_DEGREES)) {
                    showRadius();
                    return;
                }
                radiusField.value = typed.toFixed(6);
                showRadius();
            });

            showRadius();

            function fillLocation(latitude, longitude, message, suggestedName) {
                document.getElementById('latitude').value = Number(latitude).toFixed(6);
                document.getElementById('longitude').value = Number(longitude).toFixed(6);
                if (typeof suggestedName === 'string' && suggestedName.trim())
                    locationNameInput.value = compactLocationName(suggestedName);
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
                            'Approximate network location filled' + (area ? ' (' + area + ').' : '.'),
                            area
                        );
                    })
                    .catch(function() {
                        locationResult.textContent =
                            'Approximate location unavailable. Enter coordinates manually.';
                    });
            }

            if (!window.isSecureContext && !setupMode) {
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

            function compactLocationName(value) {
                let cleaned = String(value || '')
                    .replace(/\s+/g, ' ')
                    .replace(/\s*,\s*/g, ', ')
                    .trim();
                if (cleaned.length <= maxLocationNameLength)
                    return cleaned;

                let compacted = cleaned
                    .replace(/\bInternational\b/ig, '')
                    .replace(/\s+/g, ' ')
                    .trim();
                if (compacted.length <= maxLocationNameLength)
                    return compacted;

                compacted = compacted.replace(/\s+Airport\b/i, '').trim();
                if (compacted.length <= maxLocationNameLength)
                    return compacted;

                const words = compacted.split(' ');
                let selected = '';
                for (const word of words) {
                    const next = selected ? selected + ' ' + word : word;
                    if (next.length > maxLocationNameLength)
                        break;
                    selected = next;
                }
                return selected || compacted.slice(0, maxLocationNameLength);
            }

            function firstText(value) {
                return String(value || '')
                    .split(',')[0]
                    .replace(/\s*\([^)]*\)\s*/g, ' ')
                    .replace(/\s+/g, ' ')
                    .trim();
            }

            function placeName(place) {
                const names = place.namedetails || {};
                return firstText(names['name:en'] || names.name || place.name || place.display_name);
            }

            function placeCode(place) {
                const tags = place.extratags || {};
                const code = String(tags.icao || tags.iata || '').trim().toUpperCase();
                return /^[A-Z0-9]{3,5}$/.test(code) ? code : '';
            }

            function suggestLocationName(place) {
                const compacted = compactLocationName(placeName(place));
                if (compacted)
                    return compacted;

                const code = placeCode(place);
                if (code)
                    return code;

                return compactLocationName(place.display_name);
            }

            function showPlaceResults(results) {
                placeResults.replaceChildren();

                results.forEach(function(place) {
                    const option = document.createElement('option');
                    option.textContent = place.display_name;
                    option.dataset.latitude = place.lat;
                    option.dataset.longitude = place.lon;
                    option.dataset.locationName = suggestLocationName(place);
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
                searchUrl.searchParams.set('namedetails', '1');
                searchUrl.searchParams.set('extratags', '1');

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
                    'Selected place coordinates filled.',
                    selected.dataset.locationName
                );
                placeResult.textContent = selected.dataset.locationName
                    ? 'Suggested screen name: ' + selected.dataset.locationName
                    : selected.textContent;
            });

            const networkDetails = document.querySelector('.network-details');
            const ssidSelect = document.getElementById('wifi-ssid');
            const manualSsid = document.getElementById('wifi-ssid-manual');
            const manualRow = document.getElementById('wifi-manual-row');
            const rescanButton = document.getElementById('rescan-wifi');
            const wifiResult = document.getElementById('wifi-result');
            let scanPolls = 0;
            let scanStarted = false;

            // Sentinel value of the list entry that reveals the typed-name box.
            // The radar knows to read it as "the name is in the other field".
            const OTHER_SSID = '__other__';

            // Kept disabled while hidden so a leftover name cannot be submitted
            // after switching back to a network from the list.
            function syncManualSsid(focusInput) {
                const other = ssidSelect.value === OTHER_SSID;
                manualRow.hidden = !other;
                manualSsid.disabled = !other;
                if (!other)
                    manualSsid.value = '';
                else if (focusInput)
                    manualSsid.focus();
            }

            ssidSelect.addEventListener('change', function() { syncManualSsid(true); });

            function signalWords(rssi) {
                if (rssi >= -55) return 'excellent';
                if (rssi >= -67) return 'good';
                if (rssi >= -75) return 'fair';
                return 'weak';
            }

            function showNetworks(networks) {
                const current = ssidSelect.dataset.current || '';
                ssidSelect.replaceChildren();

                const blank = document.createElement('option');
                blank.value = '';
                blank.textContent = networks.length ? 'Select a network' : 'No networks found';
                ssidSelect.appendChild(blank);

                let sawCurrent = false;
                networks.forEach(function(network) {
                    const option = document.createElement('option');
                    option.value = network.ssid;
                    option.textContent = network.ssid + ' - signal ' + signalWords(network.rssi)
                        + (network.secure ? '' : ', open');
                    option.selected = network.ssid === current;
                    sawCurrent = sawCurrent || network.ssid === current;
                    ssidSelect.appendChild(option);
                });

                const other = document.createElement('option');
                other.value = OTHER_SSID;
                other.textContent = 'Other network...';
                ssidSelect.appendChild(other);

                // A stored network that is hidden or out of range never appears
                // in the scan, so it starts out as the typed name instead of
                // silently reading as "no network chosen".
                if (current && !sawCurrent) {
                    other.selected = true;
                    syncManualSsid(false);
                    manualSsid.value = current;
                } else {
                    syncManualSsid(false);
                }

                wifiResult.textContent = networks.length
                    ? ''
                    : 'Nothing in range. Choose "Other network..." to type the name if it is hidden.';
            }

            // The radar answers the first request by starting a scan and saying
            // so; the results arrive on a later one. Scanning takes a few
            // seconds and cannot be done on the web server's task.
            function pollScan() {
                fetch('/wifi/scan', { cache: 'no-store' })
                    .then(r => r.json())
                    .then(function(data) {
                        if (data.scanning) {
                            scanPolls += 1;
                            if (scanPolls > 20) {
                                wifiResult.textContent =
                                    'The scan is taking too long. Press Rescan to try again.';
                                rescanButton.disabled = false;
                                return;
                            }
                            setTimeout(pollScan, 1000);
                            return;
                        }
                        showNetworks(data.networks || []);
                        rescanButton.disabled = false;
                    })
                    .catch(function() {
                        wifiResult.textContent = 'Could not reach the radar to scan.';
                        rescanButton.disabled = false;
                    });
            }

            function scanNetworks() {
                scanStarted = true;
                scanPolls = 0;
                rescanButton.disabled = true;
                wifiResult.textContent = 'Looking for networks...';
                pollScan();
            }

            rescanButton.addEventListener('click', scanNetworks);

            // A scan drops the station connection for a moment, which would cut
            // off the very page requesting it. In normal operation it therefore
            // waits until someone opens the section to change networks.
            if (setupMode) {
                scanNetworks();
            } else {
                networkDetails.addEventListener('toggle', function() {
                    if (networkDetails.open && !scanStarted)
                        scanNetworks();
                });
            }

            const forgetButton = document.getElementById('forget-wifi');
            const restartButton = document.getElementById('restart-radar');
            const factoryResetButton = document.getElementById('factory-reset');
            const factoryResetConfirm = document.getElementById('factory-reset-confirm');
            const networkActionResult = document.getElementById('network-action-result');
            const restartResult = document.getElementById('restart-result');
            const factoryResetResult = document.getElementById('factory-reset-result');

            // All of these end in a reboot, so the connection dropping is the
            // expected finish rather than a failure worth reporting. Every one
            // of them is disabled together: once the radar is on its way down,
            // none of the others can be honoured either.
            function postDeviceAction(url, result, pending, done) {
                result.textContent = pending;
                forgetButton.disabled = true;
                restartButton.disabled = true;
                factoryResetButton.disabled = true;
                fetch(url, { method: 'POST' })
                    .then(function() { result.textContent = done; })
                    .catch(function() { result.textContent = done; });
            }

            forgetButton.addEventListener('click', function() {
                if (!confirm('Forget the stored Wi-Fi network? The radar restarts into its setup hotspot.'))
                    return;
                postDeviceAction(
                    '/wifi/forget',
                    networkActionResult,
                    'Forgetting...',
                    'Restarting into setup mode. Connect to the MicroRadar-Setup hotspot.'
                );
            });

            restartButton.addEventListener('click', function() {
                if (!confirm('Restart the radar now?'))
                    return;
                postDeviceAction(
                    '/restart',
                    restartResult,
                    'Restarting...',
                    'Restarting. Reload this page in a minute.'
                );
            });

            // Two gates rather than one, and the checkbox clears itself after
            // the button is armed only by an explicit tick: this is the one
            // control on the page that cannot be undone by pressing Save again.
            factoryResetConfirm.addEventListener('change', function() {
                factoryResetButton.disabled = !factoryResetConfirm.checked;
            });

            factoryResetButton.addEventListener('click', function() {
                if (!factoryResetConfirm.checked)
                    return;
                if (!confirm('Erase every setting and restart into the setup hotspot? This cannot be undone.'))
                    return;
                postDeviceAction(
                    '/factory-reset',
                    factoryResetResult,
                    'Erasing...',
                    'Erased. Restarting into setup mode - connect to the MicroRadar-Setup hotspot.'
                );
            });

            const checkButton = document.getElementById('check-update');
            const installButton = document.getElementById('install-update');
            const updateStatus = document.getElementById('update-status');
            let updatePolls = 0;

            function showUpdateStatus(text) {
                updateStatus.textContent = text ? ' - ' + text : '';
            }

            // In "Ask me first" mode a found release sits there until someone
            // presses Install, so the button follows the radar's own view of
            // whether it is waiting rather than anything remembered here.
            function applyUpdateState(state) {
                showUpdateStatus(state.message);
                installButton.hidden = !state.awaiting;
                if (state.awaiting)
                    installButton.disabled = false;
            }

            function pollUpdateStatus() {
                fetch('/update/status', { cache: 'no-store' })
                    .then(r => r.json())
                    .then(function(state) {
                        applyUpdateState(state);
                        updatePolls += 1;

                        // Anything else means the radar is still working on it.
                        // Downloading deliberately keeps polling until the
                        // reboot cuts the connection.
                        const settled = state.status === 'up-to-date'
                            || state.status === 'failed'
                            || state.status === 'idle'
                            || state.awaiting;

                        if (settled || updatePolls > 60) {
                            checkButton.disabled = false;
                            return;
                        }
                        setTimeout(pollUpdateStatus, 2000);
                    })
                    .catch(function() {
                        // Installing ends with a restart, so losing the radar
                        // mid-poll is the expected finish, not a failure.
                        showUpdateStatus('radar restarting, reload this page in a minute');
                        checkButton.disabled = false;
                    });
            }

            checkButton.addEventListener('click', function() {
                checkButton.disabled = true;
                installButton.hidden = true;
                updatePolls = 0;
                showUpdateStatus('checking...');
                fetch('/update/check', { method: 'POST' })
                    .then(() => setTimeout(pollUpdateStatus, 1500))
                    .catch(function() {
                        showUpdateStatus('could not reach the radar');
                        checkButton.disabled = false;
                    });
            });

            installButton.addEventListener('click', function() {
                installButton.disabled = true;
                checkButton.disabled = true;
                updatePolls = 0;
                showUpdateStatus('starting install...');
                fetch('/update/install', { method: 'POST' })
                    .then(() => setTimeout(pollUpdateStatus, 1500))
                    .catch(function() {
                        showUpdateStatus('could not reach the radar');
                        installButton.disabled = false;
                        checkButton.disabled = false;
                    });
            });

            // Every control that has to reach past the radar itself: browser
            // geolocation and its IP fallback, the place search, and the update
            // check. On the setup hotspot they would all simply time out, so
            // say why instead of letting them be pressed.
            if (setupMode) {
                const offlineNote = 'Available once the radar has joined your network.';
                [
                    [locationButton, locationResult],
                    [placeSearchButton, placeResult],
                    [checkButton, updateStatus]
                ].forEach(function(control) {
                    control[0].disabled = true;
                    control[0].classList.add('is-offline');
                    control[1].textContent = offlineNote;
                });
                placeQuery.disabled = true;
            }

            // A release found by the hourly check may already be waiting when
            // this page is opened, so show that without making anyone press
            // "check" to rediscover it.
            if (!setupMode) {
                fetch('/update/status', { cache: 'no-store' })
                    .then(r => r.json())
                    .then(function(state) {
                        if (state.awaiting)
                            applyUpdateState(state);
                    })
                    .catch(function() {});
            }

            const configForm = document.getElementById('cfg');
            const saveState = document.getElementById('save-state');

            // One Save covers all three tabs, so a change made on a tab that is
            // no longer showing is easy to forget about. This is the only thing
            // on the page that says the form has been touched at all.
            function markDirty() {
                saveState.textContent = 'Unsaved changes on this page';
                saveState.classList.add('is-dirty');
            }

            configForm.addEventListener('input', markDirty);
            configForm.addEventListener('change', markDirty);

            // A number outside its min/max stops submission before the submit
            // event ever fires, and the browser cannot focus a field inside a
            // hidden panel -- so the press would do nothing at all and say
            // nothing about why. Capture, because this has to run before the
            // browser gives up trying to focus it.
            configForm.addEventListener('invalid', function(e) {
                const panel = e.target.closest('.tab-panel');
                if (panel && panel.hidden)
                    showTab(panel.dataset.panel);
            }, true);

            configForm.addEventListener('submit', function(e) {
                e.preventDefault();

                // Saving from the hotspot with no network chosen would reboot
                // straight back into the hotspot, having stored everything else
                // but still with nowhere to go.
                const chosenSsid = ssidSelect.value === OTHER_SSID
                    ? manualSsid.value.trim()
                    : ssidSelect.value;
                if (setupMode && !chosenSsid) {
                    showTab('setup');
                    networkDetails.open = true;
                    wifiResult.textContent = 'Choose a network, or type its name, before saving.';
                    return;
                }

                fetch(this.action, { method: 'POST', body: new FormData(this) })
                    .then(r => r.text())
                    .then(function(html) {
                        document.getElementById('result').innerHTML = html;
                        saveState.textContent = '';
                        saveState.classList.remove('is-dirty');
                    });
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

    // Empty means "no network known", which is what sends the radar to its
    // setup hotspot on the next boot.
    ensureKey("wifi-ssid", "");
    ensureKey("wifi-pass", "");

    // Left behind by firmware that finished first setup in two passes: the
    // Wi-Fi portal, a reboot, then a hold on the configuration screen. The page
    // now takes the network and the settings in one submission, so the flag has
    // nothing left to mean. Removed rather than ignored so it cannot be read
    // back by mistake later.
    if (prefs.isKey("setup-pending"))
        prefs.remove("setup-pending");

    // A radar centred on nothing shows nothing, and an empty first screen reads
    // as a broken device rather than an unconfigured one. Ben Gurion is busy
    // enough that traffic appears within a sweep or two, which is the quickest
    // way to prove the unit works before the owner sets their own centre.
    ensureKey("latitude", "32.002714");
    ensureKey("longitude", "34.880919");
    ensureKey("location-name", "Ben Gurion Airport");
    ensureKey("radius", "1.0");

    // Presentation only: the radius is stored in degrees either way, because
    // that is what the projection divides by. This is which unit the page shows
    // it in, kept on the radar so it is the same on every phone that opens it.
    ensureKey("distance-unit", "km");
    ensureKey("opensky-id", "");
    ensureKey("opensky-secret", "");

    // Empty means remote diagnostics stay off, which is the only safe default
    // for a radar that will live in someone else's house -- see Diagnostics.h.
    // It also keeps the key out of the published firmware image, where it would
    // be readable by anyone who downloads a release.
    ensureKey("insights-key", "");
    ensureKey("insights-label", "");
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
    ensureKey("destination", "true");
    ensureKey("wind", "true");

    // Off by default: the clock is the largest thing on the face after the
    // radar itself, and a radar that shows one without being asked reads as a
    // clock with aircraft on it.
    ensureKey("clock", "false");
    ensureKey("clock-format", "24h");

    // Automatic by default: an unattended radar that quietly keeps itself
    // current is the behaviour most owners want, and the alternative leaves
    // security fixes waiting on someone opening this page.
    ensureKey("auto-update", "true");

    // Square by default, because almost every module is. This one is per-unit
    // calibration rather than a preference -- it is the only setting here whose
    // right value is a property of the physical panel in front of the owner.
    ensureKey("screen-trim", "0");

    // Never on by default, and not something a firmware update should ever turn
    // on: it replaces the radar face entirely, so a unit that came up showing it
    // would read as broken rather than as being calibrated.
    ensureKey("alignment-test", "false");

    prefs.end();
}

void ConfigurationWebServer::Initialise(FirmwareUpdater& updater, Mode serveMode) {
    // Stored before any route is registered: the handlers below reach it
    // through `this`, so capturing the parameter by reference would leave them
    // pointing at a stack frame that is gone by the first request.
    firmwareUpdater = &updater;
    mode = serveMode;

    EnsureDefaults();

    // The radar gets whatever address the router hands it, and that address can
    // change between boots -- mDNS is what makes the configuration page findable
    // without going and looking it up. The IP still works and is still what the
    // boot screen leads with, because a household with no mDNS resolver (some
    // Android, some locked-down networks) has nothing else to go on.
    if (!MDNS.begin(MdnsHostname)) {
        Serial.println("[WARN] Failed to start mDNS. Continuing without mDNS...");
    }
    else {
        // Without the service record the name resolves but the radar does not
        // show up in anything that browses for HTTP servers.
        MDNS.addService("http", "tcp", port);
        Serial.printf("mDNS responder started: http://%s\n", MdnsAddress);
    }

    if (mode == Mode::Setup) {
        // Every lookup resolves to the radar. Together with the redirect in
        // onNotFound below, this is what makes a phone joining the hotspot open
        // the configuration page on its own.
        dns.setErrorReplyCode(DNSReplyCode::NoError);
        dns.start(53, "*", SetupPortalIp);
    }

    // Handle visit to config web server
    server.on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Serial.println("[GET] Handling request to config web server...");

        const bool setupMode = mode == Mode::Setup;

        // read all values up front so the processor lambda can capture by value
        prefs.begin("config", true);
        PageValues values;
        values.latitude = EscapeHtmlAttribute(prefs.getString("latitude", ""));
        values.longitude = EscapeHtmlAttribute(prefs.getString("longitude", ""));
        values.locationName = EscapeHtmlAttribute(prefs.getString("location-name", ""));
        values.radius = EscapeHtmlAttribute(prefs.getString("radius", "1.0"));
        values.distanceUnit = prefs.getString("distance-unit", "km");
        values.openskyClientId = EscapeHtmlAttribute(prefs.getString("opensky-id", ""));
        const bool hasOpenskySecret = !prefs.getString("opensky-secret", "").isEmpty();
        values.geocoderUrl = EscapeHtmlAttribute(
            prefs.getString("geocoder-url", "https://nominatim.openstreetmap.org/search")
        );
        values.scanlineEnabled = prefs.getString("scanline", "true");
        values.sweepPeriod = prefs.getString("sweep-period", "5");
        values.speedEnabled = prefs.getString("speed", "true");
        values.speedUnit = prefs.getString("speed-unit", "knots");
        values.altitudeEnabled = prefs.getString("altitude", "true");
        values.altitudeUnit = prefs.getString("altitude-unit", "feet");
        values.destinationEnabled = prefs.getString("destination", "false");
        values.windEnabled = prefs.getString("wind", "false");
        values.clockEnabled = prefs.getString("clock", "false");
        values.clockFormat = prefs.getString("clock-format", "24h");
        values.aircraftMarker = prefs.getString("aircraft-marker", "radar");
        values.autoUpdate = prefs.getString("auto-update", "true");
        values.screenTrim = EscapeHtmlAttribute(prefs.getString("screen-trim", "0"));
        values.alignmentTest = prefs.getString("alignment-test", "false");
        const bool hasInsightsKey = !prefs.getString("insights-key", "").isEmpty();
        values.insightsLabel = EscapeHtmlAttribute(prefs.getString("insights-label", ""));
        const String storedSsid = prefs.getString("wifi-ssid", "");
        prefs.end();

        // Same contract as the OpenSky secret below: the stored key never goes
        // back to the page, so an empty field on save means "keep it". The
        // wording doubles as the on/off indicator -- it is the only place the
        // page can honestly say whether reporting is running.
        values.insightsKeyPlaceholder = hasInsightsKey
            ? "Reporting is on - leave blank to keep the stored key"
            : "Paste a key to turn reporting on";

        // Sent as a placeholder, never as a value. A masked value would still
        // have to be told apart from a real one on save, and the previous
        // "contains an asterisk" test failed exactly that job.
        values.openskySecretPlaceholder = hasOpenskySecret
            ? "Leave blank to keep the stored secret"
            : "Client secret";

        // The Wi-Fi password is never sent back, masked or otherwise: an empty
        // field means "leave the stored one alone", which also spares the page
        // the guesswork the OpenSky secret needs on save.
        values.wifiSsid = EscapeHtmlAttribute(storedSsid);
        values.setupMode = setupMode ? "setup" : "station";
        values.networkOpen = setupMode ? "open" : "";
        values.networkSummary = "Network settings";
        values.forgetHidden = setupMode ? "hidden" : "";
        values.saveLabel = setupMode ? "Save and connect" : "Save";
        values.tabsHidden = setupMode ? "hidden" : "";
        values.openskyHidden = setupMode ? "hidden" : "";
        values.wifiPassPlaceholder = setupMode
            ? "Network password"
            : "Leave blank to keep the stored password";
        values.networkNote = setupMode
            ? "The radar is serving this page from its own hotspot. Pick your network below "
              "and set everything else on this page at the same time -- saving joins the "
              "network and starts the radar."
            : (storedSsid.length()
                ? "Connected to " + EscapeHtmlAttribute(storedSsid) + "."
                : String("Connected."));

        // Stands in for the separate info page the old Wi-Fi portal had.
        values.netIp = setupMode
            ? WiFi.softAPIP().toString() + " (setup hotspot)"
            : WiFi.localIP().toString();
        values.netRssi = setupMode ? String("access point mode") : String(WiFi.RSSI()) + " dBm";
        values.netMac = WiFi.macAddress();
        values.netUptime = FormatUptime(millis());
        values.netHeap = String(ESP.getFreeHeap() / 1024) + " KB free";

        // template processor called once per %PLACEHOLDER% token found in CONFIG_HTML.
        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html",
            (const uint8_t*)CONFIG_HTML, sizeof(CONFIG_HTML) - 1,
            [values](const String& var) -> String {
                if (var == "NET_MODE")       return values.setupMode;
                if (var == "TABS_HIDDEN")    return values.tabsHidden;
                if (var == "OPENSKY_HIDDEN") return values.openskyHidden;
                if (var == "NETWORK_NOTE")   return values.networkNote;
                if (var == "NETWORK_OPEN")   return values.networkOpen;
                if (var == "NETWORK_SUMMARY") return values.networkSummary;
                if (var == "WIFI_SSID")      return values.wifiSsid;
                if (var == "WIFI_PASS_PLACEHOLDER") return values.wifiPassPlaceholder;
                if (var == "FORGET_HIDDEN")  return values.forgetHidden;
                if (var == "SAVE_LABEL")     return values.saveLabel;
                if (var == "NET_IP")         return values.netIp;
                if (var == "NET_RSSI")       return values.netRssi;
                if (var == "NET_MAC")        return values.netMac;
                if (var == "NET_UPTIME")     return values.netUptime;
                if (var == "NET_HEAP")       return values.netHeap;

                if (var == "LATITUDE")       return values.latitude;
                if (var == "LONGITUDE")      return values.longitude;
                if (var == "LOCATION_NAME")  return values.locationName;
                if (var == "RADIUS")         return values.radius;
                if (var == "DISTANCE_KM_SELECTED") return values.distanceUnit != "mi" ? "selected" : "";
                if (var == "DISTANCE_MI_SELECTED") return values.distanceUnit == "mi" ? "selected" : "";
                if (var == "OPENSKY_ID")     return values.openskyClientId;
                if (var == "OPENSKY_SECRET_PLACEHOLDER") return values.openskySecretPlaceholder;
                if (var == "INSIGHTS_KEY_PLACEHOLDER") return values.insightsKeyPlaceholder;
                if (var == "INSIGHTS_LABEL")  return values.insightsLabel;
                if (var == "GEOCODER_URL")   return values.geocoderUrl;
                if (var == "SCANLINE")       return values.scanlineEnabled == "true" ? "checked" : "";
                if (var == "SWEEP_2_SELECTED") return values.sweepPeriod == "2" ? "selected" : "";
                if (var == "SWEEP_5_SELECTED") return values.sweepPeriod == "5" ? "selected" : "";
                if (var == "SWEEP_10_SELECTED") return values.sweepPeriod == "10" ? "selected" : "";
                if (var == "SWEEP_18_SELECTED") return values.sweepPeriod == "18" ? "selected" : "";
                if (var == "SWEEP_30_SELECTED") return values.sweepPeriod == "30" ? "selected" : "";
                if (var == "SPEED")          return values.speedEnabled == "true" ? "checked" : "";
                if (var == "SPEED_KNOTS_SELECTED") return values.speedUnit == "knots" ? "selected" : "";
                if (var == "SPEED_MS_SELECTED") return values.speedUnit == "meters-second" ? "selected" : "";
                if (var == "ALTITUDE")       return values.altitudeEnabled == "true" ? "checked" : "";
                if (var == "ALTITUDE_FEET_SELECTED") return values.altitudeUnit == "feet" || values.altitudeUnit == "kft" ? "selected" : "";
                if (var == "ALTITUDE_METERS_SELECTED") return values.altitudeUnit == "meters" ? "selected" : "";
                if (var == "DESTINATION")    return values.destinationEnabled == "true" ? "checked" : "";
                if (var == "WIND")           return values.windEnabled == "true" ? "checked" : "";
                if (var == "CLOCK")          return values.clockEnabled == "true" ? "checked" : "";
                if (var == "CLOCK_24H_SELECTED") return values.clockFormat != "12h" ? "selected" : "";
                if (var == "CLOCK_12H_SELECTED") return values.clockFormat == "12h" ? "selected" : "";
                if (var == "MARKER_RADAR_SELECTED") return values.aircraftMarker == "radar" ? "selected" : "";
                if (var == "MARKER_TRIANGLE_SELECTED") return values.aircraftMarker == "triangle" ? "selected" : "";
                if (var == "MARKER_DOT_SELECTED") return values.aircraftMarker == "dot" ? "selected" : "";
                if (var == "AUTO_UPDATE_ON_SELECTED")  return values.autoUpdate != "false" ? "selected" : "";
                if (var == "AUTO_UPDATE_OFF_SELECTED") return values.autoUpdate == "false" ? "selected" : "";
                if (var == "SCREEN_TRIM")    return values.screenTrim;
                if (var == "ALIGNMENT_TEST") return values.alignmentTest == "true" ? "checked" : "";
                if (var == "SCREEN_TRIM_MIN") return String(-SCREEN_TRIM_MAX_DEGREES, 1);
                if (var == "SCREEN_TRIM_MAX") return String(SCREEN_TRIM_MAX_DEGREES, 1);

                // Describes the build that is actually running, which after an
                // over-the-air update is not necessarily the one that was
                // flashed over USB. Escaped because the notes are free text
                // edited by hand in FirmwareVersion.h.
                if (var == "FIRMWARE_VERSION")  return EscapeHtmlAttribute(FIRMWARE_VERSION);
                if (var == "FIRMWARE_RELEASED") return EscapeHtmlAttribute(FIRMWARE_RELEASED);
                if (var == "FIRMWARE_NOTES")    return EscapeHtmlAttribute(FIRMWARE_NOTES);
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

    // The masthead mark and the favicon, which are one file. It is the only
    // thing this server hands out that is worth letting a browser keep: the
    // page itself is deliberately never cached, but a phone on the setup
    // hotspot should not be pulling 10 KB of artwork back over an ESP32 access
    // point on every reload. It only ever changes with the firmware, and a
    // firmware update reboots the radar, so a long expiry costs nothing.
    server.on("/logo.png", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(
            200, "image/png", WebLogo::Png, WebLogo::Length
        );
        response->addHeader("Cache-Control", "public, max-age=604800, immutable");
        request->send(response);
    });

    // Network list for the Wi-Fi section. A scan takes several seconds, which
    // is far too long to hold the web server's task, so the first request
    // starts one and answers "scanning"; the page asks again until results
    // appear. Consuming them frees the scan, so the next first request starts a
    // fresh one -- which is exactly what the Rescan button wants.
    server.on("/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest* request) {
        const int16_t found = WiFi.scanComplete();

        if (found == WIFI_SCAN_RUNNING) {
            request->send(200, "application/json", "{\"scanning\":true}");
            return;
        }

        if (found < 0) {
            Serial.println("[GET] Starting Wi-Fi scan");
            WiFi.scanNetworks(true, false);
            request->send(200, "application/json", "{\"scanning\":true}");
            return;
        }

        // Strongest first, and only once per name: a mesh network answers from
        // every node it has, and a list of six identical names is no use to
        // anyone choosing one. Read each entry out of the driver once -- the
        // previous pass called WiFi.SSID() inside its comparison loop, which
        // allocated and freed a String for every pair.
        struct ScannedNetwork {
            String ssid;
            int32_t rssi;
            bool secure;
        };

        std::vector<ScannedNetwork> networks;
        networks.reserve(found);
        for (int16_t i = 0; i < found; ++i) {
            String ssid = WiFi.SSID(i);
            if (ssid.isEmpty())
                continue;

            const int32_t rssi = WiFi.RSSI(i);
            auto existing = std::find_if(
                networks.begin(),
                networks.end(),
                [&](const ScannedNetwork& seen) { return seen.ssid == ssid; }
            );

            // Keep the strongest node of a mesh rather than the first seen.
            if (existing != networks.end()) {
                if (rssi > existing->rssi)
                    existing->rssi = rssi;
                continue;
            }

            networks.push_back({
                std::move(ssid),
                rssi,
                WiFi.encryptionType(i) != WIFI_AUTH_OPEN
            });
        }
        WiFi.scanDelete();

        std::sort(
            networks.begin(),
            networks.end(),
            [](const ScannedNetwork& a, const ScannedNetwork& b) { return a.rssi > b.rssi; }
        );

        String body = "{\"scanning\":false,\"networks\":[";
        for (const auto& network : networks) {
            if (body.endsWith("}"))
                body += ',';
            body += "{\"ssid\":\"" + EscapeJsonString(network.ssid) +
                    "\",\"rssi\":" + String(network.rssi) +
                    ",\"secure\":" + (network.secure ? "true" : "false") + '}';
        }
        body += "]}";

        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", body);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    // Drops the stored network and reboots, which lands in setup mode. The
    // credentials the Wi-Fi driver keeps in its own NVS entry are deliberately
    // left alone -- they are only ever read by the one-shot import in main.cpp,
    // which the flag written here permanently disables.
    server.on("/wifi/forget", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (RejectedAsCrossSite(request))
            return;

        Serial.println("[POST] Forgetting stored Wi-Fi network");
        prefs.begin("config", false);
        prefs.putString("wifi-ssid", "");
        prefs.putString("wifi-pass", "");
        prefs.putString("wifi-imported", "true");
        prefs.end();

        request->send(200, "text/plain", "Wi-Fi forgotten - restarting into setup mode");
        restartRequested.store(true);
    });

    // Erases every setting and reboots, which lands in setup mode with the
    // defaults EnsureDefaults() writes back on the way up. The firmware image
    // itself is untouched: this is a settings reset, not a downgrade.
    server.on("/factory-reset", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (RejectedAsCrossSite(request))
            return;

        Serial.println("[POST] Factory reset requested from the configuration page");
        prefs.begin("config", false);
        prefs.clear();

        // Written back immediately after the wipe, for the same reason
        // /wifi/forget writes it: the Wi-Fi driver keeps its own copy of the
        // last network joined, and clearing this flag re-arms the one-shot
        // import in WiFiConnection.cpp. A reset that let that run would put the
        // radar straight back on the network it was just reset off.
        prefs.putString("wifi-imported", "true");
        prefs.end();

        request->send(200, "text/plain", "Settings erased - restarting into setup mode");
        restartRequested.store(true);
    });

    server.on("/restart", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (RejectedAsCrossSite(request))
            return;

        Serial.println("[POST] Restart requested from the configuration page");
        request->send(200, "text/plain", "Restarting");
        restartRequested.store(true);
    });

    // On the setup hotspot every unrecognised path is a phone checking whether
    // the network has internet: /generate_204 on Android, /hotspot-detect.html
    // on iOS and macOS, /connecttest.txt and /ncsi.txt on Windows. Answering
    // with a redirect rather than the expected body is what makes each of them
    // pop the configuration page up by itself.
    server.onNotFound([this](AsyncWebServerRequest* request) {
        if (mode != Mode::Setup) {
            request->send(404, "text/plain", "Not found");
            return;
        }

        AsyncWebServerResponse* response = request->beginResponse(302, "text/plain", "");
        response->addHeader("Location", String("http://") + SetupPortalIp.toString() + "/");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    // Ask the updater to look now instead of waiting for its hourly tick. This
    // returns as soon as the request is queued -- the manifest fetch needs a
    // TLS handshake, which does not belong on the web server's task -- so the
    // page polls /update/status afterwards to find out how it went.
    server.on("/update/check", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (RejectedAsCrossSite(request))
            return;

        Serial.println("[POST] Manual update check requested");
        if (firmwareUpdater != nullptr)
            firmwareUpdater->RequestCheckNow();
        request->send(202, "application/json", "{\"accepted\":true}");
    });

    // Installs the release the last check found. Only reachable in "ask me
    // first" mode -- with automatic updates the render loop has already taken
    // it -- so this is the owner answering that question.
    server.on("/update/install", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (RejectedAsCrossSite(request))
            return;

        Serial.println("[POST] Manual firmware install requested");
        const bool accepted = firmwareUpdater != nullptr && firmwareUpdater->RequestInstallNow();
        request->send(accepted ? 202 : 409, "application/json",
                      accepted ? "{\"accepted\":true}"
                               : "{\"accepted\":false,\"reason\":\"no release waiting\"}");
    });

    server.on("/update/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        String body = "{\"status\":\"idle\",\"message\":\"\",\"awaiting\":false}";
        if (firmwareUpdater != nullptr) {
            const FirmwareUpdater::StatusSnapshot snapshot = firmwareUpdater->CurrentStatus();
            body = String("{\"status\":\"") + FirmwareUpdater::StatusName(snapshot.status) +
                   "\",\"message\":\"" + EscapeJsonString(snapshot.message) +
                   "\",\"awaiting\":" + (snapshot.awaitingConfirmation ? "true" : "false") + "}";
        }

        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", body);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    // Handle save submission to web server
    server.on("/save", HTTP_POST, [&](AsyncWebServerRequest* request) {
        if (RejectedAsCrossSite(request))
            return;

        Serial.println("[POST] Handling form submission to config web server...");

        // Returns the submitted value, or an empty String when the field was
        // not part of this submission. An absent field always means "leave the
        // stored value alone" -- every validated save below reads that way.
        auto submitted = [request](const char* paramName) {
            const auto* param = request->getParam(paramName, true);
            return param != nullptr ? param->value() : String("");
        };

        // Stores only when the field was submitted and `accept` likes it. A
        // rejected value leaves the previous setting in place rather than
        // writing something the radar cannot use.
        auto saveIf = [&](const char* paramName, auto accept) {
            const String value = submitted(paramName);
            if (!value.isEmpty() && accept(value))
                prefs.putString(paramName, value);
        };

        // Same contract as saveIf, for the settings whose valid values are a
        // fixed list. Taking the list directly rather than returning a
        // predicate that captures it keeps its lifetime obviously in scope.
        auto saveIfOneOf = [&](const char* paramName, std::initializer_list<const char*> allowed) {
            const String value = submitted(paramName);
            for (const char* option : allowed) {
                if (value == option) {
                    prefs.putString(paramName, value);
                    return;
                }
            }
        };

        prefs.begin("config", false);

        // Radius must stay clear of zero: it is the divisor in the coordinate
        // projection. The upper bounds are OpenSky's own bounding-box limits.
        saveIf("latitude", [](const String& v) { return IsNumberInRange(v, -90.0, 90.0); });
        saveIf("longitude", [](const String& v) { return IsNumberInRange(v, -180.0, 180.0); });
        saveIf("radius", [](const String& v) { return IsNumberInRange(v, 0.000001, 2.499999); });
        saveIf("screen-trim", [](const String& v) {
            return IsNumberInRange(v, -SCREEN_TRIM_MAX_DEGREES, SCREEN_TRIM_MAX_DEGREES);
        });
        saveIf("geocoder-url", IsHttpUrl);
        saveIfOneOf("distance-unit", { "km", "mi" });
        saveIfOneOf("speed-unit", { "knots", "meters-second" });
        saveIfOneOf("altitude-unit", { "feet", "meters", "kft" });
        saveIfOneOf("sweep-period", { "2", "5", "10", "18", "30" });
        saveIfOneOf("clock-format", { "24h", "12h" });
        saveIfOneOf("aircraft-marker", { "radar", "triangle", "dot" });
        saveIfOneOf("auto-update", { "true", "false" });

        // Free text rather than a fixed set, so these two are stored whenever
        // submitted -- including empty, which is how the OpenSky client id is
        // cleared to go back to the anonymous allowance.
        const auto* openskyIdParam = request->getParam("opensky-id", true);
        if (openskyIdParam != nullptr)
            prefs.putString("opensky-id", openskyIdParam->value());

        const auto* locationNameParam = request->getParam("location-name", true);
        if (locationNameParam != nullptr)
            prefs.putString("location-name", CleanLocationName(locationNameParam->value()));

        // The stored secret is never sent to the page, so an empty field means
        // "keep it" -- the same contract the Wi-Fi password uses. The page used
        // to receive a row of asterisks and skip any value containing one,
        // which quietly made a secret with a '*' in it impossible to save.
        const String openskySecret = submitted("opensky-secret");
        if (!openskySecret.isEmpty())
            prefs.putString("opensky-secret", openskySecret);

        // Same "empty means keep" contract as the secret above, which leaves no
        // way to express "stop reporting" -- hence the explicit checkbox. It is
        // read first so that ticking the box and typing a key in the same
        // submission ends with the key stored, which is the reading that
        // matches what the person doing it can see on screen.
        if (request->hasParam("insights-clear", true))
            prefs.putString("insights-key", "");

        const String insightsKey = submitted("insights-key");
        if (!insightsKey.isEmpty())
            prefs.putString("insights-key", insightsKey);

        // Free text, stored whenever submitted -- including empty, which is how
        // a label is removed.
        const auto* insightsLabelParam = request->getParam("insights-label", true);
        if (insightsLabelParam != nullptr) {
            String label = insightsLabelParam->value();
            label.trim();
            prefs.putString("insights-label", label.substring(0, 48));
        }

        // A name typed into the "other network" box wins over the scan list, so
        // a hidden network can be joined without it ever appearing there. The
        // list's own "Other network" entry is that box's placeholder rather than
        // a network name, so it never becomes one on its own.
        String ssid = submitted("wifi-ssid-manual");
        ssid.trim();
        if (ssid.isEmpty()) {
            ssid = submitted("wifi-ssid");
            if (ssid == "__other__")
                ssid = "";
        }

        if (!ssid.isEmpty()) {
            const String previousSsid = prefs.getString("wifi-ssid", "");
            prefs.putString("wifi-ssid", ssid);

            // The page never receives the stored password, so an empty field is
            // "leave it alone" -- unless the network itself is changing, where
            // keeping the old network's password would be nonsense and empty is
            // how an open network is entered.
            const String password = submitted("wifi-pass");
            if (!password.isEmpty() || ssid != previousSsid)
                prefs.putString("wifi-pass", password);

            // Whatever the Wi-Fi driver still has stored from a WiFiManager-era
            // build is now stale. See /wifi/forget.
            prefs.putString("wifi-imported", "true");
        }

        prefs.putString("scanline", request->hasParam("scanline", true) ? "true" : "false");
        prefs.putString("speed", request->hasParam("speed", true) ? "true" : "false");
        prefs.putString("altitude", request->hasParam("altitude", true) ? "true" : "false");
        prefs.putString("destination", request->hasParam("destination", true) ? "true" : "false");
        prefs.putString("wind", request->hasParam("wind", true) ? "true" : "false");
        prefs.putString("clock", request->hasParam("clock", true) ? "true" : "false");
        prefs.putString("alignment-test", request->hasParam("alignment-test", true) ? "true" : "false");
        prefs.end();

        request->send(200, "text/html",
            mode == Mode::Setup
                ? "Saved - joining " + EscapeHtmlAttribute(ssid) +
                  ". This hotspot disappears now; reconnect to your own network and open "
                  "http://" + String(MdnsAddress) +
                  " -- the radar also shows its address on screen when it comes back up."
                : String("Saved - restarting device..."));

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

void ConfigurationWebServer::SaveWiFiCredentials(const String& ssid, const String& password)
{
    prefs.begin("config", false);
    prefs.putString("wifi-ssid", ssid);
    prefs.putString("wifi-pass", password);
    prefs.putString("wifi-imported", "true");
    prefs.end();
}

void ConfigurationWebServer::PumpCaptivePortal()
{
    if (mode == Mode::Setup)
        dns.processNextRequest();
}
