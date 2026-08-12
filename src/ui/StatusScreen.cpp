#include "ui/StatusScreen.h"

#include <cmath>

#include "ui/ConfigQr.h"
#include "ui/PanelTrim.h"

namespace {

// Tried largest first: the first entry whose every line clears the bezel wins,
// so the order here is the preference order.
//
// These are real proportional letterforms drawn at the size they were designed
// for, which is the whole point of the ladder. The alternative -- what this
// screen used to do -- is the built-in 6x8 bitmap face scaled up by a whole
// number, and at the 3x the longer lines force, every curve in the alphabet
// arrives as a staircase of 3x3 blocks. Bold rather than regular because the
// text is green-on-black at arm's length, where thin stems disappear.
//
// The gap between the 12pt and 18pt entries is wide -- a third of the cap
// height -- and there is nothing in the library to put between them: the other
// faces that size are all wider per letter, which is the opposite of what a
// screen short of width has any use for. That gap is why this file sets a
// screen in two sizes rather than one; see ShowQrAddressScreen.
const lgfx::IFont* const FontLadder[] = {
  &fonts::FreeSansBold24pt7b,
  &fonts::FreeSansBold18pt7b,
  &fonts::FreeSansBold12pt7b,
  &fonts::FreeSansBold9pt7b,
};

constexpr int LadderSize = sizeof(FontLadder) / sizeof(*FontLadder);
constexpr int SmallestFontIndex = LadderSize - 1;

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
// A hundred pixels, which for the current address comes out at three pixels a
// module and a 99px plate. Two pixels a module is where a phone stops being
// able to focus, and it is the only reason the budget is not smaller still --
// what is left over is what the heading and the lines are read from.
constexpr int BadgeBudget = 100;

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

// Whether this pairing of faces holds the whole screen. Everything is measured
// where it would actually be drawn -- there is no other way to ask on a round
// panel, where what a line may be worth in width depends on how far down it
// lands, which depends on how tall everything above it is.
Placement TryFaces(LovyanGFX& canvas, const Layout& layout, int count,
                   int emphasisIndex, int supportIndex)
{
  canvas.setFont(FontLadder[emphasisIndex]);
  const int emphasisHeight = canvas.fontHeight();
  const int headingWidth = canvas.textWidth(layout.heading);
  const int addressWidth = canvas.textWidth(layout.address);

  canvas.setFont(FontLadder[supportIndex]);
  const int supportHeight = canvas.fontHeight();

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
    fits = fits && ClearsBezel(canvas.textWidth(line), y, supportHeight);
    y += placement.linePitch;
  }

  placement.valid = fits;
  return placement;
}

void DrawLayout(LGFX& tft, const Layout& layout)
{
  LovyanGFX& canvas = PanelTrim::Canvas(tft);

  const int count = CountLines(layout);

  // The ladder is walked at scale 1 throughout -- picking a bigger face is what
  // this does instead of multiplying a small one.
  canvas.setTextSize(1);

  // Every pairing, largest first, emphasis before support: sixteen tries at
  // worst, once per screen. Written as a search rather than as two independent
  // choices because the two are not independent -- a bigger emphasis face
  // pushes the supporting lines further down the panel, where the curve leaves
  // them less width than they had.
  //
  // Support is never allowed above emphasis. A screen whose small print is set
  // larger than its heading is not a screen that has fitted itself to the
  // panel; it is one that has forgotten what it is saying.
  int emphasisIndex = SmallestFontIndex;
  int supportIndex = SmallestFontIndex;
  Placement placement;

  for (int emphasis = 0; emphasis < LadderSize && !placement.valid; emphasis++) {
    for (int support = emphasis; support < LadderSize; support++) {
      const Placement candidate = TryFaces(canvas, layout, count, emphasis, support);
      if (candidate.valid) {
        emphasisIndex = emphasis;
        supportIndex = support;
        placement = candidate;
        break;
      }
    }
  }

  if (!placement.valid) {
    // Nothing in the ladder fit. The smallest face is drawn anyway and allowed
    // to overrun the bezel -- a line clipped at the rim is still worth more to
    // whoever is reading it than a blank screen.
    placement = TryFaces(canvas, layout, count, SmallestFontIndex, SmallestFontIndex);
  }

  canvas.fillScreen(lgfx::color888(0, 0, 0));
  canvas.setTextColor(lgfx::color888(0, 255, 0));

  canvas.setFont(FontLadder[emphasisIndex]);
  if (!layout.heading.isEmpty())
    canvas.drawCentreString(layout.heading, SCREEN_SIZE_DIV_2, placement.headingTop);
  if (!layout.address.isEmpty())
    canvas.drawCentreString(layout.address, SCREEN_SIZE_DIV_2, placement.addressTop);

  if (layout.badgeSize > 0)
    ConfigQr::Draw(canvas,
                   (SCREEN_SIZE - layout.badgeSize) / 2,
                   placement.badgeTop,
                   layout.badgeSize);

  canvas.setFont(FontLadder[supportIndex]);
  int y = placement.firstLineTop;
  for (const String& line : layout.lines) {
    if (line.isEmpty())
      continue;
    canvas.drawCentreString(line, SCREEN_SIZE_DIV_2, y);
    y += placement.linePitch;
  }

  PanelTrim::Present(tft);

  // Everything else that draws on this canvas expects the default face at the
  // default scale. Reset on the canvas rather than on the panel, because with a
  // trim set those are different objects and the panel's own text state is
  // never touched by anything here.
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
                         const String& fallback,
                         const String& footnote)
{
  Layout layout;
  layout.heading = heading;
  layout.address = address;
  layout.lines[0] = fallback;
  layout.lines[1] = footnote;

  // Asked for before anything is laid out, because the badge is part of the
  // block the type has to fit around -- and it is only ever as big as a whole
  // number of pixels per module allows.
  layout.badgeSize = ConfigQr::SizeWithin(BadgeBudget);
  DrawLayout(tft, layout);
}
