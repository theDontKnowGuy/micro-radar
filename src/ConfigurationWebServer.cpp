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
    String groundTrafficEnabled;
    String clockEnabled;
    String aircraftMarker, autoUpdate, screenTrim, alignmentTest;
    String insightsKeyPlaceholder, insightsLabel;

    // Wi-Fi section and the mode it is being shown in.
    String setupMode, wifiSsid, wifiPassPlaceholder;
    String networkNote, networkOpen, networkSummary, forgetHidden, saveLabel;
    String netIp, netRssi, netMac, netUptime, netHeap, netStatus;

    // Group rail. Hidden on the setup hotspot, where the network is the only
    // thing worth setting and the other groups have nothing to offer yet.
    String navHidden, openskyHidden;
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
        <title>Micro Radar Configuration</title>
        <link rel="icon" type="image/png" href="/logo.png">
        <style>
            :root {
                color-scheme: dark;
                --ground: rgb(5 11 10);
                --surface: rgb(10 21 19);
                --card: rgb(13 29 25);
                --field: rgb(7 18 17);
                --hair: rgb(122 226 172 / .13);
                --hair-strong: rgb(122 226 172 / .24);
                --text: rgb(216 239 227);
                --dim: rgb(127 164 146);
                --faint: rgb(85 115 100);
                --phos: rgb(91 231 155);
                --phos-deep: rgb(47 169 108);
                --amber: rgb(232 194 94);
                --clay: rgb(232 121 107);

                /* Width of the control column every row's input lands in. One
                   number rather than a width on each control is what keeps the
                   right-hand edge straight down a card of mixed inputs. */
                --control: 15rem;
                --rail: 16.5rem;
                --app-radius: 1rem;
            }
            * {
                box-sizing: border-box;
            }
            html {
                background: var(--ground);
            }
            body {
                margin: 0;
                min-height: 100vh;
                padding: 1.5rem 1rem 3rem;
                background:
                    radial-gradient(900px 420px at 15rem 0rem, rgb(91 231 155 / .07), transparent 70rem),
                    var(--ground);
                color: var(--text);
                font-family:
                    ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont,
                    "Segoe UI", sans-serif;
                font-size: 15px;
                line-height: 1.5;
                -webkit-font-smoothing: antialiased;
            }
            h1, h2, p, dl, dd {
                margin: 0;
            }
            button, input, select {
                font: inherit;
                color: inherit;
            }
            a {
                color: var(--phos-deep);
            }

            /* ---- frame ---- */

            .app {
                width: min(1180px, calc(100vw - 2rem));
                margin: 0 auto;
                border: 1px solid var(--hair-strong);
                border-radius: var(--app-radius);
                background: var(--surface);
                box-shadow: 0 28px 80px rgb(0 0 0 / .45);
            }
            .topbar {
                display: flex;
                align-items: center;
                gap: .8rem;
                padding: .85rem 1.15rem;
                border-bottom: 1px solid var(--hair);
                border-radius: var(--app-radius) var(--app-radius) 0 0;
                background: linear-gradient(180deg, rgb(17 38 34 / .55), transparent);
            }
            .brand-mark {
                display: block;
                width: 2.1rem;
                height: 2.1rem;
                flex: none;
                border-radius: 50rem;
            }
            .brand-name {
                display: block;
                font-weight: 600;
                font-size: .95rem;
                letter-spacing: -.015em;
            }
            .brand-sub {
                display: block;
                color: var(--faint);
                font-size: .72rem;
            }
            .link-status {
                display: flex;
                align-items: center;
                gap: .45rem;
                margin-left: auto;
                color: var(--dim);
                font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
                font-size: .72rem;
                text-align: right;
            }
            .pip {
                width: .45rem;
                height: .45rem;
                flex: none;
                border-radius: 50rem;
                background: var(--phos);
                box-shadow: 0 0 8px var(--phos);
            }
            body[data-net-mode="setup"] .pip {
                background: var(--amber);
                box-shadow: 0 0 8px var(--amber);
            }
            .shell {
                display: grid;
                grid-template-columns: var(--rail) minmax(0, 1fr);
            }
            .shell.is-single {
                grid-template-columns: minmax(0, 1fr);
            }

            /* ---- rail ---- */

            .rail {
                display: flex;
                flex-direction: column;
                gap: .15rem;
                padding: .9rem .7rem;
                border-right: 1px solid var(--hair);
                border-bottom-left-radius: var(--app-radius);
                background: rgb(7 17 15 / .5);
            }
            .rail[hidden] {
                display: none;
            }
            .rail-search {
                width: auto;
                margin-bottom: .7rem;
                padding: .45rem .6rem;
                border: 1px solid var(--hair);
                border-radius: .45rem;
                background: var(--field);
                font-size: .82rem;
            }
            .nav {
                display: flex;
                align-items: center;
                gap: .6rem;
                border: 0;
                border-left: 2px solid transparent;
                border-radius: .4rem;
                padding: .5rem .6rem;
                background: none;
                color: var(--dim);
                font-size: .88rem;
                text-align: left;
                cursor: pointer;
                transition:
                    background-color 140ms ease,
                    color 140ms ease;
            }
            .nav:hover {
                background: rgb(91 231 155 / .05);
                color: var(--text);
            }
            .nav:focus-visible {
                outline: 2px solid var(--phos);
                outline-offset: -2px;
            }
            .nav[aria-current="true"] {
                border-left-color: var(--phos);
                background: rgb(91 231 155 / .13);
                color: var(--text);
                font-weight: 600;
            }
            .nav svg {
                width: 1rem;
                height: 1rem;
                flex: none;
                fill: none;
                stroke: currentColor;
                stroke-width: 1.5;
                opacity: .75;
            }
            .nav[aria-current="true"] svg {
                color: var(--phos);
                opacity: 1;
            }
            /* Counts what has been touched in a group that may not be showing.
               One Save covers every group, so a change made two groups ago has
               to stay visible from wherever the next one is made. */
            .nav-count {
                display: none;
                place-items: center;
                min-width: 1.2rem;
                height: 1.2rem;
                margin-left: auto;
                border-radius: 50rem;
                background: var(--amber);
                color: rgb(32 24 11);
                font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
                font-size: .66rem;
                font-weight: 600;
            }
            .nav.has-changes .nav-count {
                display: grid;
            }
            .rail-foot {
                margin-top: auto;
                padding: .8rem .6rem 0;
                border-top: 1px solid var(--hair);
                color: var(--faint);
                font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
                font-size: .68rem;
                line-height: 1.7;
            }

            /* ---- content ---- */

            .content {
                display: flex;
                flex-direction: column;
                min-width: 0;
                padding: 1.35rem 1.5rem 0;
            }
            .config-form {
                display: flex;
                flex-direction: column;
                flex: 1;
                min-width: 0;
            }
            .group-panel {
                display: flex;
                flex-direction: column;
                gap: 1rem;
                padding-bottom: 1.25rem;
            }
            /* display: flex above would otherwise beat the hidden attribute. */
            .group-panel[hidden] {
                display: none;
            }
            .group-head h2 {
                font-size: 1.2rem;
                font-weight: 600;
                letter-spacing: -.02em;
                text-wrap: balance;
            }
            .group-head p {
                max-width: 62ch;
                margin-top: .15rem;
                color: var(--dim);
                font-size: .87rem;
            }

            .card {
                min-width: 0;
                border: 1px solid var(--hair);
                border-radius: .7rem;
                overflow: hidden;
                background: var(--card);
            }
            .card[hidden] {
                display: none;
            }
            .card-title {
                padding: .6rem 1rem .5rem;
                border-bottom: 1px solid var(--hair);
                background: rgb(91 231 155 / .025);
                color: var(--faint);
                font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
                font-size: .64rem;
                font-weight: 500;
                letter-spacing: .16em;
                text-transform: uppercase;
            }

            /* One grammar for every setting on the page: what it is and one
               line about it on the left, the control right-aligned in the
               fixed column on the right. */
            .row {
                display: grid;
                grid-template-columns: minmax(0, 1fr) var(--control);
                align-items: center;
                gap: 1.25rem;
                padding: .8rem 1rem;
            }
            .row + .row,
            .row + .network-details,
            .network-details + .row {
                border-top: 1px solid var(--hair);
            }
            .row.stack {
                grid-template-columns: minmax(0, 1fr);
                gap: .55rem;
            }
            /* display: grid above would otherwise beat the hidden attribute. */
            .row[hidden] {
                display: none;
            }
            /* A setting that only applies while the toggle above it is on.
               Indented and ticked so it reads as subordinate before it is
               switched off, not only afterwards. */
            .row.is-dependent {
                padding-left: 2rem;
                background: rgb(91 231 155 / .018);
            }
            .row.is-dependent .row-name::before {
                position: absolute;
                top: .68em;
                left: -1rem;
                width: .6rem;
                height: 1px;
                background: var(--hair-strong);
                content: "";
            }
            .row-label {
                display: block;
                min-width: 0;
                cursor: pointer;
            }
            .row-name {
                position: relative;
                font-size: .92rem;
                font-weight: 500;
            }
            .row-note {
                max-width: 54ch;
                margin-top: .1rem;
                color: var(--faint);
                font-size: .79rem;
            }
            .row-control {
                display: flex;
                align-items: center;
                justify-content: flex-end;
                gap: .5rem;
                min-width: 0;
            }
            .row.is-off .row-control {
                opacity: .35;
            }
            .row.is-off .row-name {
                color: var(--dim);
            }
            .mono {
                font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
                font-variant-numeric: tabular-nums;
            }

            /* ---- controls ---- */

            select,
            input:not([type="checkbox"]):not([type="range"]) {
                width: auto;
                min-width: 0;
                border: 1px solid var(--hair-strong);
                border-radius: .45rem;
                outline: none;
                background: var(--field);
                color: var(--text);
                padding: .45rem .55rem;
                font-size: .85rem;
                transition:
                    border-color 140ms ease,
                    box-shadow 140ms ease;
            }
            .row-control > select,
            .row-control > input:not([type="checkbox"]) {
                flex: 1 1 auto;
            }
            select:hover,
            input:hover {
                border-color: rgb(122 226 172 / .36);
            }
            select:focus,
            input:focus {
                border-color: var(--phos-deep);
                box-shadow: 0 0 0 3px rgb(91 231 155 / .11);
            }
            input::placeholder {
                color: var(--faint);
            }
            .switch {
                position: relative;
                width: 2.4rem;
                height: 1.32rem;
                flex: none;
                margin: 0;
                border: 1px solid rgb(122 226 172 / .28);
                border-radius: 50rem;
                appearance: none;
                background: rgb(18 40 36);
                cursor: pointer;
                transition:
                    border-color 160ms ease,
                    background-color 160ms ease;
            }
            .switch::after {
                position: absolute;
                top: .16rem;
                left: .18rem;
                width: .86rem;
                height: .86rem;
                border-radius: 50rem;
                background: rgb(127 164 146);
                content: "";
                transition:
                    transform 160ms ease,
                    background-color 160ms ease;
            }
            .switch:checked {
                border-color: var(--phos);
                background: var(--phos-deep);
            }
            .switch:checked::after {
                background: rgb(234 255 243);
                transform: translateX(1.05rem);
            }
            .switch:focus-visible {
                outline: none;
                box-shadow: 0 0 0 3px rgb(91 231 155 / .18);
            }
            .ghost {
                flex: none;
                border: 1px solid var(--hair-strong);
                border-radius: .45rem;
                background: none;
                color: var(--dim);
                padding: .42rem .7rem;
                font-size: .82rem;
                white-space: nowrap;
                cursor: pointer;
                transition:
                    color 140ms ease,
                    border-color 140ms ease,
                    background-color 140ms ease;
            }
            .ghost:hover {
                border-color: var(--phos-deep);
                background: rgb(91 231 155 / .06);
                color: var(--phos);
            }
            .ghost:focus-visible {
                outline: 2px solid var(--phos);
                outline-offset: 2px;
            }
            .ghost:disabled,
            .ghost.is-offline {
                opacity: .4;
                cursor: not-allowed;
            }
            .ghost.danger {
                border-color: rgb(232 121 107 / .38);
                color: var(--clay);
            }
            .ghost.danger:hover {
                border-color: var(--clay);
                background: rgb(232 121 107 / .1);
                color: rgb(243 171 160);
            }
            .ghost.danger:disabled:hover {
                border-color: rgb(232 121 107 / .38);
                background: none;
                color: var(--clay);
            }
            .link-button {
                border: 0;
                padding: 0;
                background: none;
                color: var(--phos);
                font: inherit;
                font-size: .82rem;
                text-decoration: underline;
                cursor: pointer;
            }
            .link-button:hover {
                filter: brightness(1.15);
            }
            .link-button:disabled,
            .link-button.is-offline {
                color: var(--faint);
                text-decoration: none;
                cursor: not-allowed;
            }
            .status-text {
                color: var(--faint);
                font-size: .79rem;
            }
            .status-text:not(:empty) {
                margin-top: .35rem;
            }

            /* ---- composite controls ---- */

            .pair {
                display: grid;
                grid-template-columns: repeat(2, minmax(0, 1fr));
                gap: .5rem;
            }
            .search-row {
                display: grid;
                grid-template-columns: minmax(0, 1fr) auto;
                gap: .5rem;
            }
            .place-results {
                grid-column: 1 / -1;
                min-height: 7rem;
            }
            .radius-row {
                display: flex;
                align-items: center;
                gap: 1rem;
            }
            /* The generic input rule dresses every non-checkbox input as a text
               box, which would draw a border and padding around a slider. */
            input[type="range"] {
                flex: 1 1 auto;
                width: auto;
                min-width: 0;
                border: 0;
                padding: 0;
                background: none;
                accent-color: var(--phos);
            }
            .radius-readout {
                min-width: 5rem;
                color: var(--phos);
                font-size: .85rem;
                font-weight: 600;
                text-align: right;
                white-space: nowrap;
            }
            .radius-unit {
                flex: none;
                width: 6rem;
            }
            .stat-grid {
                display: grid;
                grid-template-columns: repeat(auto-fit, minmax(9rem, 1fr));
                gap: .1rem 1.5rem;
                padding: .85rem 1rem;
            }
            .stat-grid > div {
                display: flex;
                flex-direction: column;
                padding: .3rem 0;
            }
            .stat-grid dt {
                color: var(--faint);
                font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
                font-size: .66rem;
                letter-spacing: .13em;
                text-transform: uppercase;
            }
            .stat-grid dd {
                font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
                font-size: .85rem;
            }

            /* The Wi-Fi picker stays a details element: opening it is what
               starts the scan, and a scan drops the station connection for a
               moment -- which would cut off the very page asking for it. */
            .network-details > summary {
                display: grid;
                grid-template-columns: minmax(0, 1fr) auto;
                align-items: center;
                gap: 1.25rem;
                padding: .8rem 1rem;
                cursor: pointer;
                list-style: none;
            }
            /* display: grid above would otherwise beat the hidden attribute
               the search puts on this row. */
            .network-details > summary[hidden] {
                display: none;
            }
            .network-details > summary::-webkit-details-marker {
                display: none;
            }
            .network-details > summary:hover {
                background: rgb(91 231 155 / .04);
            }
            .network-details > summary:focus-visible {
                outline: 2px solid var(--phos);
                outline-offset: -2px;
            }
            .summary-action {
                border: 1px solid var(--hair-strong);
                border-radius: .45rem;
                padding: .42rem .7rem;
                color: var(--dim);
                font-size: .82rem;
                white-space: nowrap;
            }
            .network-details[open] > summary .summary-action {
                border-color: var(--phos-deep);
                color: var(--phos);
            }
            .network-body {
                display: grid;
                gap: .7rem;
                padding: .2rem 1rem 1rem;
            }
            .ssid-row {
                display: grid;
                grid-template-columns: minmax(0, 1fr) auto;
                gap: .5rem;
            }
            .stacked-field {
                display: grid;
                gap: .3rem;
            }
            /* Same again: the typed-name field is hidden until the list's
               "Other network" entry is chosen. */
            .stacked-field[hidden] {
                display: none;
            }
            .stacked-field > span {
                color: var(--dim);
                font-size: .82rem;
            }
            .fine-print {
                color: var(--faint);
                font-size: .76rem;
            }
            .card.danger {
                border-color: rgb(232 121 107 / .3);
            }
            .card.danger .card-title {
                border-bottom-color: rgb(232 121 107 / .22);
                background: rgb(232 121 107 / .05);
                color: var(--clay);
            }
            details.help {
                padding: 0 1rem .8rem;
                color: var(--faint);
                font-size: .79rem;
            }
            details.help > summary {
                color: var(--dim);
                cursor: pointer;
            }
            details.help p {
                max-width: 66ch;
                margin-top: .5rem;
            }

            /* ---- save ---- */

            .no-matches {
                padding: 2rem 0 1rem;
                color: var(--faint);
                font-size: .87rem;
            }
            .no-matches[hidden] {
                display: none;
            }
            .save-row {
                position: sticky;
                bottom: 0;
                z-index: 2;
                display: flex;
                align-items: center;
                gap: .85rem;
                margin: auto -1.5rem 0;
                padding: .8rem 1.5rem;
                border-top: 1px solid var(--hair);
                background: rgb(10 21 19 / .97);
                backdrop-filter: blur(6px);
            }
            .save-button {
                flex: none;
                border: 0;
                border-radius: .5rem;
                background: rgb(26 43 39);
                color: var(--faint);
                padding: .55rem 1.15rem;
                font-weight: 600;
                font-size: .88rem;
                cursor: pointer;
                transition:
                    background-color 160ms ease,
                    color 160ms ease,
                    box-shadow 160ms ease;
            }
            .save-row.is-dirty .save-button {
                background: var(--phos);
                color: rgb(4 23 14);
                box-shadow: 0 6px 20px rgb(91 231 155 / .2);
            }
            .save-row.is-dirty .save-button:hover {
                filter: brightness(1.07);
            }
            .save-button:focus-visible {
                outline: 2px solid var(--phos);
                outline-offset: 2px;
            }
            .save-state {
                color: var(--faint);
                font-size: .82rem;
            }
            .save-row.is-dirty .save-state {
                color: var(--amber);
            }
            #result {
                color: var(--phos);
                font-size: .82rem;
            }
            .save-note {
                margin-left: auto;
                color: var(--faint);
                font-size: .74rem;
                text-align: right;
            }
            .project-footer {
                padding: .9rem 0 1.1rem;
                border-top: 1px solid var(--hair);
                color: var(--faint);
                font-size: .76rem;
                line-height: 1.6;
            }
            .project-footer a {
                color: var(--phos-deep);
            }

            @media (max-width: 900px) {
                :root {
                    --control: 11.5rem;
                }
                .shell {
                    grid-template-columns: minmax(0, 1fr);
                }
                .rail {
                    flex-direction: row;
                    gap: .35rem;
                    overflow-x: auto;
                    padding: .6rem;
                    border-right: 0;
                    border-bottom: 1px solid var(--hair);
                    border-bottom-left-radius: 0;
                }
                .rail-search,
                .rail-foot {
                    display: none;
                }
                .nav {
                    border-bottom: 2px solid transparent;
                    border-left: 0;
                    white-space: nowrap;
                }
                .nav[aria-current="true"] {
                    border-bottom-color: var(--phos);
                }
            }
            @media (max-width: 620px) {
                body {
                    padding: .75rem .5rem 2rem;
                }
                :root {
                    --app-radius: .8rem;
                }
                .app {
                    width: min(1180px, calc(100vw - 1rem));
                }
                .content {
                    padding: 1rem .85rem 0;
                }
                .row,
                .network-details > summary {
                    grid-template-columns: minmax(0, 1fr);
                    gap: .55rem;
                }
                .row-control {
                    justify-content: flex-start;
                }
                .radius-row {
                    flex-wrap: wrap;
                }
                .ssid-row,
                .search-row {
                    grid-template-columns: minmax(0, 1fr);
                }
                .place-results {
                    grid-column: 1;
                }
                .save-row {
                    flex-wrap: wrap;
                    margin: auto -.85rem 0;
                    padding: .8rem .85rem;
                }
                .save-note {
                    display: none;
                }
            }
            @media (prefers-reduced-motion: reduce) {
                * {
                    transition-duration: 1ms;
                }
            }
        </style>
    </head>
    <body data-net-mode="%NET_MODE%">
      <div class="app">

        <header class="topbar">
            <!--
                The mark the panel itself shows at boot, so the page someone
                opens next is recognisably the same device. Served from
                /logo.png rather than inlined: it is the one asset here worth
                caching, and it would otherwise be re-sent with every reload.
            -->
            <img class="brand-mark" src="/logo.png" width="34" height="34" alt="" aria-hidden="true">
            <div>
                <span class="brand-name">Micro Radar</span>
                <span class="brand-sub">Configuration</span>
            </div>
            <div class="link-status"><span class="pip" aria-hidden="true"></span>%NET_STATUS%</div>
        </header>

        <div class="shell" id="shell">

            <nav class="rail" id="rail" aria-label="Settings groups" %NAV_HIDDEN%>
                <input
                    id="setting-search"
                    class="rail-search"
                    type="search"
                    placeholder="Search settings"
                    autocomplete="off"
                    aria-label="Search settings">

                <button type="button" class="nav" data-group="location" aria-controls="panel-location" aria-current="true">
                    <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="2.5"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3"/></svg>
                    Location &amp; range<span class="nav-count" aria-hidden="true">0</span>
                </button>
                <button type="button" class="nav" data-group="display" aria-controls="panel-display">
                    <svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="4" width="18" height="13" rx="2"/><path d="M8 21h8M12 17v4"/></svg>
                    Display<span class="nav-count" aria-hidden="true">0</span>
                </button>
                <button type="button" class="nav" data-group="connection" aria-controls="panel-connection">
                    <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M5 12.5a10 10 0 0 1 14 0M8 16a5.5 5.5 0 0 1 8 0"/><circle cx="12" cy="19.5" r="1"/></svg>
                    Connection<span class="nav-count" aria-hidden="true">0</span>
                </button>
                <button type="button" class="nav" data-group="device" aria-controls="panel-device">
                    <svg viewBox="0 0 24 24" aria-hidden="true"><rect x="4" y="4" width="16" height="16" rx="2"/><path d="M9 9h6v6H9zM2 9h2M2 15h2M20 9h2M20 15h2M9 2v2M15 2v2M9 20v2M15 20v2"/></svg>
                    Firmware &amp; panel<span class="nav-count" aria-hidden="true">0</span>
                </button>
                <button type="button" class="nav" data-group="about" aria-controls="panel-about">
                    <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/></svg>
                    This radar<span class="nav-count" aria-hidden="true">0</span>
                </button>

                <div class="rail-foot">
                    Firmware %FIRMWARE_VERSION%<br>
                    %FIRMWARE_RELEASED%
                </div>
            </nav>

            <div class="content">

            <!--
                One form across every group, and one Save. The groups hide their
                panels with the hidden attribute rather than removing them,
                because /save reads every checkbox as "present means on" -- a
                panel taken out of the document would submit as a row of
                switched-off settings. The same goes for a row the search box
                filters out. Nothing here may be disabled for the same reason,
                other than the selects that already follow their own toggle.
            -->
            <form id="cfg" action="/save" method="POST" class="config-form">

                <!-- ============ LOCATION & RANGE ============ -->
                <section class="group-panel" id="panel-location" data-group="location">
                    <div class="group-head">
                        <h2>Location &amp; range</h2>
                        <p>
                            Where the radar is centred and how far out the face reaches.
                            Everything on the display is drawn relative to this point.
                        </p>
                    </div>

                    <div class="card">
                        <h3 class="card-title">Centre point</h3>

                        <div class="row" data-terms="latitude longitude coordinates position">
                            <div>
                                <div class="row-name">Coordinates</div>
                                <div class="row-note">Latitude and longitude, in decimal degrees.</div>
                            </div>
                            <div class="row-control pair">
                                <input
                                    name="latitude"
                                    id="latitude"
                                    class="mono"
                                    type="number"
                                    min="-90"
                                    step="0.000001"
                                    max="90"
                                    aria-label="Latitude"
                                    value='%LATITUDE%'>
                                <input
                                    name="longitude"
                                    id="longitude"
                                    class="mono"
                                    type="number"
                                    min="-180"
                                    step="0.000001"
                                    max="180"
                                    aria-label="Longitude"
                                    value='%LONGITUDE%'>
                            </div>
                        </div>

                        <div class="row stack" data-terms="find place search airport city landmark address geolocation">
                            <div class="search-row">
                                <input
                                    id="place-query"
                                    type="search"
                                    placeholder="Find an airport, city, landmark or address"
                                    autocomplete="off"
                                    aria-label="Find a place">
                                <button id="search-places" type="button" class="ghost">Search</button>
                                <select
                                    id="place-results"
                                    class="place-results"
                                    size="5"
                                    hidden
                                    aria-label="Place search results"></select>
                            </div>
                            <div class="fine-print">
                                Or
                                <button id="use-location" type="button" class="link-button">use my current location</button>.
                                Search data &copy;
                                <a href="https://www.openstreetmap.org/copyright" target="_blank" rel="noopener">OpenStreetMap contributors</a>.
                            </div>
                            <div id="place-result" class="status-text" aria-live="polite"></div>
                            <div id="location-result" class="status-text" aria-live="polite"></div>
                        </div>

                        <div class="row" data-terms="screen name label caption">
                            <label class="row-label" for="location-name">
                                <div class="row-name">Screen name</div>
                                <div class="row-note">
                                    Shown at the bottom of the radar face. Place search suggests a
                                    short one. 18 characters.
                                </div>
                            </label>
                            <div class="row-control">
                                <input
                                    id="location-name"
                                    name="location-name"
                                    maxlength="18"
                                    value="%LOCATION_NAME%"
                                    aria-label="Screen name"
                                    placeholder="e.g. Ben Gurion">
                            </div>
                        </div>
                    </div>

                    <div class="card">
                        <h3 class="card-title">Coverage</h3>

                        <div class="row stack" data-terms="coverage radius range distance km miles degrees">
                            <div class="radius-row">
                                <input
                                    id="radius-range"
                                    type="range"
                                    min="3"
                                    max="278"
                                    step="1"
                                    value="111"
                                    aria-label="Coverage radius">
                                <output id="radius-readout" class="radius-readout mono" for="radius-range">111 km</output>
                                <select id="distance-unit" name="distance-unit" class="radius-unit" aria-label="Distance units">
                                    <option value="km" %DISTANCE_KM_SELECTED%>km</option>
                                    <option value="mi" %DISTANCE_MI_SELECTED%>miles</option>
                                </select>
                            </div>
                            <div class="row-note">
                                Measured north to south from the centre. East to west spans the same
                                number of degrees, which is less ground the further you are from the
                                equator.
                            </div>

                            <!--
                                The stored setting is a half-width in degrees, which is
                                what the projection divides by. The slider is the only
                                thing that writes it, so opening the page and saving
                                without touching anything leaves the stored value exactly
                                as it was rather than re-rounding it through kilometres.
                            -->
                            <input type="hidden" id="radius" name="radius" value="%RADIUS%">
                        </div>

                        <details class="help">
                            <summary>Set the radius in degrees</summary>
                            <p>
                                <input
                                    id="radius-degrees"
                                    class="mono"
                                    type="number"
                                    min="0.000001"
                                    step="0.000001"
                                    max="2.499999"
                                    aria-label="Coverage radius in degrees">
                            </p>
                        </details>
                    </div>
                </section>

                <!-- ============ DISPLAY ============ -->
                <section class="group-panel" id="panel-display" data-group="display" hidden>
                    <div class="group-head">
                        <h2>Display</h2>
                        <p>What the scope draws, and how it draws it.</p>
                    </div>

                    <div class="card">
                        <h3 class="card-title">Local time</h3>

                        <div class="row" data-terms="clock time local">
                            <label class="row-label" for="clock">
                                <div class="row-name">Show local time</div>
                                <div class="row-note">A clock in the centre of the scope.</div>
                            </label>
                            <div class="row-control">
                                <input id="clock" name="clock" class="switch" type="checkbox"
                                       aria-label="Show local time" %CLOCK%>
                            </div>
                        </div>
                    </div>

                    <div class="card">
                        <h3 class="card-title">Sweep</h3>

                        <div class="row" data-terms="sweep scanline animation trace">
                            <label class="row-label" for="scanline">
                                <div class="row-name">Animated radar sweep</div>
                                <div class="row-note">The rotating trace. Off leaves a static face.</div>
                            </label>
                            <div class="row-control">
                                <input id="scanline" name="scanline" class="switch" type="checkbox"
                                       aria-label="Animated radar sweep" %SCANLINE%>
                            </div>
                        </div>

                        <div class="row is-dependent" id="row-sweep-period" data-terms="sweep speed revolution seconds">
                            <div><div class="row-name">Sweep speed</div></div>
                            <div class="row-control">
                                <select id="sweep-period" name="sweep-period" aria-label="Radar sweep speed">
                                    <option value="2" %SWEEP_2_SELECTED%>Very fast &middot; 2 s/rev</option>
                                    <option value="5" %SWEEP_5_SELECTED%>Fast &middot; 5 s/rev</option>
                                    <option value="10" %SWEEP_10_SELECTED%>Balanced &middot; 10 s/rev</option>
                                    <option value="18" %SWEEP_18_SELECTED%>Classic &middot; 18 s/rev</option>
                                    <option value="30" %SWEEP_30_SELECTED%>Slow &middot; 30 s/rev</option>
                                </select>
                            </div>
                        </div>

                        <div class="row" data-terms="aircraft symbol marker triangle dot block vector">
                            <div><div class="row-name">Aircraft symbol</div></div>
                            <div class="row-control">
                                <select id="aircraft-marker" name="aircraft-marker" aria-label="Aircraft symbol">
                                    <option value="radar" %MARKER_RADAR_SELECTED%>Radar block + vector</option>
                                    <option value="triangle" %MARKER_TRIANGLE_SELECTED%>Aircraft triangle</option>
                                    <option value="dot" %MARKER_DOT_SELECTED%>Simple dot</option>
                                </select>
                            </div>
                        </div>
                    </div>

                    <div class="card">
                        <h3 class="card-title">On the face</h3>

                        <div class="row" data-terms="ground traffic taxiing parked">
                            <label class="row-label" for="ground-traffic">
                                <div class="row-name">Aircraft on the ground</div>
                                <div class="row-note">Taxiing and parked traffic inside the coverage circle.</div>
                            </label>
                            <div class="row-control">
                                <input id="ground-traffic" name="ground-traffic" class="switch" type="checkbox"
                                       aria-label="Show aircraft on the ground" %GROUND_TRAFFIC%>
                            </div>
                        </div>

                        <div class="row" data-terms="wind surface weather">
                            <label class="row-label" for="wind">
                                <div class="row-name">Surface wind at the centre</div>
                                <div class="row-note">Direction and speed at the centre point.</div>
                            </label>
                            <div class="row-control">
                                <input id="wind" name="wind" class="switch" type="checkbox"
                                       aria-label="Show centre surface wind" %WIND%>
                            </div>
                        </div>
                    </div>

                    <div class="card">
                        <h3 class="card-title">Aircraft labels &mdash; callsigns are always shown</h3>

                        <div class="row" data-terms="speed knots velocity">
                            <div><div class="row-name">Speed</div></div>
                            <div class="row-control">
                                <input id="speed" name="speed" class="switch" type="checkbox"
                                       aria-label="Show speed" %SPEED%>
                            </div>
                        </div>
                        <div class="row is-dependent" id="row-speed-unit" data-terms="speed units knots metres per second">
                            <div><div class="row-name">Speed units</div></div>
                            <div class="row-control">
                                <select id="speed-unit" name="speed-unit" aria-label="Speed units">
                                    <option value="knots" %SPEED_KNOTS_SELECTED%>Knots</option>
                                    <option value="meters-second" %SPEED_MS_SELECTED%>m/s</option>
                                </select>
                            </div>
                        </div>

                        <div class="row" data-terms="altitude height flight level">
                            <div><div class="row-name">Altitude</div></div>
                            <div class="row-control">
                                <input id="altitude" name="altitude" class="switch" type="checkbox"
                                       aria-label="Show altitude" %ALTITUDE%>
                            </div>
                        </div>
                        <div class="row is-dependent" id="row-altitude-unit" data-terms="altitude units feet metres">
                            <div><div class="row-name">Altitude units</div></div>
                            <div class="row-control">
                                <select id="altitude-unit" name="altitude-unit" aria-label="Altitude units">
                                    <option value="feet" %ALTITUDE_FEET_SELECTED%>Feet</option>
                                    <option value="meters" %ALTITUDE_METERS_SELECTED%>Metres</option>
                                </select>
                            </div>
                        </div>

                        <div class="row" data-terms="route destination origin">
                            <label class="row-label" for="destination">
                                <div class="row-name">Route</div>
                                <div class="row-note">Origin and destination, when the network reports them.</div>
                            </label>
                            <div class="row-control">
                                <input id="destination" name="destination" class="switch" type="checkbox"
                                       aria-label="Show route when available" %DESTINATION%>
                            </div>
                        </div>
                    </div>
                </section>

                <!-- ============ CONNECTION ============ -->
                <section class="group-panel" id="panel-connection" data-group="connection" hidden>
                    <div class="group-head">
                        <h2>Connection</h2>
                        <p>The network the radar joins, and the services it asks for aircraft and place names.</p>
                    </div>

                    <div class="card">
                        <h3 class="card-title">Wi-Fi</h3>

                        <details class="network-details" %NETWORK_OPEN%>
                            <summary data-terms="wifi network ssid password join hotspot">
                                <div>
                                    <div class="row-name">%NETWORK_SUMMARY%</div>
                                    <div class="row-note">%NETWORK_NOTE%</div>
                                </div>
                                <span class="summary-action">Change network</span>
                            </summary>

                            <div class="network-body">
                                <div class="ssid-row">
                                    <select
                                        id="wifi-ssid"
                                        name="wifi-ssid"
                                        data-current="%WIFI_SSID%"
                                        aria-label="Available networks">
                                        <option value="">Looking for networks...</option>
                                    </select>
                                    <button id="rescan-wifi" type="button" class="ghost">Rescan</button>
                                </div>

                                <label class="stacked-field" id="wifi-manual-row" hidden>
                                    <span>Network name</span>
                                    <input
                                        id="wifi-ssid-manual"
                                        name="wifi-ssid-manual"
                                        maxlength="32"
                                        autocomplete="off"
                                        spellcheck="false"
                                        disabled
                                        placeholder="Hidden or out-of-range network name">
                                </label>

                                <label class="stacked-field">
                                    <span>Password</span>
                                    <input
                                        id="wifi-pass"
                                        name="wifi-pass"
                                        type="password"
                                        maxlength="63"
                                        autocomplete="off"
                                        spellcheck="false"
                                        placeholder="%WIFI_PASS_PLACEHOLDER%">
                                </label>

                                <div class="fine-print">
                                    The radar joins 2.4 GHz networks only. Leave the password blank for an
                                    open network, or to keep the one already stored. Pick "Other network"
                                    from the list to type a hidden network's name.
                                </div>
                                <div id="wifi-result" class="status-text" aria-live="polite"></div>
                            </div>
                        </details>

                        <div class="row" %FORGET_HIDDEN%>
                            <div>
                                <div class="row-name">Forget this network</div>
                                <div class="row-note">
                                    Clears the stored network and restarts into the setup hotspot.
                                </div>
                                <div id="network-action-result" class="status-text" aria-live="polite"></div>
                            </div>
                            <div class="row-control">
                                <button type="button" id="forget-wifi" class="ghost danger">Forget</button>
                            </div>
                        </div>
                    </div>

                    <!--
                        Hidden on the setup hotspot: the credentials have to be
                        fetched from a website the phone filling this in cannot
                        open while it is joined to the radar, and OpenSky cannot
                        be reached from the hotspot to check them. A first setup
                        therefore ends with none stored, and the radar comes back
                        up holding on its "OpenSky key needed" screen until this
                        card -- visible once it is on the owner's own network --
                        has been filled in. Hidden rather than removed, so its
                        two fields still submit and keep whatever is stored.
                    -->
                    <div class="card" %OPENSKY_HIDDEN%>
                        <h3 class="card-title">OpenSky &mdash; required, the radar will not sweep without it</h3>

                        <div class="row stack">
                            <div class="row-note">
                                Free: register at
                                <a href="https://opensky-network.org" target="_blank" rel="noopener">opensky-network.org</a>,
                                sign in, then open
                                <a href="https://opensky-network.org/my-opensky/account" target="_blank" rel="noopener">Account &rarr; API client</a>
                                and create a new API client.
                            </div>
                        </div>

                        <div class="row" data-terms="opensky client id credentials api">
                            <div><div class="row-name">Client ID</div></div>
                            <div class="row-control">
                                <input
                                    id="opensky-id"
                                    name="opensky-id"
                                    class="mono"
                                    autocomplete="off"
                                    spellcheck="false"
                                    aria-label="OpenSky client ID"
                                    value='%OPENSKY_ID%'>
                            </div>
                        </div>

                        <div class="row" data-terms="opensky client secret credentials api">
                            <div><div class="row-name">Client secret</div></div>
                            <div class="row-control">
                                <input
                                    id="opensky-secret"
                                    name="opensky-secret"
                                    type="password"
                                    autocomplete="off"
                                    spellcheck="false"
                                    aria-label="OpenSky client secret"
                                    placeholder="%OPENSKY_SECRET_PLACEHOLDER%">
                            </div>
                        </div>
                    </div>

                    <div class="card">
                        <h3 class="card-title">Place search</h3>
                        <div class="row" data-terms="geocoder nominatim place search provider url endpoint">
                            <label class="row-label" for="geocoder-url">
                                <div class="row-name">Nominatim endpoint</div>
                                <div class="row-note">
                                    The service the place search on Location &amp; range asks. Change it
                                    only to point at your own instance.
                                </div>
                            </label>
                            <div class="row-control">
                                <input
                                    id="geocoder-url"
                                    name="geocoder-url"
                                    class="mono"
                                    value="%GEOCODER_URL%"
                                    aria-label="Nominatim-compatible place search URL">
                            </div>
                        </div>
                    </div>
                </section>

                <!-- ============ FIRMWARE & PANEL ============ -->
                <section class="group-panel" id="panel-device" data-group="device" hidden>
                    <div class="group-head">
                        <h2>Firmware &amp; panel</h2>
                        <p>Updates, and the trim that squares the picture up in its bezel.</p>
                    </div>

                    <div class="card">
                        <h3 class="card-title">Firmware</h3>

                        <div class="row" data-terms="firmware version update release check install">
                            <div>
                                <div class="row-name mono">%FIRMWARE_VERSION%</div>
                                <div class="row-note">
                                    Released %FIRMWARE_RELEASED%. The radar checks for a new one once an
                                    hour.<span id="update-status" aria-live="polite"></span>
                                </div>
                            </div>
                            <div class="row-control">
                                <button type="button" id="install-update" class="ghost" hidden>Install now</button>
                                <button type="button" id="check-update" class="ghost">Check now</button>
                            </div>
                        </div>

                        <div class="row" data-terms="auto update automatic install ask">
                            <label class="row-label" for="auto-update">
                                <div class="row-name">When a new release is found</div>
                                <div class="row-note">
                                    Installing takes about a minute and ends with a restart, so the
                                    display is unavailable while it runs.
                                </div>
                            </label>
                            <div class="row-control">
                                <select id="auto-update" name="auto-update" aria-label="Firmware update behaviour">
                                    <option value="true" %AUTO_UPDATE_ON_SELECTED%>Install automatically</option>
                                    <option value="false" %AUTO_UPDATE_OFF_SELECTED%>Ask me first</option>
                                </select>
                            </div>
                        </div>

                        <details class="help">
                            <summary>Release notes</summary>
                            <p>%FIRMWARE_NOTES%</p>
                        </details>
                    </div>

                    <div class="card">
                        <h3 class="card-title">Panel alignment</h3>

                        <div class="row" data-terms="rotation trim tilt crooked bezel alignment degrees">
                            <label class="row-label" for="screen-trim">
                                <div class="row-name">Rotation trim</div>
                                <div class="row-note">
                                    Degrees. Leave at 0 unless the display sits crooked in its bezel.
                                    Anything else slows each frame down.
                                </div>
                            </label>
                            <div class="row-control">
                                <input
                                    id="screen-trim"
                                    name="screen-trim"
                                    class="mono"
                                    type="number"
                                    min="%SCREEN_TRIM_MIN%"
                                    step="0.1"
                                    max="%SCREEN_TRIM_MAX%"
                                    aria-label="Rotation trim in degrees"
                                    value='%SCREEN_TRIM%'>
                            </div>
                        </div>

                        <div class="row" data-terms="alignment test pattern crosshair ring scale">
                            <label class="row-label" for="alignment-test">
                                <div class="row-name">Show alignment pattern</div>
                                <div class="row-note">
                                    A crosshair, an edge ring and a degree scale instead of the radar.
                                    No aircraft are shown while this is on.
                                </div>
                            </label>
                            <div class="row-control">
                                <input id="alignment-test" name="alignment-test" class="switch" type="checkbox"
                                       aria-label="Show alignment pattern" %ALIGNMENT_TEST%>
                            </div>
                        </div>

                        <details class="help">
                            <summary>How to use these two</summary>
                            <p>
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
                                repeat &mdash; then switch the pattern back off.
                            </p>
                        </details>
                    </div>
                </section>

                <!-- ============ THIS RADAR ============ -->
                <section class="group-panel" id="panel-about" data-group="about" hidden>
                    <div class="group-head">
                        <h2>This radar</h2>
                        <p>Status, reporting, and the two actions that restart it.</p>
                    </div>

                    <div class="card">
                        <h3 class="card-title">Status</h3>
                        <dl class="stat-grid">
                            <div><dt>Address</dt><dd>%NET_IP%</dd></div>
                            <div><dt>Signal</dt><dd>%NET_RSSI%</dd></div>
                            <div><dt>MAC</dt><dd>%NET_MAC%</dd></div>
                            <div><dt>Uptime</dt><dd>%NET_UPTIME%</dd></div>
                            <div><dt>Free memory</dt><dd>%NET_HEAP%</dd></div>
                        </dl>

                        <div class="row" data-terms="restart reboot">
                            <div>
                                <div class="row-name">Restart</div>
                                <div class="row-note">Comes back in about a minute. Every setting is kept.</div>
                                <div id="restart-result" class="status-text" aria-live="polite"></div>
                            </div>
                            <div class="row-control">
                                <button type="button" id="restart-radar" class="ghost">Restart radar</button>
                            </div>
                        </div>
                    </div>

                    <div class="card">
                        <h3 class="card-title">Remote diagnostics &mdash; off unless a key is set</h3>

                        <div class="row stack">
                            <div class="row-note">
                                When it is on, this radar sends crash reports and error messages to
                                <a href="https://insights.espressif.com" target="_blank" rel="noopener">ESP Insights</a>,
                                along with free memory, uptime, Wi-Fi signal strength, and this
                                network's name and IP address. It does not send your Wi-Fi password,
                                your OpenSky credentials, or the location the radar is centred on.
                            </div>
                        </div>

                        <div class="row" data-terms="insights diagnostics key telemetry crash reports">
                            <div><div class="row-name">Insights auth key</div></div>
                            <div class="row-control">
                                <input
                                    id="insights-key"
                                    name="insights-key"
                                    type="password"
                                    autocomplete="off"
                                    spellcheck="false"
                                    aria-label="ESP Insights auth key"
                                    placeholder="%INSIGHTS_KEY_PLACEHOLDER%">
                            </div>
                        </div>

                        <div class="row" data-terms="insights label dashboard name">
                            <label class="row-label" for="insights-label">
                                <div class="row-name">Label</div>
                                <div class="row-note">
                                    So this radar is recognisable on the dashboard, which otherwise
                                    lists it as %NET_MAC%.
                                </div>
                            </label>
                            <div class="row-control">
                                <input
                                    id="insights-label"
                                    name="insights-label"
                                    autocomplete="off"
                                    maxlength="48"
                                    aria-label="Insights label"
                                    placeholder="e.g. kitchen shelf"
                                    value='%INSIGHTS_LABEL%'>
                            </div>
                        </div>

                        <div class="row" data-terms="insights clear forget key off">
                            <label class="row-label" for="insights-clear">
                                <div class="row-name">Turn reporting off and forget the key</div>
                                <div class="row-note">
                                    An empty key box means "keep the stored key", so this is how the key
                                    is actually removed. Takes effect on the restart that saving does.
                                </div>
                            </label>
                            <div class="row-control">
                                <input id="insights-clear" name="insights-clear" class="switch" type="checkbox"
                                       aria-label="Turn reporting off and forget the key">
                            </div>
                        </div>
                    </div>

                    <div class="card danger">
                        <h3 class="card-title">Factory reset</h3>

                        <div class="row stack">
                            <div class="row-note">
                                Erases every setting on this page &mdash; network, location, OpenSky
                                credentials, display choices, panel trim and the diagnostics key
                                &mdash; and restarts into the setup hotspot as if the radar had just
                                been flashed. The installed firmware version is not affected. There is
                                no undo.
                            </div>
                        </div>

                        <div class="row" data-terms="factory reset erase wipe">
                            <label class="row-label" for="factory-reset-confirm">
                                <div class="row-name">Yes, erase all settings on this radar</div>
                                <div class="row-note">Tick to arm the button.</div>
                                <div id="factory-reset-result" class="status-text" aria-live="polite"></div>
                            </label>
                            <div class="row-control">
                                <input id="factory-reset-confirm" class="switch" type="checkbox"
                                       aria-label="Confirm erasing all settings">
                                <button type="button" id="factory-reset" class="ghost danger" disabled>Erase</button>
                            </div>
                        </div>
                    </div>
                </section>

                <p class="no-matches" id="no-matches" hidden>Nothing matches that search.</p>

                <div class="save-row" id="save-row">
                    <input type="submit" value="%SAVE_LABEL%" class="save-button">
                    <span id="save-state" class="save-state" aria-live="polite">No changes yet</span>
                    <span id="result" aria-live="polite"></span>
                    <span class="save-note">Save applies every group at once</span>
                </div>
            </form>

            <div class="project-footer">
                <a href="https://github.com/thedontknowguy/micro-radar" target="_blank" rel="noopener">Micro Radar on GitHub</a>
                &middot; a fork of the original
                <a href="https://github.com/AnthonySturdy/micro-radar" target="_blank" rel="noopener">Micro Radar</a>
                by Anthony Sturdy. Full credit to Anthony for the original project, firmware,
                enclosure, and design.
            </div>

            </div>
        </div>
      </div>

        <script>
            // Setup mode means this page is being served from the radar's own
            // hotspot, so nothing on the far side of the internet is reachable.
            const setupMode = document.body.dataset.netMode === 'setup';

            const navButtons = Array.from(document.querySelectorAll('.nav'));
            const groupPanels = Array.from(document.querySelectorAll('.group-panel'));

            function showGroup(name) {
                groupPanels.forEach(function(panel) {
                    panel.hidden = panel.dataset.group !== name;
                });
                navButtons.forEach(function(button) {
                    if (button.dataset.group === name)
                        button.setAttribute('aria-current', 'true');
                    else
                        button.removeAttribute('aria-current');
                });
            }

            function currentGroup() {
                const shown = groupPanels.find(function(panel) { return !panel.hidden; });
                return shown ? shown.dataset.group : 'location';
            }

            navButtons.forEach(function(button) {
                button.addEventListener('click', function() {
                    clearSearch();
                    showGroup(button.dataset.group);
                    // Replaced rather than pushed: the groups are one page, and
                    // filling the history with them turns Back into a way of
                    // walking sideways instead of leaving.
                    history.replaceState(null, '', '#' + button.dataset.group);
                });
            });

            // On the hotspot there is nothing to choose between: the network is
            // the only thing worth setting before the radar can reach anything.
            const requestedGroup = location.hash.replace('#', '');
            showGroup(
                setupMode
                    ? 'connection'
                    : (navButtons.some(b => b.dataset.group === requestedGroup) ? requestedGroup : 'location')
            );

            // Filtering never removes a row from the document, only hides it:
            // /save reads every checkbox as "present means on", so a row taken
            // out would submit as a switched-off setting. Searching shows every
            // group at once, which is the point -- the reason to type is not
            // knowing which group a setting is in.
            const searchBox = document.getElementById('setting-search');
            const searchableRows = Array.from(
                document.querySelectorAll('.row[data-terms], .network-details > summary[data-terms]')
            );
            const noMatches = document.getElementById('no-matches');

            function rowHaystack(row) {
                if (!row.dataset.haystack) {
                    row.dataset.haystack =
                        (row.textContent + ' ' + row.dataset.terms).toLocaleLowerCase();
                }
                return row.dataset.haystack;
            }

            function clearSearch() {
                if (!searchBox || !searchBox.value)
                    return;
                searchBox.value = '';
                applySearch();
            }

            function applySearch() {
                const query = searchBox.value.trim().toLocaleLowerCase();

                if (!query) {
                    searchableRows.forEach(function(row) { row.hidden = false; });
                    document.querySelectorAll('.card, .group-head').forEach(function(el) {
                        el.hidden = false;
                    });
                    noMatches.hidden = true;
                    showGroup(currentGroup());
                    return;
                }

                searchableRows.forEach(function(row) {
                    row.hidden = rowHaystack(row).indexOf(query) === -1;
                });

                // A card whose every searchable row is hidden has nothing left
                // to show but its own title, and a group with no visible card
                // is just a heading.
                groupPanels.forEach(function(panel) {
                    let panelHit = false;
                    panel.querySelectorAll('.card').forEach(function(card) {
                        const rows = card.querySelectorAll('[data-terms]');
                        const hit = rows.length === 0
                            ? false
                            : Array.from(rows).some(function(row) { return !row.hidden; });
                        card.hidden = !hit;
                        panelHit = panelHit || hit;
                    });
                    panel.hidden = !panelHit;
                    const head = panel.querySelector('.group-head');
                    if (head)
                        head.hidden = !panelHit;
                });

                noMatches.hidden = groupPanels.some(function(panel) { return !panel.hidden; });
            }

            if (searchBox) {
                searchBox.addEventListener('input', applySearch);
                searchBox.addEventListener('keydown', function(event) {
                    if (event.key === 'Escape') {
                        searchBox.value = '';
                        applySearch();
                    }
                });
            }

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

            // The select only applies while its toggle is on, so it follows it
            // rather than sitting there looking editable.
            function bindDependentSelect(toggleId, selectId, rowId) {
                const toggle = document.getElementById(toggleId);
                const select = document.getElementById(selectId);
                const row = document.getElementById(rowId);

                function syncSelectState() {
                    select.disabled = !toggle.checked;
                    row.classList.toggle('is-off', !toggle.checked);
                }

                toggle.addEventListener('change', syncSelectState);
                syncSelectState();
            }

            bindDependentSelect('scanline', 'sweep-period', 'row-sweep-period');
            bindDependentSelect('speed', 'speed-unit', 'row-speed-unit');
            bindDependentSelect('altitude', 'altitude-unit', 'row-altitude-unit');
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

            // Writing a value from script fires none of the events the change
            // counter listens for, so each field it fills is reported by hand.
            function fillLocation(latitude, longitude, message, suggestedName) {
                const latitudeField = document.getElementById('latitude');
                const longitudeField = document.getElementById('longitude');

                latitudeField.value = Number(latitude).toFixed(6);
                longitudeField.value = Number(longitude).toFixed(6);
                markChanged(latitudeField);
                markChanged(longitudeField);

                if (typeof suggestedName === 'string' && suggestedName.trim()) {
                    locationNameInput.value = compactLocationName(suggestedName);
                    markChanged(locationNameInput);
                }

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
            const saveRow = document.getElementById('save-row');
            const saveState = document.getElementById('save-state');
            const resultBox = document.getElementById('result');

            // One Save covers every group, so a change made in a group that is
            // no longer showing is easy to forget about. Counting them per
            // group and badging the rail is what keeps it visible from wherever
            // the next change is made.
            const touched = new Map();

            // Controls that are not settings: the tick that only arms the
            // erase button, the two place-search boxes, and the coverage
            // slider and its degrees box -- those two write the hidden radius
            // field, which is counted for them, and would otherwise make one
            // drag read as two changes.
            const notASetting = new Set([
                'factory-reset-confirm',
                'place-query',
                'place-results',
                'radius-range',
                'radius-degrees'
            ]);

            function updateChangeCounts() {
                let total = 0;

                navButtons.forEach(function(button) {
                    const changed = touched.get(button.dataset.group);
                    const count = changed ? changed.size : 0;
                    total += count;
                    button.classList.toggle('has-changes', count > 0);
                    button.querySelector('.nav-count').textContent = String(count);
                });

                saveRow.classList.toggle('is-dirty', total > 0);
                saveState.textContent = total === 0
                    ? 'No changes yet'
                    : total + (total === 1 ? ' unsaved change' : ' unsaved changes');
            }

            function markChanged(field) {
                const key = field.name || field.id;
                const panel = field.closest('.group-panel');
                if (!key || !panel || notASetting.has(field.id))
                    return;

                if (!touched.has(panel.dataset.group))
                    touched.set(panel.dataset.group, new Set());
                touched.get(panel.dataset.group).add(key);

                resultBox.textContent = '';
                updateChangeCounts();
            }

            configForm.addEventListener('input', function(e) { markChanged(e.target); });
            configForm.addEventListener('change', function(e) { markChanged(e.target); });

            // Dragging the slider writes the stored degrees rather than
            // carrying a name of its own, so the hidden field it feeds has to
            // be counted for it.
            radiusRange.addEventListener('input', function() { markChanged(radiusField); });
            radiusDegrees.addEventListener('change', function() { markChanged(radiusField); });

            updateChangeCounts();

            // A number outside its min/max stops submission before the submit
            // event ever fires, and the browser cannot focus a field inside a
            // hidden panel -- so the press would do nothing at all and say
            // nothing about why. Capture, because this has to run before the
            // browser gives up trying to focus it.
            configForm.addEventListener('invalid', function(e) {
                clearSearch();
                const panel = e.target.closest('.group-panel');
                if (panel && panel.hidden)
                    showGroup(panel.dataset.group);
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
                    clearSearch();
                    showGroup('connection');
                    networkDetails.open = true;
                    wifiResult.textContent = 'Choose a network, or type its name, before saving.';
                    return;
                }

                fetch(this.action, { method: 'POST', body: new FormData(this) })
                    .then(r => r.text())
                    .then(function(html) {
                        resultBox.innerHTML = html;
                        touched.clear();
                        updateChangeCounts();
                        saveState.textContent = 'Saved';
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

    // Off by default. It is the one display setting whose usefulness depends
    // entirely on what the radar is aimed at: pointed at an airport it doubles
    // the number of targets on the face, most of them parked.
    ensureKey("ground-traffic", "false");

    // Off by default: the clock is the largest thing on the face after the
    // radar itself, and a radar that shows one without being asked reads as a
    // clock with aircraft on it.
    ensureKey("clock", "false");

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
        values.groundTrafficEnabled = prefs.getString("ground-traffic", "false");
        values.clockEnabled = prefs.getString("clock", "false");
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
        values.forgetHidden = setupMode ? "hidden" : "";
        values.saveLabel = setupMode ? "Save and connect" : "Save";
        values.navHidden = setupMode ? "hidden" : "";
        values.openskyHidden = setupMode ? "hidden" : "";
        values.wifiPassPlaceholder = setupMode
            ? "Network password"
            : "Leave blank to keep the stored password";
        // The summary is the row someone reads before deciding to open the
        // picker, so it carries the network's name; the note underneath says
        // what opening it is for.
        values.networkSummary = setupMode
            ? String("Choose your network")
            : (storedSsid.length() ? EscapeHtmlAttribute(storedSsid) : String("No network stored"));
        values.networkNote = setupMode
            ? "The radar is serving this page from its own hotspot. Saving joins the network "
              "and starts the radar; everything else can be set once it is on."
            : (storedSsid.length()
                ? "Connected. Open this to move the radar to a different network."
                : String("Open this to choose one."));

        // Stands in for the separate info page the old Wi-Fi portal had.
        values.netIp = setupMode
            ? WiFi.softAPIP().toString() + " (setup hotspot)"
            : WiFi.localIP().toString();
        values.netRssi = setupMode ? String("access point mode") : String(WiFi.RSSI()) + " dBm";
        values.netMac = WiFi.macAddress();
        values.netUptime = FormatUptime(millis());
        values.netHeap = String(ESP.getFreeHeap() / 1024) + " KB free";

        // The one line of state the header carries on every group.
        values.netStatus = setupMode
            ? String("Setup hotspot")
            : (storedSsid.length()
                ? EscapeHtmlAttribute(storedSsid) + " &middot; " + values.netRssi
                : String("Connected"));

        // template processor called once per %PLACEHOLDER% token found in CONFIG_HTML.
        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html",
            (const uint8_t*)CONFIG_HTML, sizeof(CONFIG_HTML) - 1,
            [values](const String& var) -> String {
                if (var == "NET_MODE")       return values.setupMode;
                if (var == "NAV_HIDDEN")     return values.navHidden;
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
                if (var == "NET_STATUS")     return values.netStatus;

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
                if (var == "GROUND_TRAFFIC") return values.groundTrafficEnabled == "true" ? "checked" : "";
                if (var == "CLOCK")          return values.clockEnabled == "true" ? "checked" : "";
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
        saveIfOneOf("aircraft-marker", { "radar", "triangle", "dot" });
        saveIfOneOf("auto-update", { "true", "false" });

        // Free text rather than a fixed set, so these two are stored whenever
        // submitted -- including empty. An empty client id is not rejected
        // here: it is how a pair is cleared before a different one is entered,
        // and a radar left with none simply holds on its "OpenSky key needed"
        // screen after the restart rather than sweeping with nothing to show.
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
        prefs.putString("ground-traffic",
                        request->hasParam("ground-traffic", true) ? "true" : "false");
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
