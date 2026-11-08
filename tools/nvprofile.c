/* nvprofile: manage an NVIDIA driver application profile for ironphoenix.exe (NVAPI DRS).
 *   nvprofile get            show the OpenGL "Threaded optimization" value for ironphoenix.exe
 *   nvprofile set on|off     create/update profile "Iron Phoenix" with Threaded optimization on/off
 *   nvprofile delete         remove that profile again
 * Build (32-bit, matches the 32-bit nvapi.dll in SysWOW64):
 *   i686-w64-mingw32-clang -O2 tools/nvprofile.c -o build/nvprofile.exe */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef unsigned int NvU32;
typedef unsigned short NvU16;
typedef unsigned char NvU8;
typedef int NvAPI_Status;
typedef void *NvDRSSessionHandle;
typedef void *NvDRSProfileHandle;
typedef NvU16 NvAPI_UnicodeString[2048];

typedef struct {
    NvU32 version;
    NvAPI_UnicodeString profileName;
    NvU32 gpuSupport;
    NvU32 isPredefined;
    NvU32 numOfApps;
    NvU32 numOfSettings;
} NVDRS_PROFILE;

typedef struct {
    NvU32 version;
    NvU32 isPredefined;
    NvAPI_UnicodeString appName;
    NvAPI_UnicodeString userFriendlyName;
    NvAPI_UnicodeString launcher;
} NVDRS_APPLICATION_V1;

typedef struct {
    NvU32 valueLength;
    NvU8 valueData[4096];
} NVDRS_BINARY_SETTING;

typedef struct {
    NvU32 version;
    NvAPI_UnicodeString settingName;
    NvU32 settingId;
    NvU32 settingType;
    NvU32 settingLocation;
    NvU32 isCurrentPredefined;
    NvU32 isPredefinedValid;
    union { NvU32 u32PredefinedValue; NVDRS_BINARY_SETTING binaryPredefinedValue; NvAPI_UnicodeString wszPredefinedValue; };
    union { NvU32 u32CurrentValue; NVDRS_BINARY_SETTING binaryCurrentValue; NvAPI_UnicodeString wszCurrentValue; };
} NVDRS_SETTING;

#define VER(s, v) ((NvU32)sizeof(s) | ((v) << 16))
#define OGL_THREAD_CONTROL_ID 0x20C1221E
#define OGL_THREAD_CONTROL_ENABLE 1
#define OGL_THREAD_CONTROL_DISABLE 2
#define NVAPI_PROFILE_NOT_FOUND (-163)
#define NVAPI_EXECUTABLE_NOT_FOUND (-166)
#define NVAPI_SETTING_NOT_FOUND (-160)

typedef void *(__cdecl *QI_t)(NvU32);
static QI_t qi;
#define FN(name, id, ...) typedef NvAPI_Status(__cdecl *name##_t)(__VA_ARGS__); static name##_t name; static const NvU32 name##_id = id;
FN(NvAPI_Initialize, 0x0150E828, void)
FN(NvAPI_DRS_CreateSession, 0x0694D52E, NvDRSSessionHandle *)
FN(NvAPI_DRS_DestroySession, 0xDAD9CFF8, NvDRSSessionHandle)
FN(NvAPI_DRS_LoadSettings, 0x375DBD6B, NvDRSSessionHandle)
FN(NvAPI_DRS_SaveSettings, 0xFCBC7E14, NvDRSSessionHandle)
FN(NvAPI_DRS_CreateProfile, 0xCC176068, NvDRSSessionHandle, NVDRS_PROFILE *, NvDRSProfileHandle *)
FN(NvAPI_DRS_DeleteProfile, 0x17093206, NvDRSSessionHandle, NvDRSProfileHandle)
FN(NvAPI_DRS_FindProfileByName, 0x7E4A9A0B, NvDRSSessionHandle, NvAPI_UnicodeString, NvDRSProfileHandle *)
FN(NvAPI_DRS_CreateApplication, 0x4347A9DE, NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_APPLICATION_V1 *)
FN(NvAPI_DRS_FindApplicationByName, 0xEEE566B2, NvDRSSessionHandle, NvAPI_UnicodeString, NvDRSProfileHandle *, NVDRS_APPLICATION_V1 *)
FN(NvAPI_DRS_SetSetting, 0x577DD202, NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING *)
FN(NvAPI_DRS_GetSetting, 0x73BF8338, NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NVDRS_SETTING *)
#define LOAD(name) if (!(name = (name##_t)qi(name##_id))) { fprintf(stderr, "missing " #name "\n"); return 1; }
#define CHECK(x) do { NvAPI_Status st_ = (x); if (st_) { fprintf(stderr, "%s failed: %d\n", #x, st_); return 1; } } while (0)

static void ustr(NvAPI_UnicodeString d, const wchar_t *s) { memset(d, 0, sizeof(NvAPI_UnicodeString)); wcsncpy((wchar_t *)d, s, 2047); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: nvprofile get | set on|off | delete\n"); return 2; }
    HMODULE m = LoadLibraryA("nvapi.dll");
    if (!m || !(qi = (QI_t)GetProcAddress(m, "nvapi_QueryInterface"))) { fprintf(stderr, "nvapi.dll not available\n"); return 1; }
    LOAD(NvAPI_Initialize) LOAD(NvAPI_DRS_CreateSession) LOAD(NvAPI_DRS_DestroySession) LOAD(NvAPI_DRS_LoadSettings)
    LOAD(NvAPI_DRS_SaveSettings) LOAD(NvAPI_DRS_CreateProfile) LOAD(NvAPI_DRS_DeleteProfile) LOAD(NvAPI_DRS_FindProfileByName)
    LOAD(NvAPI_DRS_CreateApplication) LOAD(NvAPI_DRS_FindApplicationByName) LOAD(NvAPI_DRS_SetSetting) LOAD(NvAPI_DRS_GetSetting)
    CHECK(NvAPI_Initialize());
    NvDRSSessionHandle s;
    CHECK(NvAPI_DRS_CreateSession(&s));
    CHECK(NvAPI_DRS_LoadSettings(s));

    static NvAPI_UnicodeString pname, aname;
    ustr(pname, L"Iron Phoenix");
    ustr(aname, L"ironphoenix.exe");
    NvDRSProfileHandle p = NULL;
    static NVDRS_APPLICATION_V1 app;
    app.version = VER(NVDRS_APPLICATION_V1, 1);
    NvAPI_Status st = NvAPI_DRS_FindApplicationByName(s, aname, &p, &app);

    if (!strcmp(argv[1], "get")) {
        if (st) { printf("no profile contains ironphoenix.exe (%d): driver default applies\n", st); return 0; }
        static NVDRS_SETTING set;
        set.version = VER(NVDRS_SETTING, 1);
        st = NvAPI_DRS_GetSetting(s, p, OGL_THREAD_CONTROL_ID, &set);
        if (st) printf("ironphoenix.exe profile found; threaded optimization not set (%d): auto\n", st);
        else printf("threaded optimization = %u (1 on, 2 off)%s\n", set.u32CurrentValue, set.isCurrentPredefined ? " [predefined]" : "");
        return 0;
    }
    if (!strcmp(argv[1], "set") && argc > 2) {
        if (st) {
            if (NvAPI_DRS_FindProfileByName(s, pname, &p)) {
                static NVDRS_PROFILE prof;
                prof.version = VER(NVDRS_PROFILE, 1);
                ustr(prof.profileName, L"Iron Phoenix");
                CHECK(NvAPI_DRS_CreateProfile(s, &prof, &p));
            }
            memset(&app, 0, sizeof app);
            app.version = VER(NVDRS_APPLICATION_V1, 1);
            ustr(app.appName, L"ironphoenix.exe");
            ustr(app.userFriendlyName, L"Iron Phoenix");
            CHECK(NvAPI_DRS_CreateApplication(s, p, &app));
        }
        static NVDRS_SETTING set;
        memset(&set, 0, sizeof set);
        set.version = VER(NVDRS_SETTING, 1);
        set.settingId = OGL_THREAD_CONTROL_ID;
        set.settingType = 0; /* NVDRS_DWORD_TYPE */
        set.u32CurrentValue = !strcmp(argv[2], "on") ? OGL_THREAD_CONTROL_ENABLE : OGL_THREAD_CONTROL_DISABLE;
        CHECK(NvAPI_DRS_SetSetting(s, p, &set));
        CHECK(NvAPI_DRS_SaveSettings(s));
        printf("threaded optimization for ironphoenix.exe set to %s\n", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "delete")) {
        if (NvAPI_DRS_FindProfileByName(s, pname, &p)) { printf("no \"Iron Phoenix\" profile\n"); return 0; }
        CHECK(NvAPI_DRS_DeleteProfile(s, p));
        CHECK(NvAPI_DRS_SaveSettings(s));
        printf("profile deleted\n");
        return 0;
    }
    fprintf(stderr, "usage: nvprofile get | set on|off | delete\n");
    return 2;
}
