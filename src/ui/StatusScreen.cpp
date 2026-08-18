#include "ui/StatusScreen.h"

#include <cmath>

#include "ui/PanelTrim.h"
#include "ui/SmoothText.h"

namespace {

// The sizes this screen sets, in pixels of line height. Tried largest first:
// the first entry whose every line clears the bezel wins, so the order here is
// the preference order.
//
// These are real proportional letterforms at a size that suits them, which is
// the whole point of the ladder. The alternative -- what this screen used to do
// -- is the built-in 6x8 bitmap face scaled up by a whole number, and at the 3x
// the longer lines force, every curve in the alphabet arrives as a staircase of
// 3x3 blocks.
//
// The four heights are the ones the bold bitmap ladder that came before offered
// -- 24pt, 18pt, 12pt and 9pt of FreeSansBold -- kept to the pixel so the
// screens are laid out exactly as they were. What changed underneath is how a
// run gets onto the panel: see SmoothText.
//
// The gap between the third and second entries is wide, a third of the cap
// height, and it is why this file sets a screen in two sizes rather than one;
// see ShowQrAddressScreen.
constexpr int SizeLadder[] = { 47, 33, 23, 18 };

constexpr int LadderSize = sizeof(SizeLadder) / sizeof(*SizeLadder);
constexpr int SmallestSizeIndex = LadderSize - 1;

// How many supporting lines a screen can carry, and so how many slots every
// layout here is written for.
constexpr int MaxLines = 5;

// How close to the rim the longest line may come. The panel is round and sits
// in a bezel, so the outermost ring of pixels is never properly visible. Ten is
// about what the bezel actually covers; the old flat inset was SCREEN_SIZE/12,
// or thirty, which threw away a fifth of the usable width and bought nothing.
constexpr int BezelMargin = 10;

// Air between baselines, on top of whatever leading the face already carries.
// Small, because these fonts bring their own: the old value of 10 was there to
// separate lines of a bitmap font that had none.
constexpr int LineGap = 6;

// How much of the panel the QR badge may have.
//
// Sized to the denser of the two codes this screen draws, not the shorter one:
// the config address is 25 modules a side and would be happy with less, but the
// WiFi join code -- WIFI:T:nopass;S:<ssid>;; is long enough to force version 3,
// 29 modules -- and with its quiet zone that is a 37-module plate. 148 is what
// puts four pixels under every one of them. What is left over is what the
// heading and the lines are read from.
//
// Four rather than the three a 111px budget bought, because three is where a
// phone starts failing to read these off the glass. Not for want of focus: at
// three pixels a module the pattern sits close enough to what a phone sensor
// can resolve at arm's length that it aliases, and an aliased code reads as one
// whose module spacing wanders -- which is the thing a decoder gives up on.
// A unit with a trim set was hiding this, because the rotation resamples the
// finished frame and an antialiased grid does not beat against a sensor grid;
// the codes only ever failed on the units presenting them pixel-exact.
constexpr int BadgeBudget = 148;

// Air above and below the badge. Wider than the gap between two lines of text,
// because the badge is not a line of text: the code needs to read as its own
// thing rather than as a picture wedged into a paragraph.
constexpr int BadgeGap = 14;

// Half the width the panel actually offers `dy` pixels above or below its
// centre line. On a round screen that is a chord rather than the full width,
// and the difference is not small: a line 90px off centre has about 40px less
// room than one across the middle. Measuring every line against the centre
// width -- which is what a single flat USABLE_WIDTH does -- is what lets a long
// top or bottom line run into the curve.
int HalfWidthAt(int dy)
{
  const int usableRadius = SCREEN_SIZE_DIV_2 - BezelMargin;
  const int squared = usableRadius * usableRadius - dy * dy;
  return squared <= 0 ? 0 : static_cast<int>(sqrtf(static_cast<float>(squared)));
}

// Everything a screen is made of, in the order it is stacked. Any part may be
// missing: the plain status screens are supporting lines and nothing else.
//
// Which part a run of text goes in is a statement about what it is for, not
// about how long it is. `heading` and `address` are what the screen is trying
// to say and are set in the emphasis face; `lines` are what it says to whoever
// the first two did not reach, and are set in the supporting one.
struct Layout {
  String heading;
  String address;
  String lines[MaxLines];
  QrBadgeSource badge = { nullptr, nullptr };
  int badgeSize = 0;
};

// Where each part of a laid-out screen starts. Worked out by the fitting pass
// and handed to the drawing pass, so the two cannot drift.
struct Placement {
  int headingTop = 0;
  int badgeTop = 0;
  int addressTop = 0;
  int firstLineTop = 0;
  int linePitch = 0;
  int blockTop = 0;
  int blockBottom = 0;
  bool valid = false;
};

int CountLines(const Layout& layout)
{
  int count = 0;
  for (const String& line : layout.lines)
    count += line.isEmpty() ? 0 : 1;
  return count;
}

// The whole stack -- heading, badge, address, lines -- centred on the panel as
// one block, rather than each part placed against an edge. Centring the parts
// separately is what leaves a screen with its text pushed to the rim and a hole
// in the middle of it.
Placement Place(const Layout& layout, int emphasisHeight, int supportHeight, int count)
{
  // Built by walking the stack once to measure it and again to place it, from
  // the same list, so a part cannot be counted in one pass and forgotten in the
  // other.
  const int headingPart = layout.heading.isEmpty() ? 0 : emphasisHeight;
  const int badgePart = layout.badgeSize;
  const int addressPart = layout.address.isEmpty() ? 0 : emphasisHeight;
  const int linesPart = count > 0 ? count * supportHeight + (count - 1) * LineGap : 0;

  // The gap before a part, given something came before it. The badge takes the
  // wider one on both sides; everything else is a line of text against a line
  // of text.
  const int beforeBadge = BadgeGap;
  const int afterBadge = BadgeGap;

  int height = headingPart;
  if (badgePart > 0)
    height += (height > 0 ? beforeBadge : 0) + badgePart;
  if (addressPart > 0)
    height += (height > 0 ? (badgePart > 0 ? afterBadge : LineGap) : 0) + addressPart;
  if (linesPart > 0)
    height += (height > 0 ? (badgePart > 0 && addressPart == 0 ? afterBadge : LineGap) : 0) + linesPart;

  Placement placement;
  placement.linePitch = supportHeight + LineGap;
  placement.blockTop = (SCREEN_SIZE - height) / 2;
  placement.blockBottom = placement.blockTop + height;

  int y = placement.blockTop;
  placement.headingTop = y;
  if (headingPart > 0)
    y += headingPart;
  if (badgePart > 0) {
    y += (y > placement.blockTop ? beforeBadge : 0);
    placement.badgeTop = y;
    y += badgePart;
  }
  if (addressPart > 0) {
    y += (y > placement.blockTop ? (badgePart > 0 ? afterBadge : LineGap) : 0);
    placement.addressTop = y;
    y += addressPart;
  }
  if (linesPart > 0) {
    y += (y > placement.blockTop ? (badgePart > 0 && addressPart == 0 ? afterBadge : LineGap) : 0);
    placement.firstLineTop = y;
  }

  return placement;
}

// Whether a run of text `fontHeight` tall, with its top at `top`, clears the
// curve of the panel. Whichever of its own edges sits nearer the rim decides
// how much width it gets: the top edge for a line above the centre, the bottom
// edge for one below it.
bool ClearsBezel(int width, int top, int fontHeight)
{
  const int above = top - SCREEN_SIZE_DIV_2;
  const int below = above + fontHeight;
  const int dy = abs(above) > abs(below) ? abs(above) : abs(below);
  return width <= 2 * HalfWidthAt(dy);
}

// Whether this pairing of sizes holds the whole screen. Everything is measured
// where it would actually be drawn -- there is no other way to ask on a round
// panel, where what a line may be worth in width depends on how far down it
// lands, which depends on how tall everything above it is.
Placement TrySizes(LovyanGFX& canvas, const Layout& layout, int count,
                   int emphasisIndex, int supportIndex)
{
  const int emphasisHeight = SizeLadder[emphasisIndex];
  const int headingWidth = SmoothText::Width(canvas, layout.heading, emphasisHeight);
  const int addressWidth = SmoothText::Width(canvas, layout.address, emphasisHeight);

  const int supportHeight = SizeLadder[supportIndex];

  Placement placement = Place(layout, emphasisHeight, supportHeight, count);

  // Room for the block itself, before any question of how wide its lines are.
  bool fits = placement.blockTop >= BezelMargin
           && placement.blockBottom <= SCREEN_SIZE - BezelMargin;

  if (!layout.heading.isEmpty())
    fits = fits && ClearsBezel(headingWidth, placement.headingTop, emphasisHeight);
  if (!layout.address.isEmpty())
    fits = fits && ClearsBezel(addressWidth, placement.addressTop, emphasisHeight);

  int y = placement.firstLineTop;
  for (const String& line : layout.lines) {
    if (line.isEmpty())
      continue;
    fits = fits && ClearsBezel(SmoothText::Width(canvas, line, supportHeight), y, supportHeight);
    y += placement.linePitch;
  }

  placement.valid = fits;
  return placement;
}

void DrawLayout(LGFX& tft, const Layout& layout)
{
  LovyanGFX& canvas = PanelTrim::Canvas(tft);

  const int count = CountLines(layout);

  // Measuring is done at scale 1 throughout -- picking a bigger size is what
  // this does instead of multiplying a small one.
  canvas.setTextSize(1);

  // Every pairing, largest first, emphasis before support: sixteen tries at
  // worst, once per screen. Written as a search rather than as two independent
  // choices because the two are not independent -- a bigger emphasis size
  // pushes the supporting lines further down the panel, where the curve leaves
  // them less width than they had.
  //
  // Support is never allowed above emphasis. A screen whose small print is set
  // larger than its heading is not a screen that has fitted itself to the
  // panel; it is one that has forgotten what it is saying.
  int emphasisIndex = SmallestSizeIndex;
  int supportIndex = SmallestSizeIndex;
  Placement placement;

  for (int emphasis = 0; emphasis < LadderSize && !placement.valid; emphasis++) {
    for (int support = emphasis; support < LadderSize; support++) {
      const Placement candidate = TrySizes(canvas, layout, count, emphasis, support);
      if (candidate.valid) {
        emphasisIndex = emphasis;
        supportIndex = support;
        placement = candidate;
        break;
      }
    }
  }

  if (!placement.valid) {
    // Nothing in the ladder fit. The smallest size is drawn anyway and allowed
    // to overrun the bezel -- a line clipped at the rim is still worth more to
    // whoever is reading it than a blank screen.
    placement = TrySizes(canvas, layout, count, SmallestSizeIndex, SmallestSizeIndex);
  }

  constexpr uint32_t ink = lgfx::color888(0, 255, 0);

  canvas.fillScreen(lgfx::color888(0, 0, 0));

  if (!layout.heading.isEmpty())
    SmoothText::DrawCentre(canvas, layout.heading, SCREEN_SIZE_DIV_2,
                           placement.headingTop, SizeLadder[emphasisIndex], ink);
  if (!layout.address.isEmpty())
    SmoothText::DrawCentre(canvas, layout.address, SCREEN_SIZE_DIV_2,
                           placement.addressTop, SizeLadder[emphasisIndex], ink);

  // Drawn straight onto the canvas at whole pixels per module, never through
  // the resample the text goes through: a code is read by a machine looking for
  // a grid, and softening the edge of every module is the one thing that helps
  // a letterform and hurts a badge.
  if (layout.badgeSize > 0)
    layout.badge.draw(canvas,
                      (SCREEN_SIZE - layout.badgeSize) / 2,
                      placement.badgeTop,
                      layout.badgeSize);

  int y = placement.firstLineTop;
  for (const String& line : layout.lines) {
    if (line.isEmpty())
      continue;
    SmoothText::DrawCentre(canvas, line, SCREEN_SIZE_DIV_2, y,
                           SizeLadder[supportIndex], ink);
    y += placement.linePitch;
  }

  PanelTrim::Present(tft);

  // Everything else that draws on this canvas expects the default face at the
  // default scale. Nothing here sets a face on it any more -- the runs are
  // composed elsewhere and copied in -- but SmoothText falls back to drawing on
  // the canvas directly when it cannot get a buffer, so the reset stays. Done
  // on the canvas rather than on the panel, because with a trim set those are
  // different objects and the panel's own text state is never touched by
  // anything here.
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
}

}

void ShowStatusScreen(LGFX& tft,
                      const String& first,
                      const String& second,
                      const String& third,
                      const String& fourth,
                      const String& fifth)
{
  // No heading and no address: one face for the lot, exactly as before. The
  // search above collapses to the old ladder walk when there is nothing set in
  // the emphasis face to hold it back.
  Layout layout;
  layout.lines[0] = first;
  layout.lines[1] = second;
  layout.lines[2] = third;
  layout.lines[3] = fourth;
  layout.lines[4] = fifth;
  DrawLayout(tft, layout);
}

void ShowQrAddressScreen(LGFX& tft,
                         const String& heading,
                         const String& address,
                         const QrBadgeSource& badge,
                         const String& fallback,
                         const String& footnote)
{
  Layout layout;
  layout.heading = heading;
  layout.address = address;
  layout.lines[0] = fallback;
  layout.lines[1] = footnote;
  layout.badge = badge;

  // Asked for before anything is laid out, because the badge is part of the
  // block the type has to fit around -- and it is only ever as big as a whole
  // number of pixels per module allows.
  layout.badgeSize = badge.sizeWithin(BadgeBudget);
  DrawLayout(tft, layout);
}
