#include "hotkey.hpp"
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char* scenario) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", scenario);
        std::exit(1);
    }
}
}

int main() {
    ce::HotkeyState key;
    check(!key.sample(true, true), "initial held key does not toggle");
    check(!key.sample(true, true), "held key remains disarmed");
    check(!key.sample(false, true), "eligible release arms");
    check(key.sample(true, true), "first eligible press toggles");
    check(!key.sample(true, true), "repeat samples do not toggle");
    check(!key.sample(true, true), "long hold does not toggle");
    check(!key.sample(false, true), "release rearms while UI hidden too");
    check(key.sample(true, true), "second press toggles same as first");

    check(!key.sample(false, true), "release before background");
    check(!key.sample(false, false), "background release disarms");
    check(!key.sample(true, false), "background press does not toggle");
    check(!key.sample(true, true), "foreground return while held does not toggle");
    check(!key.sample(false, true), "foreground release rearms");
    check(key.sample(true, true), "fresh foreground press toggles");

    check(!key.sample(false, true), "armed before modal");
    key.disarm();
    check(!key.sample(true, true), "modal enter/exit between samples stays disarmed");
    check(!key.sample(false, false), "modal release not eligible");
    check(!key.sample(true, false), "modal press suppressed");
    check(!key.sample(true, true), "modal close while held suppressed");
    check(!key.sample(false, true), "release after modal arms");
    check(key.sample(true, true), "fresh post-modal press toggles");

    check(!key.sample(false, true), "armed before stop");
    check(!key.sample(true, false), "stop suppresses and disarms");
    check(!key.sample(false, false), "stop release never arms");
    check(!key.sample(true, false), "stop remains suppressed");
    std::puts("Hotkey FSM tests passed (no desktop input).");
}
