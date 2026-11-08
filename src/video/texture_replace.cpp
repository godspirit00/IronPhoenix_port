// Texture replacement and dumping for the GPU emulation (called from nv2a/pgraph/gl/texture.c).
//
// A texture is identified by the 64-bit hash xemu already computes over its data (and palette).
//   XB_TEX_DUMP=1        writes every new 2D texture to <exe dir>\textures_dump\<hash>.png
//   <exe dir>\textures\<hash>.png|.jpg   replaces that texture (any size or aspect).
// A replacement is resized to an exact multiple of the original size, so it can be sampled
// like the original (linear textures use texel coordinates scaled by one factor, texScale);
// an image with a different aspect (a 16:9 menu background) is squeezed in texture space and
// shows at its own aspect when the game draws it full screen.
#include <windows.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../common.h"

namespace {

std::string exe_dir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    return s.substr(0, s.find_last_of("\\/"));
}

IWICImagingFactory *wic() {
    static IWICImagingFactory *f = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory *p = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p));
        return p;
    }();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // per calling thread; harmless if already initialized
    return f;
}

std::wstring widen(const std::string &s) {
    std::wstring w(s.size() + 1, L'\0');
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], (int)w.size());
    w.resize(n > 0 ? n - 1 : 0);
    return w;
}

// Decode an image file to RGBA8, resized to tw x th.
bool load_rgba(const std::string &path, int tw, int th, std::vector<uint8_t> &out, int &sw, int &sh) {
    IWICImagingFactory *f = wic();
    if (!f) return false;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    IWICBitmapScaler *scaler = nullptr;
    bool ok = false;
    UINT w = 0, h = 0;
    if (SUCCEEDED(f->CreateDecoderFromFilename(widen(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(conv, (UINT)tw, (UINT)th, WICBitmapInterpolationModeFant))) {
        out.resize((size_t)tw * th * 4);
        ok = SUCCEEDED(scaler->CopyPixels(nullptr, (UINT)tw * 4, (UINT)out.size(), out.data()));
        for (size_t p = 0; ok && p < out.size(); p += 4) std::swap(out[p], out[p + 2]); // BGRA -> RGBA
    }
    sw = (int)w;
    sh = (int)h;
    if (scaler) scaler->Release();
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return ok;
}

bool image_size(const std::string &path, int &w, int &h) {
    IWICImagingFactory *f = wic();
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    UINT uw = 0, uh = 0;
    bool ok = f && SUCCEEDED(f->CreateDecoderFromFilename(widen(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&uw, &uh));
    if (frame) frame->Release();
    if (dec) dec->Release();
    w = (int)uw;
    h = (int)uh;
    return ok;
}

bool save_png(const std::string &path, int w, int h, const uint8_t *rgba) {
    IWICImagingFactory *f = wic();
    if (!f) return false;
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    bool ok = false;
    std::vector<uint8_t> bgra(rgba, rgba + (size_t)w * h * 4); // the PNG encoder takes BGRA
    for (size_t p = 0; p < bgra.size(); p += 4) std::swap(bgra[p], bgra[p + 2]);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(f->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(widen(path).c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize((UINT)w, (UINT)h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(frame->WritePixels((UINT)h, (UINT)w * 4, (UINT)bgra.size(), bgra.data())) && SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(enc->Commit())) {
        ok = true;
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    return ok;
}

void flip_rows(uint8_t *rgba, int w, int h) {
    std::vector<uint8_t> tmp((size_t)w * 4);
    for (int y = 0; y < h / 2; y++) {
        uint8_t *a = rgba + (size_t)y * w * 4, *b = rgba + (size_t)(h - 1 - y) * w * 4;
        memcpy(tmp.data(), a, tmp.size());
        memcpy(a, b, tmp.size());
        memcpy(b, tmp.data(), tmp.size());
    }
}

std::string hash_name(uint64_t hash) {
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)hash);
    return b;
}

struct Replacement {
    std::string path;
    bool loaded = false, failed = false;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0, scale = 1;
};

std::mutex g_lock;
std::map<uint64_t, Replacement> g_repl;
std::set<uint64_t> g_dumped;

void scan_replacements() {
    static bool scanned = false;
    if (scanned) return;
    scanned = true;
    std::string dir = exe_dir() + "\\textures\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName;
        size_t dot = n.find_last_of('.');
        if (dot != 16) continue; // <16 hex digits>.<ext>
        uint64_t hash = strtoull(n.substr(0, 16).c_str(), nullptr, 16);
        g_repl[hash].path = dir + n;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    logf("texture replacement: %zu file(s) in %stextures", g_repl.size(), exe_dir().c_str());
}

} // namespace

// Replacement pixels for a texture, resized to (orig_w * scale) x (orig_h * scale). Returns 0 if none.
extern "C" int texrep_lookup(uint64_t hash, int orig_w, int orig_h, const uint8_t **rgba, int *w, int *h, int *scale) {
    std::lock_guard<std::mutex> g(g_lock);
    scan_replacements();
    auto it = g_repl.find(hash);
    if (it == g_repl.end() || orig_w <= 0 || orig_h <= 0) return 0;
    Replacement &r = it->second;
    if (!r.loaded && !r.failed) {
        int iw, ih;
        if (image_size(r.path, iw, ih)) {
            // smallest integer multiple of the original that keeps the image's resolution (max 8x)
            int k = (ih + orig_h - 1) / orig_h;
            int kw = (iw + orig_w - 1) / orig_w;
            if (kw > k) k = kw;
            if (k < 1) k = 1;
            if (k > 8) k = 8;
            int sw, sh;
            if (load_rgba(r.path, orig_w * k, orig_h * k, r.rgba, sw, sh)) {
                flip_rows(r.rgba.data(), orig_w * k, orig_h * k); // files are upright, see texrep_dump
                r.w = orig_w * k;
                r.h = orig_h * k;
                r.scale = k;
                r.loaded = true;
                logf("texture replacement: %016llx (%dx%d) <- %s (%dx%d, uploaded at %dx%d)", (unsigned long long)hash, orig_w, orig_h,
                     r.path.c_str(), sw, sh, r.w, r.h);
            }
        }
        if (!r.loaded) {
            r.failed = true;
            logf("texture replacement: could not load %s", r.path.c_str());
        }
    }
    if (!r.loaded) return 0;
    *rgba = r.rgba.data();
    *w = r.w;
    *h = r.h;
    *scale = r.scale;
    return 1;
}

extern "C" int texrep_dump_enabled(void) {
    static int en = getenv("XB_TEX_DUMP") ? 1 : 0;
    return en;
}

// Should this texture be dumped (first time it is seen)?
extern "C" int texrep_want_dump(uint64_t hash) {
    if (!texrep_dump_enabled()) return 0;
    std::lock_guard<std::mutex> g(g_lock);
    return g_dumped.insert(hash).second ? 1 : 0;
}

extern "C" void texrep_dump(uint64_t hash, int w, int h, const uint8_t *rgba, unsigned color_format, int linear) {
    static std::string dir = [] {
        std::string d = exe_dir() + "\\textures_dump";
        CreateDirectoryA(d.c_str(), nullptr);
        return d;
    }();
    std::string path = dir + "\\" + hash_name(hash) + ".png";
    // The game stores its UI images bottom-up; save them upright (texrep_lookup flips back), so a
    // dump can be edited as an ordinary picture and saved to textures\ unchanged.
    std::vector<uint8_t> img(rgba, rgba + (size_t)w * h * 4);
    flip_rows(img.data(), w, h);
    if (save_png(path, w, h, img.data()))
        logf("texture dump: %s %dx%d format 0x%02x%s", hash_name(hash).c_str(), w, h, color_format, linear ? " linear" : "");
}
