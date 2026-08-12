#include "ui/SmoothText.h"

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

    // The step that does the work. Always the antialiasing resampler: it
    // integrates each destination pixel over the whole source area behind it,
    // which for a run coming down by a third or more is the difference between
    // a graded edge and a differently-stepped one.
    scaled.fillScreen(Backdrop);
    master.setPivot(masterWidth * 0.5f, masterHeight * 0.5f);
    master.pushRotateZoomWithAA(&scaled,
                                scaledWidth * 0.5f, scaledHeight * 0.5f,
                                0.0f, scale, scale);

    // Placed so the run lands exactly where drawCentreString would have put it:
    // centred on centreX, line box starting on `top`. The run sits centred in
    // the master horizontally, so the width falls out; vertically it is a
    // margin down from the top of it, which is what the last term takes back.
    const float boxTop = (scaledHeight - masterHeight * scale) * 0.5f;
    scaled.pushSprite(&canvas,
                      static_cast<int>(lroundf(centreX - scaledWidth * 0.5f)),
                      static_cast<int>(lroundf(top - boxTop - Margin * scale)),
                      Backdrop);

    master.deleteSprite();
    scaled.deleteSprite();
}

}
