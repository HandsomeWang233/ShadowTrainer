#pragma once

namespace ce {
// Platform-independent edge detector. Ineligible samples (background, modal,
// stopping) disarm; a release must be observed while eligible before each press.
class HotkeyState {
public:
    bool sample(bool down, bool eligible) noexcept {
        if (!eligible) {
            armed_ = false;
            return false;
        }
        if (!down) {
            armed_ = true;
            return false;
        }
        if (!armed_) return false;
        armed_ = false;
        return true;
    }
    void disarm() noexcept { armed_ = false; }
private:
    bool armed_ = false;
};
}
