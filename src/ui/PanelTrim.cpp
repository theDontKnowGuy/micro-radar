#include "ui/PanelTrim.h"

#include <Arduino.h>

#include "DisplayConfig.h"

namespace PanelTrim {
namespace {

// The angle being applied, which may legitimately be zero.
float trimDegrees = 0.0f;

// Whether the full-screen UIs are composing into the scratch buffer, which they
// do whether or not there is an angle to apply. Separate from trimDegrees
// because the two really are different questions now: the buffer is about how a
// screen reaches the panel, the angle only about what happens to it on the way.
//
// They used to be the same flag, on the reasoning that a zero angle has nothing
// to correct and so needs no buffer. What that missed is that the trip through
// the buffer is not only a rotation -- it is a resample, and a resample is what
// gives these screens their graded edges. Skipping it at zero meant the one
// configuration with nothing wrong with its glass was the only one drawing its
// text raw, and it showed.
bool composing = false;

// The radar face's own centre -- the point its range rings are drawn about, and
// so the point every placement on it is judged against.
constexpr int FACE_CENTRE = SCREEN_SIZE_DIV_2 - 1;

// A run that wants more than this is drawn square instead of turned. Nothing on
// the face comes close -- the clock is the widest at around 260 -- so this is
// only here to keep a mistake somewhere else from asking for a buffer the size
// of the heap.
constexpr int MAX_TEXT_BUFFER = 448;

constexpr int SLOT_COUNT = static_cast<int>(TextSlot::Count);

// The longest run any slot carries is the wind, at around twenty characters.
// A run past this is drawn square rather than truncated -- losing the end of a
// label to make the rest of it level is not a trade worth making.
constexpr size_t MAX_TEXT_LENGTH = 31;

struct TurnedText {
    // Where the run is drawn square, and where it is kept once turned. Two
    // buffers rather than one because the turning is what this is trying to
    // stop doing every frame: these runs change their contents far less often
    // than they are drawn -- the clock's figures only turn over as the sweep
    // beam passes them, a location name never -- so the expensive step happens
    // when the text changes, and every frame in between is a plain copy of the
    // result.
    //
    // That is the whole reason this is affordable. Resampling thirty thousand
    // pixels per frame is what put the radar at thirteen frames a second;
    // resampling them a few times a revolution costs nothing measurable, and
    // copying them is the same work as drawing the text was in the first place.
    LGFX_Sprite square;
    LGFX_Sprite turned;

    // What `turned` currently holds, and whether it holds anything at all.
    char rendered[MAX_TEXT_LENGTH + 1] = {};
    const lgfx::IFont* font = nullptr;
    uint32_t colour = 0;
    bool ready = false;
};

TurnedText slots[SLOT_COUNT];

// Filled behind the glyphs while a run is composed, and handed to the push as
// the colour to skip, so what lands on the face is letterforms and not a plate
// around them. Black rather than an unused key colour on purpose: turning a run
// blends its edge pixels towards whatever they sit on, and blending towards
// black gives a dark fringe where blending towards a key colour would give a
// coloured one.
constexpr uint32_t TEXT_BACKDROP = 0x000000u;

// Sixteen bits per pixel where the radar backbuffer makes do with eight. The
// boot logo is full-colour artwork, and RGB332 puts visible bands through the
// blue of the wordmark; the radar face is green on black and never notices the
// difference. Costs 259,200 bytes of PSRAM on every unit -- the radar keeps its
// own backbuffer and never comes through here, so this is only ever live while
// a full-screen UI is up.
LGFX_Sprite scratch;

// Carries a point round the same arc a turned run's letterforms travel.
//
// Turning the glyphs level is only half of putting a label straight. The panel
// is tilted bodily, so a point 141 rows below the middle of the face arrives
// 141*sin(3deg) -- about seven pixels -- to one side of where it looks like it
// should be, and a label centred in the raster reads as sitting off-centre in
// the bezel.
//
// The centre used is the radar face's own, the one the range rings are drawn
// about, because those rings are what anyone judges a placement against. A
// rotation about it leaves every distance from the middle unchanged, so nothing
// here can push a label into the curve that was clear of it before.
void TurnAboutFace(int& x, int& y)
{
    if (trimDegrees == 0.0f)
        return;

    const float turn = trimDegrees * DEG_TO_RAD;
    const float cosTurn = cosf(turn);
    const float sinTurn = sinf(turn);
    const float dx = static_cast<float>(x - FACE_CENTRE);
    const float dy = static_cast<float>(y - FACE_CENTRE);

    x = FACE_CENTRE + static_cast<int>(lroundf(dx * cosTurn - dy * sinTurn));
    y = FACE_CENTRE + static_cast<int>(lroundf(dx * sinTurn + dy * cosTurn));
}

// Sends a whole composed screen to the panel, turned.
//
// Called at a zero angle too, where it is not a wasted copy: LovyanGFX blends
// each destination pixel over a window of max(|cos|,|sin|)/zoom - 1 source
// pixels, which at zoom 1.0 is a full pixel however small the angle, so a frame
// that is not being turned at all still comes out of here graded.
//
// Rotating about the centre is what makes it safe to do without clearing
// underneath first: the panel shows
// the raster's inscribed circle, and rotating that circle about its own centre
// maps it onto itself, so every visible pixel is always covered by some source
// pixel. What the turned square fails to reach is its four corners, which sit
// under the bezel.
//
// Always the blending resampler, never the nearest-pixel one. Taking the nearest
// costs a letterform a pixel of stem width here and there along its length,
// which is the ragged look this started out with. The screens that come through
// here are text and nothing else, and none of them is on a frame budget, so
// there is nowhere for the extra work to show.
void PushTurned(LGFX_Sprite& frame, LovyanGFX* destination)
{
    frame.pushRotateZoomWithAA(destination,
                               FACE_CENTRE, FACE_CENTRE,
                               trimDegrees, 1.0f, 1.0f);
}

}

bool Begin(float degrees)
{
    trimDegrees = 0.0f;
    composing = false;

    // Written the long way round so a NaN out of the stored setting falls
    // through to "no trim" rather than past the bounds check.
    const bool wanted = degrees != 0.0f
        && degrees >= -SCREEN_TRIM_MAX_DEGREES
        && degrees <= SCREEN_TRIM_MAX_DEGREES;

    // Same reasoning as the radar backbuffer: a quarter-megabyte out of the
    // internal heap would leave too little contiguous SRAM for the TLS
    // handshake OpenSky needs.
    if (ESP.getPsramSize() > 0)
        scratch.setPsram(true);

    scratch.setColorDepth(16);
    if (!scratch.createSprite(SCREEN_SIZE, SCREEN_SIZE)) {
        Serial.printf("[WARN] no room for the %dx%d screen buffer; drawing straight to the panel\n",
                      SCREEN_SIZE, SCREEN_SIZE);
        return !wanted;
    }
    composing = true;

    if (!wanted)
        return true;

    // The per-slot text buffers are not allocated here. Their sizes come from
    // the text that goes in them, which nothing knows yet, so each one is made
    // the first time its slot draws anything and kept at that size afterwards.
    for (TurnedText& slot : slots) {
        if (ESP.getPsramSize() > 0) {
            slot.square.setPsram(true);
            slot.turned.setPsram(true);
        }
        // Sixteen bits so the turning has somewhere to put its graded edges.
        // They are quantised on the way into the 8-bit frame afterwards, but
        // quantising a blend is not the same as never computing one.
        slot.square.setColorDepth(16);
        slot.turned.setColorDepth(16);
    }

    // Only now, with somewhere to compose into, does the angle become real.
    trimDegrees = degrees;
    Serial.printf("Panel trim: %.1f degrees\n", trimDegrees);
    return true;
}

float Degrees()
{
    return trimDegrees;
}

LovyanGFX& Canvas(LGFX& tft)
{
    if (!composing)
        return tft;
    return scratch;
}

void Present(LGFX& tft)
{
    // Nothing to send: the buffer would not allocate, so the caller has been
    // drawing on the panel all along.
    if (!composing)
        return;

    PushTurned(scratch, &tft);
}

void FillTurnedPlate(LGFX_Sprite& target, int centreX, int centreY,
                     int width, int height, uint32_t colour)
{
    TurnAboutFace(centreX, centreY);

    if (trimDegrees == 0.0f) {
        target.fillRect(centreX - width / 2, centreY - height / 2, width, height, colour);
        return;
    }

    // Drawn as two triangles on turned corners rather than as an upright
    // rectangle. An upright one is what the clock had, and against a range ring
    // it gave the whole trim away: the digits sitting level inside a plate
    // still tilted three degrees, cutting the ring at one height on the left
    // and another on the right.
    const float turn = trimDegrees * DEG_TO_RAD;
    const float cosTurn = cosf(turn);
    const float sinTurn = sinf(turn);
    const float halfWidth = width * 0.5f;
    const float halfHeight = height * 0.5f;
    const float cornerX[4] = { -halfWidth,  halfWidth, halfWidth, -halfWidth };
    const float cornerY[4] = { -halfHeight, -halfHeight, halfHeight, halfHeight };

    int px[4], py[4];
    for (int corner = 0; corner < 4; corner++) {
        px[corner] = centreX + static_cast<int>(lroundf(cornerX[corner] * cosTurn - cornerY[corner] * sinTurn));
        py[corner] = centreY + static_cast<int>(lroundf(cornerX[corner] * sinTurn + cornerY[corner] * cosTurn));
    }

    target.fillTriangle(px[0], py[0], px[1], py[1], px[2], py[2], colour);
    target.fillTriangle(px[0], py[0], px[2], py[2], px[3], py[3], colour);
}

void PushFrame(LGFX_Sprite& frame)
{
    // One transfer, whole frame, at any trim. The face itself is rings and a
    // sweep, which look the same at every angle, so this stays the plain buffer
    // walk it was before the trim existed -- and the sweep stays as smooth.
    frame.pushSprite(0, 0);
}

void DrawTurnedText(LGFX_Sprite& target, TextSlot slot, const char* text, int centreX, int centreY)
{
    const int width = target.textWidth(text);
    const int height = target.fontHeight();

    // Rendered at the size asked for and no larger. This did briefly draw the
    // small faces at double size and average them back down, on the theory that
    // a one-pixel stem needs more material before it can be turned -- but the
    // faces on the radar face are bitmaps, and setTextSize on a bitmap is pixel
    // replication. Doubling one adds no detail to average, so halving it again
    // returns exactly the stems it started with, having resampled them twice on
    // the way. Getting more material means a face that can actually be drawn
    // larger, which is a question about which font the label uses, not about
    // this.
    // Sized to what this run's own turn actually needs rather than to a flat
    // allowance. A block turned about its centre grows by its other dimension
    // times the sine of the angle -- for a 108-wide rim label at three degrees
    // that is six rows and one column, where the flat sixteen this used to add
    // was enough to push the plate through the outer range ring. Two pixels
    // over, one a side, for the rounding.
    const float turn = fabsf(trimDegrees) * DEG_TO_RAD;
    const int bufferWidth = width + static_cast<int>(ceilf(height * sinf(turn))) + 2;
    const int bufferHeight = height + static_cast<int>(ceilf(width * sinf(turn))) + 2;

    TurnAboutFace(centreX, centreY);

    // Nothing to correct, nothing to draw, or a run past what this is willing
    // to allocate for: draw it where it was always drawn. A label square with
    // the panel is worth more than no label at all.
    if (trimDegrees == 0.0f || width <= 0 || strlen(text) > MAX_TEXT_LENGTH
     || bufferWidth > MAX_TEXT_BUFFER || bufferHeight > MAX_TEXT_BUFFER) {
        target.drawCentreString(text, centreX, centreY - height / 2);
        return;
    }

    TurnedText& entry = slots[static_cast<int>(slot)];

    // Sized to this slot's own text and kept. The runs here change contents far
    // more often than they change extents -- a clock ticking over is the same
    // five characters -- so in practice this allocates once and then never
    // again.
    const bool resized = entry.square.width() != bufferWidth
                      || entry.square.height() != bufferHeight;
    if (resized) {
        entry.ready = false;
        entry.square.deleteSprite();
        entry.turned.deleteSprite();
        if (entry.square.createSprite(bufferWidth, bufferHeight) == nullptr
         || entry.turned.createSprite(bufferWidth, bufferHeight) == nullptr) {
            Serial.printf("[WARN] no room for a %dx%d turned-text buffer; drawing it square\n",
                          bufferWidth, bufferHeight);
            entry.square.deleteSprite();
            entry.turned.deleteSprite();
            target.drawCentreString(text, centreX, centreY - height / 2);
            return;
        }
    }

    // The expensive half, and the half that almost never runs.
    //
    // What is cached is a picture, so anything that would change the picture has
    // to be part of what decides whether it is stale -- not just the characters.
    // Nothing on the face currently recolours a label or reletters one without
    // also changing its text, so keying on the string alone would work today and
    // fail silently the first time something did.
    const lgfx::IFont* font = target.getFont();
    const uint32_t colour = target.getTextStyle().fore_rgb888;
    if (!entry.ready || entry.font != font || entry.colour != colour
     || strcmp(entry.rendered, text) != 0) {
        entry.square.setFont(font);
        entry.square.setTextStyle(target.getTextStyle());
        entry.square.setTextDatum(textdatum_t::top_left);

        entry.square.fillScreen(TEXT_BACKDROP);
        entry.square.drawString(text,
                                (bufferWidth - width) / 2,
                                (bufferHeight - height) / 2);

        entry.turned.fillScreen(TEXT_BACKDROP);
        entry.square.pushRotateZoomWithAA(&entry.turned,
                                          bufferWidth / 2, bufferHeight / 2,
                                          trimDegrees, 1.0f, 1.0f);

        strncpy(entry.rendered, text, MAX_TEXT_LENGTH);
        entry.rendered[MAX_TEXT_LENGTH] = '\0';
        entry.font = font;
        entry.colour = colour;
        entry.ready = true;
    }

    // The cheap half, every frame: a straight copy of an already-turned block
    // into the frame it belongs to, so it reaches the panel in the same single
    // transfer as everything else and cannot flicker against it.
    //
    // Keyed on the backdrop rather than copied opaque. Opaque meant every run
    // laid down a rectangle of black as wide as itself, and near the rim that
    // rectangle reached the outer range ring and cut it -- the label solver
    // reserves the room the *text* needs, and knows nothing about a plate drawn
    // larger than that. Only the glyphs land now. Edge pixels that the turn
    // blended part of the way to black are not exactly the backdrop and so
    // still draw, which is what keeps the letterforms smooth.
    entry.turned.pushSprite(&target,
                            centreX - bufferWidth / 2,
                            centreY - bufferHeight / 2,
                            TEXT_BACKDROP);
}

}
