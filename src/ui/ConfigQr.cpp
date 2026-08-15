#include "ui/ConfigQr.h"

#include <Arduino.h>

#include "ConfigurationWebServer.h"
#include "QrCode.h"
#include "ui/QrBadge.h"

namespace ConfigQr {
namespace {

// What the badge claims the radar answers to, checked against what the radar
// actually answers to. The picture is generated from MdnsAddress by a script,
// and a script that has not been run since the name changed leaves a code that
// looks perfectly good and goes nowhere -- the one kind of mistake that survives
// being looked at. So the build fails instead.
constexpr bool SameText(const char* left, const char* right)
{
  return *left == *right && (*left == '\0' || SameText(left + 1, right + 1));
}

constexpr bool StartsWith(const char* text, const char* prefix)
{
  return *prefix == '\0' || (*text == *prefix && StartsWith(text + 1, prefix + 1));
}

// The scheme is part of what is encoded on purpose: a phone offers to open a
// string that starts with one and offers to search for a string that does not.
constexpr int SchemeLength = 7;  // "http://"

static_assert(StartsWith(QrCode::Url, "http://")
              && SameText(QrCode::Url + SchemeLength, MdnsAddress),
              "the QR code and MdnsAddress disagree - "
              "rerun scripts/generate_qr_code.py");

constexpr QrBadge::Code Grid = { QrCode::Modules, QrCode::Size, QrCode::QuietZone, QrCode::Stride };

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
