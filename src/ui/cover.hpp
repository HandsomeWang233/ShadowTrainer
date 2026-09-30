#pragma once
// The startup cover: the word sequence this window plays while WebView2 comes
// up, before there is a page to show.
//
// Split out of webui.cpp so it can be rendered into an off-screen bitmap and
// looked at without a window. That is the only way to check it really does look
// like the page's own animation - nobody can see the window during a test run,
// and "it compiles" says nothing about whether the word is centred, sized right
// or lit right.
//
// Every number in here mirrors web/css/app.css: the same face and em size, the same
// letter-spacing, the same cubic-bezier curves, the same colours, the same glow.
#include <windows.h>

namespace ce::cover {

// GDI objects that outlive a single frame. Rebuilt when the window crosses a
// size step. The window owns one of these.
struct State {
    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State();

    HFONT word_font = nullptr;
    int word_em = 0;
    int word_cell = 0;
    int first_width = 0;
    int prefix[8]{};         // widths of the first i letters of the second word

    HFONT note_font = nullptr;
    int note_em = 0;

    HBITMAP glow = nullptr;  // pre-rendered radial gradient, stretched to fit
    int glow_side = 0;
    HDC glow_dc = nullptr;   // holds it, so a frame does not build one to blit
};

// Milliseconds from the first painted frame to the moment the page may be shown.
ULONGLONG duration_ms();

// Draws the frame at `elapsed` milliseconds into dc, filling `client`.
// `background` paints the field and `background_colour` is the same colour as a
// value, for the fades that have to blend into it.
void paint(HDC dc, const RECT& client, HBRUSH background, COLORREF background_colour,
           State& state, ULONGLONG elapsed);

} // namespace ce::cover
