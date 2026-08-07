#include "ui/StatusScreen.h"

void ShowStatusScreen(LGFX& tft,
                      const String& first,
                      const String& second,
                      const String& third,
                      const String& fourth,
                      const String& fifth)
{
  constexpr int MAX_TEXT_SIZE = 4;
  constexpr int USABLE_WIDTH = SCREEN_SIZE - SCREEN_SIZE / 12;

  const String lines[] = { first, second, third, fourth, fifth };
  int count = 0;
  for (const String& line : lines)
    count += line.isEmpty() ? 0 : 1;

  int textSize = 1;
  for (int candidate = MAX_TEXT_SIZE; candidate > 1; candidate--) {
    tft.setTextSize(candidate);
    bool fits = true;
    for (const String& line : lines)
      if (!line.isEmpty() && tft.textWidth(line) > USABLE_WIDTH)
        fits = false;

    if (fits) {
      textSize = candidate;
      break;
    }
  }

  tft.setTextSize(textSize);
  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.setTextColor(lgfx::color888(0, 255, 0));

  const int lineHeight = tft.fontHeight() + 10;
  int y = SCREEN_SIZE_DIV_2 - ((count - 1) * lineHeight) / 2;
  for (const String& line : lines) {
    if (line.isEmpty())
      continue;
    tft.drawCentreString(line, SCREEN_SIZE_DIV_2, y);
    y += lineHeight;
  }

  // Everything else that draws straight to the panel expects the default scale.
  tft.setTextSize(1);
}
