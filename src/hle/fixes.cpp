// Small binary patches to XDK library code that assumes real Xbox hardware details.
#include "hle.h"
#include "../cpu/patch.h"
#include "../video/video.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <windows.h>

static int hud_offset();

static void expect_and_patch(uint32_t addr, const uint8_t *expect, const uint8_t *with, size_t n, const char *what) {
    if (memcmp(xptr<void>(addr), expect, n) != 0) {
        logf("fix '%s': unexpected bytes at %08x, skipped", what, addr);
        return;
    }
    patch_bytes(addr, with, n);
    logf("fix '%s' applied at %08x", what, addr);
}

// D3DDevice_GetVisibilityTestResult (occlusion query readback). The game asks for the result in
// the same frame and spins until the GPU has processed the query, which serializes the game
// thread with the emulated GPU (it spent ~half its time spinning). Real hardware answered in
// microseconds; for us the GPU thread lags by its whole backlog. Once an index has produced a
// result, answer a still-pending query with that index's last result (one frame stale) instead.
// Layout from the original code at 0x228980: entry = [[dev+0x7d4+(Index>>8)*4] + (Index&0xff)*16],
// entry = {u64 timestamp, u32 result, u32 status (-1 = pending)}.
static const uint32_t kD3DDevicePtr = 0x00239198, kKickOff = 0x0022CE60;
static const uint32_t D3DERR_TESTINCOMPLETE = 0x88760828;
struct VisCache {
    uint32_t index;
    uint32_t result;
    bool valid;
    bool served;        // stats: last answer for this index came from the cache
    uint32_t served_value;
};
static volatile long g_vis_real, g_vis_cached, g_vis_mismatch, g_vis_incomplete;
static VisCache g_vis_cache[1024];

extern "C" uint32_t __stdcall hle_GetVisibilityTestResult(uint32_t index, uint32_t *result, uint64_t *timestamp) {
    uint8_t *dev = *xptr<uint8_t *>(kD3DDevicePtr);
    uint8_t *block = *(uint8_t **)(dev + 0x7d4 + (index >> 8) * 4);
    uint32_t *entry = (uint32_t *)(block + (index & 0xff) * 16);
    VisCache &c = g_vis_cache[index & 1023];
    if (entry[3] != 0xFFFFFFFF) {
        *result = entry[2];
        if (timestamp) *timestamp = *(uint64_t *)entry;
        g_vis_real++;
        {
            // debug (XB_VIS_LOG=1): per-index result sequences for a few indices
            static int en = getenv("XB_VIS_LOG") ? 1 : 0;
            static char seq[64][256];
            static int seqn[64];
            static int logged;
            if (en && index < 64 && logged < 64) {
                int &n = seqn[index];
                if (n < 240) n += snprintf(seq[index] + n, sizeof seq[index] - n, "%u%s ", entry[2], (c.valid && c.index == index && c.served) ? "*" : "");
                else {
                    logf("vis seq idx %u: %s", index, seq[index]);
                    n = 0;
                    logged++;
                }
            }
        }
        if (c.valid && c.index == index && c.served && ((c.served_value == 0) != (entry[2] == 0))) g_vis_mismatch++;
        c = {index, entry[2], true, false, 0};
        return 0;
    }
    if (c.valid && c.index == index) {
        // no KickOff here: the push buffer goes out soon anyway, and a kick per polled query
        // (hundreds per frame in busy scenes) fragmented submission and dirty tracking
        *result = c.result;
        if (timestamp) *timestamp = 0;
        c.served = true;
        c.served_value = c.result;
        g_vis_cached++;
        return 0;
    }
    if (entry[0] == *(uint32_t *)(dev + 0x2c)) { // query still in the unsubmitted push buffer: kick it
        typedef void(__thiscall * KickOff_t)(void *, int);
        ((KickOff_t)kKickOff)(dev, 0);
    }
    g_vis_incomplete++;
    return D3DERR_TESTINCOMPLETE;
}

extern "C" void hle_vis_stats(char *buf, int n) {
    snprintf(buf, n, "vis real %ld cached %ld mismatch %ld incomplete %ld", g_vis_real, g_vis_cached, g_vis_mismatch, g_vis_incomplete);
    g_vis_real = g_vis_cached = g_vis_mismatch = g_vis_incomplete = 0;
}

extern "C" int nv2a_occlusion_mode(void);
void hle_fixes_install() {
    static const uint8_t vis_prologue[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x0d, 0x98, 0x91, 0x23, 0x00};
    // Occlusion shortcuts are opt-in (XB_FAST_OCCLUSION=1): with fast MMIO/interrupts exact results
    // cost about nothing, and stale answers can make light effects pop.
    int om = nv2a_occlusion_mode();
    logf("occlusion queries: %s (XB_OCCLUSION=fast|exact|late)", om == 0 ? "fast" : om == 1 ? "exact" : "late");
    if (om != 0) {
    } else if (memcmp(xptr<void>(0x00228980), vis_prologue, sizeof vis_prologue) == 0) HOOK(0x00228980, hle_GetVisibilityTestResult);
    else logf("fix 'GetVisibilityTestResult': unexpected bytes, skipped");

    // Lock2DSurface: D3DLOCK_TILED returns a pointer into the NV2A tiled aperture at
    // 0xF0000000 (an alias of physical RAM). We only map RAM at 0x80000000, so keep the
    // regular (linear) pointer - the same memory, as xemu treats the aperture.
    static const uint8_t or_f0[] = {0x81, 0xC9, 0x00, 0x00, 0x00, 0xF0}; // or ecx, 0F0000000h
    static const uint8_t nop6[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
    expect_and_patch(0x0022F4E8, or_f0, nop6, sizeof or_f0, "Lock2DSurface tiled aperture");

    // Widescreen UI. The engine keeps the widescreen flag at 0x364af5 behind a getter at 0x3e4f0.
    // The 3D camera (0x15cc0) uses it for a 16:9 projection; everything else that reads it squeezes
    // a hand-picked set of 2D elements by 0.75: the script native IsWideScreen (0x21a90 -> UILib's
    // WIDTH_RATIO and a controller-guide layout) and four in-game HUD spots. The GPU emulation now
    // squeezes all 2D instead (nv2a_set_ui_squash), so those would be squeezed twice: make the getter
    // say "4:3" and give the camera an inline read of the real flag (same 5 bytes as its call).
    if (g_video.aspect > 1.5f && !g_video.stretch_ui) {
        static const uint8_t getter[] = {0xA0, 0xF5, 0x4A, 0x36, 0x00, 0xC3}; // mov al, [0x364af5]; ret
        static const uint8_t ret0[] = {0x31, 0xC0, 0xC3, 0xCC, 0xCC, 0xCC};   // xor eax, eax; ret
        static const uint8_t cam_call[] = {0xE8, 0x2B, 0x88, 0x02, 0x00};     // call 0x3e4f0
        static const uint8_t cam_read[] = {0xA0, 0xF5, 0x4A, 0x36, 0x00};     // mov al, [0x364af5]
        expect_and_patch(0x00015CC0, cam_call, cam_read, sizeof cam_call, "widescreen camera keeps flag");
        expect_and_patch(0x0003E4F0, getter, ret0, sizeof getter, "widescreen 2D tweaks off");

        // Engine-drawn radar (0xa7320, frame and blips): center x is the .rdata float at 0x312928,
        // read only there. Anchor it to the right edge like the script HUD (see hud_offset()).
        float old_x = 680.0f, new_x = 680.0f + (float)hud_offset();
        expect_and_patch(0x00312928, (const uint8_t *)&old_x, (const uint8_t *)&new_x, 4, "widescreen radar at right edge");
    }
}

// ---------------------------------------------------------------------------------------------
// Widescreen HUD layout. UI coordinates are 800x600 and the GPU squashes 2D to 4:3 around the
// center, which would bunch the HUD in the middle. HUD elements that sit near a side are moved
// by +-hud_offset() so they keep their distance to the 16:9 screen edge: squashed x =
// 400 + (x - 400) * s, and keeping 0.75-scaled margins needs x' = x -+ 400 * (1/s - 1).
static int hud_offset() {
    float s = (4.0f / 3.0f) / g_video.aspect;
    return (int)(400.0f * (1.0f / s - 1.0f) + 0.5f); // 133 for 16:9
}

struct TextPatch {
    const char *find, *replace;
    int count; // exact number of occurrences expected; otherwise the file is left unpatched
};
struct FilePatch {
    const char *file; // path below the game directory, lower case
    std::vector<TextPatch> patches;
};

// HUDL / HUDR (script globals added to UILib.cs) shift left- / right-anchored HUD elements.
static const std::vector<FilePatch> &widescreen_script_patches() {
    static const std::vector<FilePatch> p = {
        {"\\script\\ui\\uilib.cs", {
            {"float WIDTH_RATIO = 1.0;", "float WIDTH_RATIO = 1.0; int HUDL = -@; int HUDR = @;", 1},
        }},
        {"\\script\\ui\\uihud.cs", {
            // status bar (hp / rage / stamina) group; its AbsCoord() children are absolute
            {"Begin(uiStatusBar=NewUI());", "Begin(uiStatusBar=NewUI()); Position(HUDL, 0);", 1},
            {"Position(90, 65); Size(125, 6); AbsCoord();", "Position(90+HUDL, 65); Size(125, 6); AbsCoord();", 2},
            {"Position(88, 81); Size(125, 6); AbsCoord();", "Position(88+HUDL, 81); Size(125, 6); AbsCoord();", 2},
            {"Position(86, 98); Size(125, 6); AbsCoord();", "Position(86+HUDL, 98); Size(125, 6); AbsCoord();", 2},
            {"x1=250; y1=90; x2=220; y2=65;", "x1=250+HUDL; y1=90; x2=220+HUDL; y2=65;", 1}, // hit combo
            {"Position(140, 105); Size(40, 40);", "Position(140+HUDL, 105); Size(40, 40);", 1},   // phoenix stone
            {"Position(80, 105); Size(40, 40);", "Position(80+HUDL, 105); Size(40, 40);", 1},     // mission item
            {"Position(0, 450); Size(600, 30);", "Position(HUDL, 450); Size(600, 30);", 1},       // hint band
            {"Position(0, 125); Size(400, 25);", "Position(HUDL, 125); Size(400, 25);", 1},       // say band
            {"TweenPosition(-400, GetY(), 0, GetY(), 0.3)", "TweenPosition(-400+HUDL, GetY(), HUDL, GetY(), 0.3)", 1},
            {"TweenPosition(0, GetY(), -400, GetY(), 0.3)", "TweenPosition(HUDL, GetY(), -400+HUDL, GetY(), 0.3)", 1},
            {"Position(0, 100); Size(400, 25);", "Position(HUDL, 100); Size(400, 25);", 1},       // kill messages
            // button / item cluster (and the chat icon) bottom right
            {"iconX[0]=645;", "iconX[0]=645+HUDR;", 1},
            {"iconX[1]=680;", "iconX[1]=680+HUDR;", 1},
            {"iconX[2]=680;", "iconX[2]=680+HUDR;", 1},
            {"iconX[3]=715;", "iconX[3]=715+HUDR;", 1},
        }},
        // Big scroll (style 7: round/game over, server list) is 840 wide and its ink row bars ("brush2")
        // are exactly 800: both span the whole 4:3 screen, so the GPU's full-width rule would stretch
        // them to 16:9 while the rollers stay in 4:3. Keep them just inside the screen (4..796) so they
        // stay squashed with the rollers; the bar ends remain hidden under the rollers.
        {"\\script\\ui\\uiscroll.cs", {
            {"g_scw = 840;", "g_scw = 792;", 1},
        }},
        {"\\script\\ui\\ui2.cs", {
            {"Position(0, 120); Size(800, 20);", "Position(4, 120); Size(792, 20);", 1},
            {"Position(00, 145+i*25); Size(800, 20);", "Position(4, 145+i*25); Size(792, 20);", 1},
        }},
        {"\\script\\ui\\ui.cs", {
            {"Position(0, 155+i*25); Size(800, 25);", "Position(4, 155+i*25); Size(792, 25);", 1},
            {"Position(0,130); Size(800, 20); Image(\"brush2\")", "Position(4,130); Size(792, 20); Image(\"brush2\")", 1},
            {"Position(0,490); Size(800, 15); Image(\"brush2\")", "Position(4,490); Size(792, 15); Image(\"brush2\")", 1},
        }},
    };
    return p;
}

static int count_occurrences(const std::string &s, const std::string &what) {
    int n = 0;
    for (size_t pos = s.find(what); pos != std::string::npos; pos = s.find(what, pos + what.size())) n++;
    return n;
}

// Called by NtCreateFile for files on the game disc: returns the host file to open instead.
std::string hle_file_override(const std::string &xbox_path, const std::string &host_path) {
    if (!(g_video.aspect > 1.5f && !g_video.stretch_ui)) return host_path;
    std::string lp = xbox_path.substr(14); // after \Device\CdRom0
    for (char &c : lp) c = (char)tolower((unsigned char)c);
    for (const FilePatch &fp : widescreen_script_patches()) {
        if (lp != fp.file) continue;
        FILE *in = fopen(host_path.c_str(), "rb");
        if (!in) return host_path;
        std::string text;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0) text.append(buf, n);
        fclose(in);
        std::string off = std::to_string(hud_offset());
        for (const TextPatch &tp : fp.patches) {
            if (count_occurrences(text, tp.find) != tp.count) {
                logf("script patch: %s: '%s' found %d times, expected %d - file left unpatched", fp.file, tp.find,
                     count_occurrences(text, tp.find), tp.count);
                return host_path;
            }
            std::string rep = tp.replace;
            for (size_t pos; (pos = rep.find('@')) != std::string::npos;) rep.replace(pos, 1, off);
            for (size_t pos = text.find(tp.find); pos != std::string::npos; pos = text.find(tp.find, pos + rep.size()))
                text.replace(pos, strlen(tp.find), rep);
        }
        std::string dir = std::string(g_save_dir) + "\\patched";
        CreateDirectoryA(dir.c_str(), nullptr);
        std::string out_path = dir + "\\" + host_path.substr(host_path.find_last_of("\\/") + 1);
        FILE *out = fopen(out_path.c_str(), "wb");
        if (!out) return host_path;
        fwrite(text.data(), 1, text.size(), out);
        fclose(out);
        logf("script patch: %s -> %s (%zu patches)", fp.file, out_path.c_str(), fp.patches.size());
        return out_path;
    }
    return host_path;
}
