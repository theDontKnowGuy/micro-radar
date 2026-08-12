#include "ui/StatusScreen.h"

#include <cmath>

#include "ui/PanelTrim.h"
#include "ui/ProjectQr.h"

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

// How close to the rim the longest line may come. The panel is round and sits
// in a bezel, so the outermost ring of pixels is never properly visible. Ten is
// about what the bezel actually covers; the old flat inset was SCREEN_SIZE/12,
// or thirty, which threw away a fifth of the usable width and bought nothing.
constexpr int BezelMargin = 10;

// Air between baselines, on top of whatever leading the face already carries.
// Small, because these fonts bring their own: the old value of 10 was there to
// separate lines of a bitmap font that had none.
constexpr int LineGap = 6;

// Where the badge sits, and how much of the panel it may have.
//
// A hundred pixels, which for the current URL comes out at three pixels a
// module and a 99px plate. Two pixels a module is where a phone stops being
// able to focus, and it is the only reason the budget is not smaller still --
// what is left over is what the lines underneath are read from.
//
// The top is set below the bezel margin rather than at it: the badge is square
// and the panel is round, so it is the plate's top corners that decide how far
// up it can go, not the middle of its top edge.
constexpr int BadgeBudget = 100;
constexpr int BadgeTop = 20;

// Between the badge and the first line of text.
constexpr int BadgeGap = 8;

// Where the ladder starts once there is a badge above the text, rather than at
// its top. The two faces above this one do still fit underneath a badge --
// nothing here would reject them -- and that is exactly the problem: an address
// set as large as the panel allows, directly under a code that says the same
// thing, reads as two headlines arguing. The badge is the thing to look at now;
// the lines are what it resolves to.
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

// Top edge of the first line of a block centred in the band between `regionTop`
// and `regionBottom`. Shared by the fitting pass and the drawing pass so the
// two cannot drift; the datum is top_center, so this is the top of the glyph
// cell rather than its baseline.
int FirstLineTop(int count, int lineHeight, int fontHeight, int regionTop, int regionBottom)
{
  const int centre = (regionTop + regionBottom) / 2;
  return centre - ((count - 1) * lineHeight) / 2 - fontHeight / 2;
}

void DrawStatus(LGFX& tft, const String lines[5], bool withBadge)
{
  LovyanGFX& canvas = PanelTrim::Canvas(tft);

  int count = 0;
  for (int i = 0; i < 5; i++)
    count += lines[i].isEmpty() ? 0 : 1;

  // Asked for before anything is drawn, because where the text may go depends
  // on how big the badge actually came out -- and it is only ever as big as a
  // whole number of pixels per module allows.
  const int badgeSize = withBadge ? ProjectQr::SizeWithin(BadgeBudget) : 0;

  // The band the text is centred in. Without a badge that is the whole raster,
  // which is what this screen has always done. With one it starts below the
  // badge, and stops short of the rim -- the block is no longer centred on the
  // panel, so nothing else is keeping its last line off the curve.
  const int regionTop = badgeSize > 0 ? BadgeTop + badgeSize + BadgeGap : 0;
  const int regionBottom = badgeSize > 0 ? SCREEN_SIZE - BezelMargin : SCREEN_SIZE;

  // The ladder is walked at scale 1 throughout -- picking a bigger face is what
  // this does instead of multiplying a small one.
  canvas.setTextSize(1);

  int chosenIndex = SmallestFontIndex;
  int lineHeight = 0;

  for (int candidate = badgeSize > 0 ? BadgeFontIndex : 0;
       candidate <= SmallestFontIndex;
       candidate++) {
    canvas.setFont(FontLadder[candidate]);

    const int fontHeight = canvas.fontHeight();
    const int candidateLineHeight = fontHeight + LineGap;

    int y = FirstLineTop(count, candidateLineHeight, fontHeight, regionTop, regionBottom);
    bool fits = y >= regionTop;

    for (const String* line = lines; line != lines + 5; ++line) {
      if (line->isEmpty())
        continue;

      // Whichever edge of this line sits nearer the rim decides how much width
      // it gets: the top edge for a line above the centre, the bottom edge for
      // one below it.
      const int top = y - SCREEN_SIZE_DIV_2;
      const int bottom = top + fontHeight;
      const int dy = abs(top) > abs(bottom) ? abs(top) : abs(bottom);

      if (canvas.textWidth(*line) > 2 * HalfWidthAt(dy))
        fits = false;

      y += candidateLineHeight;
    }

    // The last line's own depth, not the pitch that would carry a further one.
    if (y - LineGap > regionBottom)
      fits = false;

    if (fits) {
      chosenIndex = candidate;
      lineHeight = candidateLineHeight;
      break;
    }
  }

  canvas.setFont(FontLadder[chosenIndex]);

  const int fontHeight = canvas.fontHeight();
  if (lineHeight == 0) {
    // Nothing in the ladder fit. The smallest face is drawn anyway and allowed
    // to overrun the bezel -- a line clipped at the rim is still worth more to
    // whoever is reading it than a blank screen.
    lineHeight = fontHeight + LineGap;
  }

  canvas.fillScreen(lgfx::color888(0, 0, 0));

  if (badgeSize > 0)
    ProjectQr::Draw(canvas, (SCREEN_SIZE - badgeSize) / 2, BadgeTop, badgeSize);

  canvas.setTextColor(lgfx::color888(0, 255, 0));

  int y = FirstLineTop(count, lineHeight, fontHeight, regionTop, regionBottom);
  for (const String* line = lines; line != lines + 5; ++line) {
    if (line->isEmpty())
      continue;
    canvas.drawCentreString(*line, SCREEN_SIZE_DIV_2, y);
    y += lineHeight;
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
  const String lines[] = { first, second, third, fourth, fifth };
  DrawStatus(tft, lines, false);
}

void ShowStatusScreenWithQr(LGFX& tft,
                            const String& first,
                            const String& second,
                            const String& third,
                            const String& fourth,
                            const String& fifth)
{
  const String lines[] = { first, second, third, fourth, fifth };
  DrawStatus(tft, lines, true);
}
