// Throwaway probe: render the startup cover frame by frame and measure where
// the word actually is, so a reported stutter can be seen as numbers instead of
// argued about. cover.cpp is self-contained, so including it is all the linking
// this needs.
//
// Prints, per frame: the elapsed time, the word's left and right edges and its
// centre, the
// pixel delta since the previous frame, and the same delta scaled to a 60Hz
// frame. A flat run followed by a jump, or a repeated-then-skipped delta, is
// what "not smooth" looks like here.
//
// Build:  tools\compile_probe.cmd cover_probe
// Run:    build\x64\cover_probe.exe [step_ms] [from_ms] [to_ms]
#include "../src/ui/cover.cpp"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr int kWidth = 900;
constexpr int kHeight = 640;

// The word is the only thing on screen that is near-white; the glow tops out at
// about a third of its colour and the note is dim. Anything brighter than this
// is word.
constexpr int kInk = 150;

struct Frame {
    int left = -1;
    int right = -1;
};

Frame measure(const uint32_t* pixels, int width, int height, int centre_y) {
    Frame frame;
    const int top = centre_y - 70;
    const int bottom = centre_y + 70;
    for (int y = top; y < bottom && y < height; ++y) {
        if (y < 0) continue;
        for (int x = 0; x < width; ++x) {
            const uint32_t px = pixels[static_cast<size_t>(y) * width + x];
            const int r = (px >> 16) & 0xff;
            const int g = (px >> 8) & 0xff;
            const int b = px & 0xff;
            if (r < kInk || g < kInk || b < kInk) continue;
            if (frame.left < 0 || x < frame.left) frame.left = x;
            if (x > frame.right) frame.right = x;
        }
    }
    return frame;
}

} // namespace

int main(int argc, char** argv) {
    const double step = argc > 1 ? std::atof(argv[1]) : 16.7;
    const double from = argc > 2 ? std::atof(argv[2]) : 0.0;
    const double to = argc > 3 ? std::atof(argv[3]) : 2200.0;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = kWidth;
    info.bmiHeader.biHeight = -kHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* raw = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (!bitmap || !raw) {
        std::fprintf(stderr, "no dib\n");
        return 1;
    }
    SelectObject(dc, bitmap);

    HBRUSH background = CreateSolidBrush(RGB(11, 13, 16));
    ce::cover::State state;
    const RECT area{0, 0, kWidth, kHeight};

    std::printf("elapsed   left  right  centre  delta  px/60Hz   paint_us\n");
    int previous = -1;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    for (double elapsed = from; elapsed <= to; elapsed += step) {
        // Clear: paint() fills the field itself, but the DIB still holds the
        // previous frame's glow beyond it.
        PatBlt(dc, 0, 0, kWidth, kHeight, BLACKNESS);
        // How long the frame took to draw. A spike that lands on one particular
        // moment of the sequence is a hitch the position trace cannot show: the
        // word is where it should be, but the frame arrived late.
        LARGE_INTEGER paint_start{};
        QueryPerformanceCounter(&paint_start);
        ce::cover::paint(dc, area, background, RGB(11, 13, 16), state, static_cast<ULONGLONG>(elapsed));
        LARGE_INTEGER paint_end{};
        QueryPerformanceCounter(&paint_end);
        const double paint_ms = 1000.0 * static_cast<double>(paint_end.QuadPart - paint_start.QuadPart) /
                                static_cast<double>(frequency.QuadPart);
        const Frame frame = measure(static_cast<const uint32_t*>(raw), kWidth, kHeight, kHeight / 2);
        if (frame.left < 0) {
            std::printf("%7.1f   ---- (no word)                              %8.3f\n", elapsed, paint_ms);
            continue;
        }
        const double centre = (frame.left + frame.right) / 2.0;
        const double delta = previous < 0 ? 0.0 : centre - previous;
        std::printf("%7.1f  %5d  %5d  %6.1f  %+6.1f  %+6.1f   %8.3f\n",
                    elapsed, frame.left, frame.right, centre, delta, delta * 60.0 * step / 1000.0,
                    paint_ms);
        previous = static_cast<int>(centre);
    }

    DeleteObject(background);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return 0;
}
