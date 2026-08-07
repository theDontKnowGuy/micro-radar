#include "ui/ProgressBar.h"

#include <algorithm>

namespace ProgressBar {

void DrawOutline(LovyanGFX& canvas, int y, uint32_t color)
{
  canvas.drawRoundRect(X, y, Width, Height, Radius, color);
}

void DrawFill(LovyanGFX& canvas, int y, int percent, uint32_t color)
{
  if (percent <= 0)
    return;

  // A pill cannot be narrower than its two caps, so anything shorter is drawn
  // as the round dot those caps make together -- the alternative is squaring
  // the radius off for the first few percent, which is the artefact this is
  // all trying to avoid.
  const int fill = std::max((Width * percent) / 100, Height);
  canvas.fillRoundRect(X, y, fill, Height, Radius, color);
}

}
