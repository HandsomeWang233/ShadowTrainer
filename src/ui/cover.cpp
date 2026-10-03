#include "cover.hpp"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace ce::cover {
namespace {

// ---- the sequence, mirrored from web/css/app.css ------------------------------

constexpr wchar_t first_word[] = L"Shadow";
constexpr wchar_t rest_word[] = L"Trainer";
constexpr wchar_t note_text[] = L"INITIALISING";
constexpr int letters = 7;

constexpr ULONGLONG fly_delay = 180;
constexpr ULONGLONG fly_span = 780;
// The fly curve is front-loaded, so the word is visibly still from about 750ms
// even though its span runs to 960. The letters start there rather than after a
// beat: any gap between "the word has stopped" and "the row starts widening" is
// dead time in the middle of the move, and every version of it has read as a
// stutter at the hand-over. The two now overlap, so the creep of the fly's tail
// becomes the start of the glide and the motion never stops.
constexpr ULONGLONG letter_start = 780;
constexpr ULONGLONG letter_step = 95;
constexpr ULONGLONG letter_span = 340;
// Derived rather than written down, so the last letter's landing and the fade
// below cannot drift apart when a timing above is tuned.
constexpr ULONGLONG word_ms = letter_start + (letters - 1) * letter_step + letter_span;
static_assert(fly_delay + fly_span > letter_start, "the letters must start before the fly's span ends");
// Held complete before the fade. Two seconds was the first number and read as a
// pause on the way to the UI; half a second is a beat instead.
constexpr ULONGLONG hold_ms = 500;
constexpr ULONGLONG fade_ms = 260;
constexpr ULONGLONG total_ms = word_ms + hold_ms + fade_ms;

constexpr ULONGLONG glow_delay = 100;
constexpr ULONGLONG glow_span = 1500;
constexpr int glow_limit = 1180;         // min(135vw, 1180px)
constexpr int glow_bitmap = 256;         // pre-rendered once, then stretched
constexpr double glow_from = 0.55;       // the scale the glow grows from

constexpr ULONGLONG note_delay = 1200;
constexpr ULONGLONG note_span = 1000;
constexpr ULONGLONG note_breathe_delay = 2400;
constexpr ULONGLONG note_breathe_span = 2000;
constexpr int note_em = 13;              // a step up from --fs-2xs, which is too
                                         // small to read under the word
constexpr int note_tracking = 4;         // letter-spacing: 0.34em

constexpr COLORREF first_ink = RGB(0xf6, 0xfa, 0xff);
constexpr COLORREF rest_ink = RGB(0xb9, 0xe6, 0xff);
constexpr COLORREF note_ink = RGB(150, 175, 205);
constexpr double note_alpha = 0.55;
constexpr double note_offset = 0.62;     // of the word's em box, below centre
constexpr double note_gap = 26.0;        // plus this many pixels

constexpr double pi = 3.14159265358979323846;

// ---- small maths ----------------------------------------------------------

double progress(ULONGLONG elapsed, ULONGLONG delay, ULONGLONG span) {
    if (elapsed <= delay) return 0.0;
    if (span == 0) return 1.0;
    const double t = static_cast<double>(elapsed - delay) / static_cast<double>(span);
    return t >= 1.0 ? 1.0 : t;
}

// A CSS cubic-bezier timing function, solved rather than approximated, so the
// cover moves at the page's speed between the keyframes and not only at them.
double bezier(double x, double x1, double y1, double x2, double y2) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    double low = 0.0, high = 1.0, t = x;
    for (int i = 0; i < 24; ++i) {   // bisection; far finer than a frame can show
        const double guess = (low + high) * 0.5;
        const double u = 1.0 - guess;
        const double at = 3.0 * u * u * guess * x1 +
                          3.0 * u * guess * guess * x2 + guess * guess * guess;
        if (at < x) low = guess; else high = guess;
        t = guess;
    }
    const double u = 1.0 - t;
    return 3.0 * u * u * t * y1 + 3.0 * u * t * t * y2 + t * t * t;
}

// The curves the sequence runs on: the fly-in and the glow. The letters do not
// use one -- their box, fade, lift and scale all run off a single linear clock,
// which is what keeps the row's re-centring in step with the ink; see paint().
double fly_curve(double t) { return bezier(t, 0.16, 1.0, 0.3, 1.0); }
double glow_curve(double t) { return bezier(t, 0.2, 0.8, 0.3, 1.0); }

BYTE mix(BYTE from, BYTE to, double t) {
    return static_cast<BYTE>(from * (1.0 - t) + to * t + 0.5);
}

COLORREF blend(COLORREF from, COLORREF to, double t) {
    return RGB(mix(GetRValue(from), GetRValue(to), t),
               mix(GetGValue(from), GetGValue(to), t),
               mix(GetBValue(from), GetBValue(to), t));
}

// ---- fonts ----------------------------------------------------------------

int CALLBACK family_found(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) {
    *reinterpret_cast<bool*>(param) = true;
    return 0;   // one is enough
}

// The face web/css/app.css asks for first, when this machine has it. GDI quietly
// substitutes the default face for a missing family, so look it up first.
const wchar_t* face(HDC dc) {
    static const wchar_t* cached = nullptr;
    if (cached) return cached;
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(query.lfFaceName, LF_FACESIZE, L"Segoe UI Variable Text", _TRUNCATE);
    bool found = false;
    EnumFontFamiliesExW(dc, &query, family_found, reinterpret_cast<LPARAM>(&found), 0);
    cached = found ? L"Segoe UI Variable Text" : L"Segoe UI";
    return cached;
}

void make_font(HDC dc, HFONT& font, int em, int weight) {
    if (font) {
        DeleteObject(font);
        font = nullptr;
    }
    // GDI's lfHeight is the em size, which is the number the stylesheet uses.
    //
    // ANTIALIASED_QUALITY is greyscale, and it is deliberate: the page sets
    // -webkit-font-smoothing: antialiased, which turns Chromium's subpixel
    // rendering off. ClearType here would put red and blue fringes on every
    // stem - very visible under big white type on a dark field - and leave the
    // two copies of the word looking different.
    font = CreateFontW(-em, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH, face(dc));
}

void ensure_word_font(HDC dc, State& state, int em) {
    if (state.word_font && state.word_em == em) return;
    make_font(dc, state.word_font, em, FW_BOLD);
    if (!state.word_font) return;
    state.word_em = em;

    const HGDIOBJ previous = SelectObject(dc, state.word_font);
    // web/css/app.css sets letter-spacing: -0.02em, and GDI applies the extra to
    // GetTextExtent as well as to TextOut, so measuring and drawing agree.
    SetTextCharacterExtra(dc, -MulDiv(em, 2, 100));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    state.word_cell = metrics.tmHeight;
    SIZE measured{};
    GetTextExtentPoint32W(dc, first_word, static_cast<int>(std::size(first_word)) - 1, &measured);
    state.first_width = measured.cx;
    for (int i = 0; i <= letters; ++i) {
        SIZE part{};
        GetTextExtentPoint32W(dc, rest_word, i, &part);
        state.prefix[i] = part.cx;
    }
    SetTextCharacterExtra(dc, 0);
    SelectObject(dc, previous);
}

// ---- the glow -------------------------------------------------------------

// The stylesheet's radial-gradient, pre-rendered once and then stretched. It is
// a soft blob, so scaling costs nothing visible and saves rebuilding a gradient
// on every frame of the fade-in.
void ensure_glow(HDC dc, State& state) {
    if (state.glow) return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = glow_bitmap;
    info.bmiHeader.biHeight = -glow_bitmap;   // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        return;
    }

    auto* out = static_cast<DWORD*>(pixels);
    const double centre = glow_bitmap / 2.0;
    for (int y = 0; y < glow_bitmap; ++y) {
        for (int x = 0; x < glow_bitmap; ++x) {
            const double dx = (x + 0.5 - centre) / centre;
            const double dy = (y + 0.5 - centre) / centre;
            const double r = std::sqrt(dx * dx + dy * dy);
            // The stylesheet's four stops, as colour and alpha.
            double alpha = 0.0;
            COLORREF colour = RGB(70, 165, 235);
            if (r <= 0.34) {
                const double t = r / 0.34;
                alpha = 0.30 + (0.14 - 0.30) * t;
                colour = blend(RGB(132, 96, 226), RGB(96, 120, 235), t);
            } else if (r <= 0.52) {
                const double t = (r - 0.34) / 0.18;
                alpha = 0.14 + (0.07 - 0.14) * t;
                colour = blend(RGB(96, 120, 235), RGB(70, 165, 235), t);
            } else if (r <= 0.72) {
                alpha = 0.07 * (1.0 - (r - 0.52) / 0.20);
            }
            // AlphaBlend wants premultiplied BGRA.
            const double a = std::clamp(alpha, 0.0, 1.0);
            const BYTE alpha8 = static_cast<BYTE>(a * 255.0 + 0.5);
            out[y * glow_bitmap + x] =
                (static_cast<DWORD>(alpha8) << 24) |
                (static_cast<DWORD>(GetRValue(colour) * a + 0.5) << 16) |
                (static_cast<DWORD>(GetGValue(colour) * a + 0.5) << 8) |
                static_cast<DWORD>(GetBValue(colour) * a + 0.5);
        }
    }
    state.glow = bitmap;
    state.glow_side = glow_bitmap;
    // Held in its own DC: creating one per frame is the kind of cost that only
    // shows up as a stutter.
    state.glow_dc = CreateCompatibleDC(dc);
    if (state.glow_dc) SelectObject(state.glow_dc, state.glow);
}

} // namespace

State::~State() {
    if (word_font) DeleteObject(word_font);
    if (note_font) DeleteObject(note_font);
    if (glow_dc) DeleteDC(glow_dc);
    if (glow) DeleteObject(glow);
}

ULONGLONG duration_ms() { return total_ms; }

void paint(HDC dc, const RECT& client, HBRUSH background, COLORREF background_colour,
           State& state, ULONGLONG elapsed) {
    FillRect(dc, &client, background);

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    const int centre_x = client.left + width / 2;
    const int centre_y = client.top + height / 2;
    const int em = std::clamp(width * 7 / 100, 52, 92);

    const double fade = progress(elapsed, word_ms + hold_ms, fade_ms);
    if (fade >= 1.0) return;   // the word has left; only the field remains

    const int saved = SaveDC(dc);
    SetBkMode(dc, TRANSPARENT);

    // ---- the glow, behind everything ------------------------------------
    const double grown = glow_curve(progress(elapsed, glow_delay, glow_span));
    if (grown > 0.0 && state.glow_dc) {
        const int full = std::min(width * 135 / 100, glow_limit);
        const int side = static_cast<int>(full * (glow_from + (1.0 - glow_from) * grown));
        if (side > 0) {
            BLENDFUNCTION fn{};
            fn.BlendOp = AC_SRC_OVER;
            // The animation fades the glow up as well as scaling it, so the
            // gradient's own alpha is modulated by the same curve.
            fn.SourceConstantAlpha = static_cast<BYTE>(grown * 255.0 + 0.5);
            fn.AlphaFormat = AC_SRC_ALPHA;
            AlphaBlend(dc, centre_x - side / 2, centre_y - side / 2, side, side,
                       state.glow_dc, 0, 0, state.glow_side, state.glow_side, fn);
        }
    }

    // ---- the word, centred on what is on screen -------------------------
    ensure_word_font(dc, state, em);
    if (state.word_font) {
        SelectObject(dc, state.word_font);
        SetTextCharacterExtra(dc, -MulDiv(em, 2, 100));

        const double fly = fly_curve(progress(elapsed, fly_delay, fly_span));
        const int fly_x = static_cast<int>((1.0 - fly) * width * 0.74);

        // The row is as wide as the sum of EVERY letter's box, each grown to its
        // own progress -- the way the page's flex row widens as its spans do.
        // Taking only the last started letter's width instead counted the ones
        // before it as fully open, so the row jumped by most of a letter every
        // time a new one started and the word slid left in seven jerks. The box
        // grows on the letter's own linear clock, so the shift and the ink agree.
        int rest_width = 0;
        for (int i = 0; i < letters; ++i) {
            const double letter = progress(elapsed, letter_start + i * letter_step, letter_span);
            if (letter <= 0.0) break;
            rest_width += static_cast<int>((state.prefix[i + 1] - state.prefix[i]) * letter);
        }

        const int left = centre_x - (state.first_width + rest_width) / 2 + fly_x;
        const int top = centre_y - state.word_cell / 2;

        SetTextColor(dc, blend(first_ink, background_colour, fade));
        TextOutW(dc, left, top, first_word, static_cast<int>(std::size(first_word)) - 1);

        if (rest_width > 0) {
            const int clipped = SaveDC(dc);
            // The clip is the page's overflow: hidden on a line-height: 1 box, so
            // a letter rising into place is revealed by the box rather than
            // drawn outside the line.
            IntersectClipRect(dc, left + state.first_width, centre_y - em / 2,
                              left + state.first_width + rest_width, centre_y + em / 2);
            for (int i = 0; i < letters; ++i) {
                const double letter = progress(elapsed, letter_start + i * letter_step, letter_span);
                if (letter <= 0.0) break;
                // The letter fades, lifts and scales on this same linear clock,
                // which is also the clock the box above grows on.
                const double alpha = std::min(letter / 0.45, 1.0) * (1.0 - fade);
                if (alpha <= 0.004) continue;
                const double rise = (1.0 - letter) * 0.45 * em;
                const double growth = 0.7 + 0.3 * letter;
                const int x = left + state.first_width + state.prefix[i];
                SetTextColor(dc, blend(background_colour, rest_ink, alpha));

                if (letter >= 1.0) {
                    // A settled letter is drawn straight. Enabling a world
                    // transform takes GDI out of its compatible graphics mode,
                    // and ClearType goes with it: the glyph falls back to
                    // greyscale antialiasing and reads as frayed at the edges.
                    // Settled is what the word is for most of the time it is on
                    // screen, so this is the case that matters.
                    TextOutW(dc, x, top, rest_word + i, 1);
                    continue;
                }

                // Still moving, so it is placed by its own transform, the way the
                // page does it: translateY(0.45em) scale(0.7) at the start. The
                // origin is the letter's own box centre, as transform-origin
                // defaults to - scaling about the left edge instead would walk
                // the letter sideways and pull the row off centre.
                const double box_x = x + (state.prefix[i + 1] - state.prefix[i]) / 2.0;
                const double box_y = top + em / 2.0;
                const int placed = SaveDC(dc);
                SetGraphicsMode(dc, GM_ADVANCED);
                XFORM transform{};
                transform.eM11 = static_cast<FLOAT>(growth);
                transform.eM22 = static_cast<FLOAT>(growth);
                transform.eDx = static_cast<FLOAT>(box_x * (1.0 - growth));
                transform.eDy = static_cast<FLOAT>(box_y * (1.0 - growth) + rise);
                SetWorldTransform(dc, &transform);
                TextOutW(dc, x, top, rest_word + i, 1);
                RestoreDC(dc, placed);
            }
            RestoreDC(dc, clipped);
        }
    }

    // ---- the note, under the word ---------------------------------------
    const double note_in = progress(elapsed, note_delay, note_span);
    if (note_in > 0.0) {
        if (!state.note_font || state.note_em != note_em) {
            make_font(dc, state.note_font, note_em, FW_BOLD);
            state.note_em = note_em;
        }
        if (state.note_font) {
            double alpha = note_in;
            if (elapsed > note_breathe_delay) {
                // The stylesheet breathes it between full and 0.45 on a 2s cycle.
                const double phase =
                    std::fmod(static_cast<double>(elapsed - note_breathe_delay),
                              static_cast<double>(note_breathe_span)) /
                    static_cast<double>(note_breathe_span);
                alpha *= 0.45 + 0.55 * (0.5 + 0.5 * std::cos(phase * 2.0 * pi));
            }
            const double ink = alpha * note_alpha;
            if (ink > 0.004) {
                SelectObject(dc, state.note_font);
                SetTextCharacterExtra(dc, note_tracking);
                SetTextColor(dc, blend(background_colour, note_ink, ink));
                SIZE measured{};
                GetTextExtentPoint32W(dc, note_text,
                                      static_cast<int>(std::size(note_text)) - 1, &measured);
                // The trailing letter-spacing would push the run off centre by
                // half a step; the page compensates with text-indent.
                const int x = centre_x - measured.cx / 2 + note_tracking / 2;
                const int y = centre_y + static_cast<int>(em * note_offset) +
                              static_cast<int>(note_gap);
                TextOutW(dc, x, y, note_text, static_cast<int>(std::size(note_text)) - 1);
                SetTextCharacterExtra(dc, 0);
            }
        }
    }

    RestoreDC(dc, saved);
}

} // namespace ce::cover
