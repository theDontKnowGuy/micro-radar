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
const lgfx::IFont* const FontLadder[] = {
  &fonts::FreeSansBold24pt7b,
  &fonts::FreeSansBold18pt7b,
  &fonts::FreeSansBold12pt7b,
  &fonts::FreeSansBold9pt7b,
};

constexpr int SmallestFontIndex = sizeof(FontLadder) / sizeof(*FontLadder) - 1;

// How many lines a screen can carry under its heading, and so how many slots
// every layout here is written for.
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

// Where the ladder starts once there is a badge in the block. The faces above
// this one do still fit -- nothing here would reject them for a heading as
// short as this screen's -- and that is exactly the problem: an address set as
// large as the panel allows, directly under a code that says the same thing,
// reads as two headlines arguing. The badge is the thing to look at; the lines
// are what it resolves to.
constexpr int BadgeFontIndex = 2;
static_assert(BadgeFontIndex <= SmallestFontIndex, "no such font");

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
// missing: the plain status screens are lines and nothing else.
struct Layout {
  String heading;
  String lines[MaxLines];
  int badgeSize = 0;
};

// Where each part of a laid-out screen starts, for a given face. Worked out
// once by the fitting pass and again by the drawing pass, from the same inputs,
// so the two cannot drift.
struct Placement {
  int headingTop = 0;
  int badgeTop = 0;
  int firstLineTop = 0;
  int lineHeight = 0;
  int blockTop = 0;
  int blockBottom = 0;
};

int CountLines(const Layout& layout)
{
  int count = 0;
  for (const String& line : layout.lines)
    count += line.isEmpty() ? 0 : 1;
  return count;
}

// The whole stack -- heading, badge, lines -- centred on the panel as one
// block, rather than each part placed against an edge. Centring the parts
// separately is what leaves a screen with its text pushed to the rim and a hole
// in the middle of it.
Placement Place(const Layout& layout, int fontHeight, int count)
{
  const int headingPart = layout.heading.isEmpty() ? 0 : fontHeight + BadgeGap;
  const int badgePart = layout.badgeSize > 0 ? layout.badgeSize + BadgeGap : 0;

  Placement placement;
  placement.lineHeight = fontHeight + LineGap;

  const int linesPart = count > 0 ? (count - 1) * placement.lineHeight + fontHeight : 0;
  const int blockHeight = headingPart + badgePart + linesPart;

  placement.blockTop = (SCREEN_SIZE - blockHeight) / 2;
  placement.blockBottom = placement.blockTop + blockHeight;
  placement.headingTop = placement.blockTop;
  placement.badgeTop = placement.blockTop + headingPart;
  placement.firstLineTop = placement.badgeTop + badgePart;
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

void DrawLayout(LGFX& tft, const Layout& layout)
{
  LovyanGFX& canvas = PanelTrim::Canvas(tft);

  const int count = CountLines(layout);

  // The ladder is walked at scale 1 throughout -- picking a bigger face is what
  // this does instead of multiplying a small one.
  canvas.setTextSize(1);

  int chosenIndex = SmallestFontIndex;
  Placement placement;

  for (int candidate = layout.badgeSize > 0 ? BadgeFontIndex : 0;
       candidate <= SmallestFontIndex;
       candidate++) {
    canvas.setFont(FontLadder[candidate]);

    const int fontHeight = canvas.fontHeight();
    const Placement candidatePlacement = Place(layout, fontHeight, count);

    // Room for the block itself, before any question of how wide its lines are.
    // Only the badge screens can fail this -- a picture takes the room it takes
    // whatever face the text ends up in -- but it is the whole reason the ladder
    // has to be walked with the badge already in the layout.
    bool fits = candidatePlacement.blockTop >= BezelMargin
             && candidatePlacement.blockBottom <= SCREEN_SIZE - BezelMargin;

    if (!layout.heading.isEmpty())
      fits = fits && ClearsBezel(canvas.textWidth(layout.heading),
                                 candidatePlacement.headingTop, fontHeight);

    int y = candidatePlacement.firstLineTop;
    for (const String& line : layout.lines) {
      if (line.isEmpty())
        continue;
      fits = fits && ClearsBezel(canvas.textWidth(line), y, fontHeight);
      y += candidatePlacement.lineHeight;
    }

    if (fits) {
      chosenIndex = candidate;
      placement = candidatePlacement;
      break;
    }
  }

  canvas.setFont(FontLadder[chosenIndex]);

  const int fontHeight = canvas.fontHeight();
  if (placement.lineHeight == 0) {
    // Nothing in the ladder fit. The smallest face is drawn anyway and allowed
    // to overrun the bezel -- a line clipped at the rim is still worth more to
    // whoever is reading it than a blank screen.
    placement = Place(layout, fontHeight, count);
  }

  canvas.fillScreen(lgfx::color888(0, 0, 0));
  canvas.setTextColor(lgfx::color888(0, 255, 0));

  if (!layout.heading.isEmpty())
    canvas.drawCentreString(layout.heading, SCREEN_SIZE_DIV_2, placement.headingTop);

  if (layout.badgeSize > 0)
    ConfigQr::Draw(canvas,
                   (SCREEN_SIZE - layout.badgeSize) / 2,
                   placement.badgeTop,
                   layout.badgeSize);

  int y = placement.firstLineTop;
  for (const String& line : layout.lines) {
    if (line.isEmpty())
      continue;
    canvas.drawCentreString(line, SCREEN_SIZE_DIV_2, y);
    y += placement.lineHeight;
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
                         const String& first,
                         const String& second,
                         const String& third)
{
  Layout layout;
  layout.heading = heading;
  layout.lines[0] = first;
  layout.lines[1] = second;
  layout.lines[2] = third;

  // Asked for before anything is laid out, because the badge is part of the
  // block the type has to fit around -- and it is only ever as big as a whole
  // number of pixels per module allows.
  layout.badgeSize = ConfigQr::SizeWithin(BadgeBudget);
  DrawLayout(tft, layout);
}
