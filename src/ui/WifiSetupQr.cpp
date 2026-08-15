#include "ui/WifiSetupQr.h"

#include <Arduino.h>

#include "WiFiConnection.h"
#include "WifiQrCode.h"
#include "ui/QrBadge.h"

namespace WifiSetupQr {
namespace {

// What the badge offers to join, checked against what the radar actually puts
// up. The picture is generated from SetupHotspotName by a script, and a script
// that has not been run since the name changed leaves a code that looks
// perfectly good and joins nothing -- the one kind of mistake that survives
// being looked at. So the build fails instead.
constexpr bool SameText(const char* left, const char* right)
{
  return *left == *right && (*left == '\0' || SameText(left + 1, right + 1));
}

constexpr bool StartsWith(const char* text, const char* prefix)
{
  return *prefix == '\0' || (*text == *prefix && StartsWith(text + 1, prefix + 1));
}

// Advances past `prefix`, which the caller must already have checked `text`
// starts with -- this only counts characters, it does not compare them again.
constexpr const char* SkipPrefix(const char* text, const char* prefix)
{
  return *prefix == '\0' ? text : SkipPrefix(text + 1, prefix + 1);
}

// The parts of the WIFI: URI format either side of the SSID. T:nopass, because
// RunSetupPortal opens the hotspot with WiFi.softAP(name) and no password.
constexpr const char* JoinPrefix = "WIFI:T:nopass;S:";
constexpr const char* JoinSuffix = ";;";

constexpr bool EncodesSsid(const char* payload, const char* ssid)
{
  return StartsWith(payload, JoinPrefix)
      && StartsWith(SkipPrefix(payload, JoinPrefix), ssid)
      && SameText(SkipPrefix(SkipPrefix(payload, JoinPrefix), ssid), JoinSuffix);
}

static_assert(EncodesSsid(WifiQrCode::Payload, WiFiConnection::SetupHotspotName),
              "the WiFi setup QR code and SetupHotspotName disagree - "
              "rerun scripts/generate_qr_code.py");

constexpr QrBadge::Code Grid = { WifiQrCode::Modules, WifiQrCode::Size, WifiQrCode::QuietZone, WifiQrCode::Stride };

}

int SizeWithin(int maxSize)
{
  return QrBadge::SizeWithin(Grid, maxSize);
}

void Draw(LovyanGFX& canvas, int x, int y, int size)
{
  QrBadge::Draw(canvas, Grid, x, y, size);
}

}
