#pragma once

#include <LovyanGFX.hpp>

// The radar's own setup access point, as a QR code that offers to join it
// directly -- for the screen that asks someone to find it by hand otherwise.
//
// Scanning it does what typing the SSID into a phone's WiFi settings does, minus
// the chance of a typo -- see ConfigQr for why that trade is worth it even for a
// name this short.
namespace WifiSetupQr {

// See QrBadge::SizeWithin.
int SizeWithin(int maxSize);

// See QrBadge::Draw.
void Draw(LovyanGFX& canvas, int x, int y, int size);

}
