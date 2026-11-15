#pragma once
#include <stdint.h>
#include <cmath>
#include <algorithm>

enum class DesktopContext { Startup, Menu, Gameplay };

inline DesktopContext desktop_context(bool ready, int app, int ui, int pending, int top) {
    if (!ready || app == 0 || ui == 1) return DesktopContext::Startup;
    auto playing = [](int value) { return value == 0 || value == 100 || value == 110; };
    return app == 50 && playing(ui) && playing(pending) && top == 0
        ? DesktopContext::Gameplay : DesktopContext::Menu;
}

// Holding a key while its meaning changes must not create another action.
struct DesktopContextKey {
    bool down = false, blocked = false;
    DesktopContext started = DesktopContext::Startup;
    void disarm() { down = false; blocked = true; }
    bool update(bool pressed, DesktopContext context) {
        if (!pressed) { down = blocked = false; return false; }
        if (!down) { down = true; started = context; }
        if (started != context) blocked = true;
        return !blocked;
    }
};

struct DesktopMouseLook {
    double dx = 0, dy = 0;
    uint32_t sampled_at = 0, moved_at = 0;
    int16_t x = 0, y = 0;
    bool active = false;
    void reset(uint32_t now, bool capture) {
        dx = dy = 0; x = y = 0; sampled_at = moved_at = now; active = capture;
    }
    void move(int mx, int my, uint32_t now) {
        if (!active || (!mx && !my)) return;
        dx = std::clamp(dx + mx, -100000.0, 100000.0);
        dy = std::clamp(dy + my, -100000.0, 100000.0);
        moved_at = now;
    }
    void sample(uint32_t now, double sensitivity) {
        if (!active || (uint32_t)(now - moved_at) >= 32) { x = y = 0; dx = dy = 0; sampled_at = now; return; }
        uint32_t elapsed = now - sampled_at;
        if (elapsed < 8) return; // Repeated XInput polls see the same sample.
        double vx = dx / elapsed, vy = -dy / elapsed;
        double length = std::hypot(vx, vy);
        if (!length) return; // Hold across extra polls until the idle timeout.
        dx = dy = 0; sampled_at = now;
        // The game consumes a stick, not raw angles. Compensate the stick's
        // dead zone, then scale mouse velocity; preserve diagonal direction.
        double magnitude = 9000 + 23767 * std::min(1.0, length * sensitivity / 2.0);
        x = (int16_t)std::clamp(std::lround(vx / length * magnitude), -32767L, 32767L);
        y = (int16_t)std::clamp(std::lround(vy / length * magnitude), -32767L, 32767L);
    }
};
