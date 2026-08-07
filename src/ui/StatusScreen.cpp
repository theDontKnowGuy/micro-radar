#include "ui/StatusScreen.h"

#include <cmath>

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

// Top edge of the first line of a centred block. Shared by the fitting pass and
// the drawing pass so the two cannot drift; the datum is top_center, so this is
// the top of the glyph cell rather than its baseline.
int FirstLineTop(int count, int lineHeight, int fontHeight)
{
  return SCREEN_SIZE_DIV_2 - ((count - 1) * lineHeight) / 2 - fontHeight / 2;
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
  int count = 0;
  for (const String& line : lines)
    count += line.isEmpty() ? 0 : 1;

  // The ladder is walked at scale 1 throughout -- picking a bigger face is what
  // this does instead of multiplying a small one.
  tft.setTextSize(1);

  int chosenIndex = SmallestFontIndex;
  int lineHeight = 0;

  for (int candidate = 0; candidate <= SmallestFontIndex; candidate++) {
    tft.setFont(FontLadder[candidate]);

    const int fontHeight = tft.fontHeight();
    const int candidateLineHeight = fontHeight + LineGap;

    int y = FirstLineTop(count, candidateLineHeight, fontHeight);
    bool fits = true;

    for (const String& line : lines) {
      if (line.isEmpty())
        continue;

      // Whichever edge of this line sits nearer the rim decides how much width
      // it gets: the top edge for a line above the centre, the bottom edge for
      // one below it.
      const int top = y - SCREEN_SIZE_DIV_2;
      const int bottom = top + fontHeight;
      const int dy = abs(top) > abs(bottom) ? abs(top) : abs(bottom);

      if (tft.textWidth(line) > 2 * HalfWidthAt(dy))
        fits = false;

      y += candidateLineHeight;
    }

    if (fits) {
      chosenIndex = candidate;
      lineHeight = candidateLineHeight;
      break;
    }
  }

  tft.setFont(FontLadder[chosenIndex]);

  const int fontHeight = tft.fontHeight();
  if (lineHeight == 0) {
    // Nothing in the ladder fit. The smallest face is drawn anyway and allowed
    // to overrun the bezel -- a line clipped at the rim is still worth more to
    // whoever is reading it than a blank screen.
    lineHeight = fontHeight + LineGap;
  }

  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.setTextColor(lgfx::color888(0, 255, 0));

  int y = FirstLineTop(count, lineHeight, fontHeight);
  for (const String& line : lines) {
    if (line.isEmpty())
      continue;
    tft.drawCentreString(line, SCREEN_SIZE_DIV_2, y);
    y += lineHeight;
  }

  // Everything else that draws straight to the panel expects the default face
  // at the default scale.
  tft.setFont(&fonts::Font0);
  tft.setTextSize(1);
}
