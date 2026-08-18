#include "ui/SmoothText.h"

#include <algorithm>
#include <cmath>

namespace SmoothText {
namespace {

// Where the material comes from. See the header for why it is this one.
const lgfx::IFont* const Master = &fonts::DejaVu72;

// DejaVu Sans is a regular weight, and the screens this draws are green on
// black at arm's length, where a thin stem disappears -- which is why they were
// set bold in the first place. So the run is stamped several times a pixel
// apart in the master and the union of those stamps is what gets scaled down: a
// stem 7 master pixels wide becomes 10, which at the largest size on these
// screens lands within a pixel of the bold face it replaces.
//
// The offsets are in master pixels rather than final ones on purpose. The
// master is always the same size, so a fixed widening here is a fixed
// proportion of a letter, and it comes down with everything else -- the small
// sizes get no clumsier than the large ones.
//
// Three and two is as far as this goes. It is the point where the weight stops
// reading as lighter than what it replaces, and one more stamp either way
// starts to silt up the counters of 'e' and 'a' at the smallest size, where
// there are only a couple of pixels of hole to lose.
constexpr int EmboldenX = 3;
constexpr int EmboldenY = 2;

// A pixel of air around the run in the master, so the resample has something to
// average the outermost stems against rather than running off the edge of the
// buffer.
constexpr int Margin = 1;

// A run wanting a master wider than this is drawn stepped instead. Nothing on
// these screens comes near it -- the widest is a full-width line at the
// smallest size, around 1500 -- so this is only here to stop a mistake
// somewhere else from asking for a buffer the size of the heap.
constexpr int MaxMasterWidth = 2048;

// Filled behind the glyphs in both buffers and handed to the final push as the
// colour to skip, so what lands on the canvas is letterforms rather than a
// plate around them. Black rather than an unused key colour for the same reason
// PanelTrim uses black: the resample blends edge pixels towards whatever they
// sit on, and blending towards black gives a dark fringe where blending towards
// a key colour would give a coloured one.
constexpr uint32_t Backdrop = 0x000000u;

// What to set a run in when there is no room to compose it. Largest first; the
// last entry takes everything that reaches it.
constexpr struct { int height; const lgfx::IFont* face; } Fallback[] = {
  { 47, &fonts::FreeSansBold24pt7b },
  { 33, &fonts::FreeSansBold18pt7b },
  { 23, &fonts::FreeSansBold12pt7b },
  {  0, &fonts::FreeSansBold9pt7b  },
};

// The master is a fixed face, so a size is only ever a scale factor away.
float ScaleFor(LovyanGFX& gfx, int height)
{
    const int masterHeight = gfx.fontHeight(Master);
    return masterHeight > 0 ? static_cast<float>(height) / masterHeight : 0.0f;
}

// `master` area-averaged down into `scaled`: every destination pixel is the mean
// of the exact source rectangle behind it, each source pixel weighted by how
// much of it that rectangle actually covers.
//
// Doing this by hand rather than through pushRotateZoomWithAA, which is here to
// do exactly this and does not. Its averaging window is (1/zoom - 1) source
// pixels across -- one pixel narrower than the 1/zoom the ratio calls for --
// and the shortfall is a whole pixel at every ratio, so it hurts most where the
// reduction is gentlest. Coming down from the master at the 47px size these
// screens set their headings in, 1/zoom is 1.60 and the window is 0.60: narrower
// than a single source pixel, which means source and end coordinates land on the
// same pixel and the copy takes its nearest-neighbour fast path. The heading is
// point-sampled, at the cost of a resample and with none of the benefit.
//
// That is also why this only ever looked right on a unit with a trim set. The
// trim resamples the finished frame again on the way to the panel, at zoom 1.0
// where the same expression gives a window of 0.9998 -- a full pixel, a real
// 2x2 blend -- so the smoothing everyone was seeing came from the rotation and
// not from here.
// One source row at a time, and a destination row's worth of ink. Both are
// bounded by MaxMasterWidth -- DrawCentre turns away anything wider before a
// buffer is made -- and the scaled run is never wider than the master it came
// from, so one size covers both.
uint16_t sourceRow[MaxMasterWidth];
float destinationInk[MaxMasterWidth];

void Resample(LGFX_Sprite& master, LGFX_Sprite& scaled, uint32_t colour)
{
    const int srcWidth = master.width();
    const int srcHeight = master.height();
    const int dstWidth = scaled.width();
    const int dstHeight = scaled.height();
    if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0
     || srcWidth > MaxMasterWidth || dstWidth > MaxMasterWidth)
        return;

    // Taken from the buffer sizes rather than from the scale the caller worked
    // them out with, so the rounding that produced them cannot leave the run
    // sampling slightly past its own edge.
    const float stepX = static_cast<float>(srcWidth) / dstWidth;
    const float stepY = static_cast<float>(srcHeight) / dstHeight;

    const uint32_t red = (colour >> 16) & 0xFF;
    const uint32_t green = (colour >> 8) & 0xFF;
    const uint32_t blue = colour & 0xFF;

    for (int dy = 0; dy < dstHeight; dy++) {
        const float top = dy * stepY;
        const float bottom = std::min(top + stepY, static_cast<float>(srcHeight));
        const int lastRow = std::min(srcHeight - 1, static_cast<int>(ceilf(bottom)) - 1);

        for (int dx = 0; dx < dstWidth; dx++)
            destinationInk[dx] = 0.0f;

        for (int sy = static_cast<int>(top); sy <= lastRow; sy++) {
            const float rows = std::min(bottom, static_cast<float>(sy + 1))
                             - std::max(top, static_cast<float>(sy));

            // A row at a time rather than a pixel at a time. readPixel builds a
            // pixelcopy and calls into the panel for every pixel it is asked
            // for, which over a master this size is a tenth of a second a run.
            master.readRect(0, sy, srcWidth, 1, sourceRow);

            for (int dx = 0; dx < dstWidth; dx++) {
                const float left = dx * stepX;
                const float right = std::min(left + stepX, static_cast<float>(srcWidth));
                const int lastCol = std::min(srcWidth - 1, static_cast<int>(ceilf(right)) - 1);

                for (int sx = static_cast<int>(left); sx <= lastCol; sx++) {
                    // How much of this destination pixel is ink, not what
                    // colour the ink is. The master carries one colour on the
                    // backdrop and nothing else, so coverage is the whole
                    // story -- and a test against black is the one test that
                    // holds whatever pixel format the buffer happens to store,
                    // which is what the last attempt at this got wrong: it read
                    // RGB565 as though it were RGB888 and turned the green
                    // wordmark blue.
                    if (sourceRow[sx] == Backdrop)
                        continue;
                    destinationInk[dx] += rows * (std::min(right, static_cast<float>(sx + 1))
                                                - std::max(left, static_cast<float>(sx)));
                }
            }
        }

        // The area behind a destination pixel is the same on every row that
        // feeds it, so it is worked out from the window rather than summed.
        for (int dx = 0; dx < dstWidth; dx++) {
            const float left = dx * stepX;
            const float area = (std::min(left + stepX, static_cast<float>(srcWidth)) - left)
                             * (bottom - top);
            const float coverage = area > 0.0f ? destinationInk[dx] / area : 0.0f;

            scaled.drawPixel(dx, dy, lgfx::color888(
                static_cast<uint8_t>(lroundf(red * coverage)),
                static_cast<uint8_t>(lroundf(green * coverage)),
                static_cast<uint8_t>(lroundf(blue * coverage))));
        }
    }
}

// The run drawn the way it always was: one bit per pixel, stepped edges, in
// whichever bitmap face comes nearest the size asked for. A line that is there
// to be read is worth more stepped than not drawn at all.
//
// Widths do not quite agree with what Width() promised the caller -- the bitmap
// faces run a few percent wider than the master does at the same height -- so a
// line laid out to just clear the bezel can reach it. That is the right way
// round: this only ever runs when a buffer would not allocate, and a screen
// touching the rim beats a blank one.
void DrawStepped(LovyanGFX& canvas, const String& text, int centreX, int top,
                 int height, uint32_t colour)
{
    const lgfx::IFont* face = Fallback[sizeof(Fallback) / sizeof(*Fallback) - 1].face;
    for (const auto& step : Fallback) {
        if (height >= step.height) {
            face = step.face;
            break;
        }
    }

    canvas.setFont(face);
    canvas.setTextSize(1);
    canvas.setTextColor(colour);
    canvas.drawCentreString(text, centreX, top);
}

}

int Width(LovyanGFX& gfx, const String& text, int height)
{
    if (text.isEmpty() || height <= 0)
        return 0;

    // The emboldening is part of the run's width, so it has to be part of what
    // the layout is told about the run's width.
    const float scale = ScaleFor(gfx, height);
    return static_cast<int>(lroundf((gfx.textWidth(text.c_str(), Master) + EmboldenX) * scale));
}

void DrawCentre(LovyanGFX& canvas, const String& text, int centreX, int top,
                int height, uint32_t colour)
{
    if (text.isEmpty() || height <= 0)
        return;

    const float scale = ScaleFor(canvas, height);
    const int masterWidth = canvas.textWidth(text.c_str(), Master) + EmboldenX + 2 * Margin;
    const int masterHeight = canvas.fontHeight(Master) + EmboldenY + 2 * Margin;
    if (scale <= 0.0f || masterWidth <= 2 * Margin || masterWidth > MaxMasterWidth) {
        DrawStepped(canvas, text, centreX, top, height, colour);
        return;
    }

    const int scaledWidth = std::max(1, static_cast<int>(lroundf(masterWidth * scale)));
    const int scaledHeight = std::max(1, static_cast<int>(lroundf(masterHeight * scale)));

    // Two buffers rather than one because the resample cannot be pointed
    // straight at the panel: it blends what it writes with what is already
    // there, and reads the destination back to do it. This panel has no SDO
    // wired -- see LGFX.h -- so a read of it returns nothing meaningful. The
    // blend happens in a sprite, which is always readable, and what reaches the
    // canvas afterwards is a plain keyed copy that needs no read at all.
    LGFX_Sprite master;
    LGFX_Sprite scaled;
    if (ESP.getPsramSize() > 0) {
        master.setPsram(true);
        scaled.setPsram(true);
    }
    master.setColorDepth(16);
    scaled.setColorDepth(16);

    if (master.createSprite(masterWidth, masterHeight) == nullptr
     || scaled.createSprite(scaledWidth, scaledHeight) == nullptr) {
        Serial.printf("[WARN] no room for a %dx%d smooth-text buffer; drawing that run stepped\n",
                      masterWidth, masterHeight);
        master.deleteSprite();
        scaled.deleteSprite();
        DrawStepped(canvas, text, centreX, top, height, colour);
        return;
    }

    master.fillScreen(Backdrop);
    master.setFont(Master);
    master.setTextSize(1);
    // One colour, no background: each stamp has to leave the ones before it
    // alone, and an opaque draw would paint over them.
    master.setTextColor(colour);
    master.setTextDatum(textdatum_t::top_left);
    for (int dy = 0; dy <= EmboldenY; dy++)
        for (int dx = 0; dx <= EmboldenX; dx++)
            master.drawString(text, Margin + dx, Margin + dy);

    // The step that does the work, and the whole reason the master exists.
    Resample(master, scaled, colour);

    // Placed so the run lands exactly where drawCentreString would have put it:
    // centred on centreX, line box starting on `top`. The run sits centred in
    // the master horizontally, so the width falls out; vertically it is a
    // margin down from the top of it, which is what the last term takes back.
    // Resample() maps the master's top-left corner onto the buffer's, so the
    // margin comes down by the ratio the buffers actually ended up in.
    const float scaleY = static_cast<float>(scaledHeight) / masterHeight;
    scaled.pushSprite(&canvas,
                      static_cast<int>(lroundf(centreX - scaledWidth * 0.5f)),
                      static_cast<int>(lroundf(top - Margin * scaleY)),
                      Backdrop);

    master.deleteSprite();
    scaled.deleteSprite();
}

}
