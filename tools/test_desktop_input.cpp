#include "../src/hle/desktop_input.h"
#include <cassert>
#include <cstdio>

int main() {
    using C = DesktopContext;
    assert(desktop_context(false, 50, 100, 100, 0) == C::Startup);
    assert(desktop_context(true, 1, 2, 2, 0) == C::Menu);
    assert(desktop_context(true, 50, 100, 110, 0) == C::Gameplay);
    assert(desktop_context(true, 50, 0, 0, 0) == C::Gameplay);
    assert(desktop_context(true, 50, 100, 150, 0) == C::Menu); // pending pause
    assert(desktop_context(true, 50, 150, 150, 0) == C::Menu);
    assert(desktop_context(true, 50, 160, 160, 0) == C::Menu); // settings
    assert(desktop_context(true, 50, 100, 100, 201) == C::Menu); // controller dialog
    assert(desktop_context(true, 51, 100, 100, 0) == C::Menu); // game over
    DesktopContextKey key;
    assert(key.update(true, C::Gameplay));
    assert(!key.update(true, C::Menu)); // Esc must not become B while held.
    assert(!key.update(true, C::Gameplay)); // Nor reopen pause after returning.
    assert(!key.update(false, C::Menu));
    assert(key.update(true, C::Menu));
    key.disarm();
    assert(!key.update(true, C::Menu)); // Held across focus gain.
    assert(!key.update(false, C::Menu));
    assert(key.update(true, C::Menu));

    DesktopMouseLook mouse;
    mouse.reset(100, true);
    mouse.move(8, -4, 108);
    mouse.sample(108, 1);
    int x = mouse.x, y = mouse.y;
    assert(x > 9000 && y > 0 && x > y);
    mouse.sample(109, 1); assert(mouse.x == x && mouse.y == y);
    mouse.sample(116, 1); assert(mouse.x == x && mouse.y == y);
    mouse.move(0, 0, 130); // Button packets must not keep the camera moving.
    mouse.sample(140, 1); assert(mouse.x == 0 && mouse.y == 0);
    mouse.move(100000, 100000, 148); mouse.sample(148, 10);
    assert(mouse.x > 0 && mouse.y < 0);
    assert(std::abs(mouse.x) < 32767 && std::abs(mouse.y) < 32767); // radial clamp
    mouse.reset(150, false);
    mouse.move(100, 100, 160); mouse.sample(160, 1);
    assert(mouse.x == 0 && mouse.y == 0);
    // Equal physical movement over equal sample periods gives equal output,
    // regardless of the number of game input polls between packets.
    DesktopMouseLook a, b;
    a.reset(200, true); b.reset(200, true);
    a.move(16, 0, 216); a.sample(216, 1);
    for (unsigned t=201; t<216; ++t) b.sample(t, 1);
    b.move(16, 0, 216); b.sample(216, 1);
    assert(a.x == b.x && a.y == b.y);
    a.reset(UINT32_MAX-8, true); a.move(4, 0, 1); a.sample(1, 1);
    assert(a.x > 0); // tick wrap
    puts("Desktop input context, focus latch and mouse-look tests passed");
}
