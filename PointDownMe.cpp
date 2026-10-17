// PointDownMe: when the player dies, nearby enemies point down at the corpse with the player's "Point Downwards"
// gesture (Elden Ring 1.17.1; WorldChrMan and character layouts from The Grand Archives table v1.19.1).
//
// Two ways, by skeleton:
// - Characters on the player skeleton (c0000) get the real animation 80220 through their animation request field
//   ([[ChrIns+0x190]+0x58]+0x18), which the game reads on its own thread.
// - Every other character gets the gesture written into its pose: the recorded gesture (Gesture.h, model-space rotations
//   relative to the bind pose of the upper-body bones, 60 frames per second, blended between frames) is applied by bone name to the character's skeleton. Bones it
//   lacks (fingers, a second arm...) are skipped, so a dragon gets whatever matches - crooked on purpose.
//   The pose lives in the character's CS::CSFD4LocationHkaPoseImporter (ChrIns+0x398): +0x48 hkaSkeleton,
//   +0x50 local pose, +0x60 model pose (hkQsTransform arrays). Its vtable slot 3 runs every frame for every character on
//   the game's threads; a stub in that slot calls the game's function and then OnPose, which overwrites the pose of
//   characters currently pointing. Verified in game with the same stub from an external test tool.
// The gesture is done once (kRepeats).
// F10 (game window active) triggers it without dying. The release build writes no log file.
// Built without the Windows SDK and without the C runtime (see build.bat).
typedef unsigned char u8;
typedef short i16;
typedef int i32;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef void* HANDLE;
typedef int BOOL;
typedef u32 DWORD;

extern "C" {
__declspec(dllimport) HANDLE __stdcall CreateThread(void*, u64, DWORD(__stdcall*)(void*), void*, DWORD, DWORD*);
__declspec(dllimport) BOOL __stdcall CloseHandle(HANDLE);
__declspec(dllimport) void __stdcall Sleep(DWORD);
__declspec(dllimport) HANDLE __stdcall GetModuleHandleW(const wchar_t*);
__declspec(dllimport) BOOL __stdcall DisableThreadLibraryCalls(HANDLE);
__declspec(dllimport) DWORD __stdcall GetCurrentProcessId();
__declspec(dllimport) BOOL __stdcall IsBadReadPtr(const void*, u64);
__declspec(dllimport) DWORD __stdcall GetModuleFileNameW(HANDLE, wchar_t*, DWORD);
__declspec(dllimport) HANDLE __stdcall CreateFileW(const wchar_t*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
__declspec(dllimport) u64 __stdcall GetTickCount64();
__declspec(dllimport) void* __stdcall VirtualAlloc(void*, u64, DWORD, DWORD);
__declspec(dllimport) BOOL __stdcall VirtualProtect(void*, u64, DWORD, DWORD*);
__declspec(dllimport) short __stdcall GetAsyncKeyState(int);
__declspec(dllimport) HANDLE __stdcall GetForegroundWindow();
__declspec(dllimport) DWORD __stdcall GetWindowThreadProcessId(HANDLE, DWORD*);
}

extern "C" void* memcpy(void* dst, const void* src, u64 n);
extern "C" void* memset(void* dst, int v, u64 n);
#pragma function(memcpy)
extern "C" void* memcpy(void* dst, const void* src, u64 n)
{
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    while (n--) *d++ = *s++;
    return dst;
}
#pragma function(memset)
extern "C" void* memset(void* dst, int v, u64 n)
{
    u8* d = (u8*)dst;
    while (n--) *d++ = (u8)v;
    return dst;
}
extern "C" int _fltused = 0;

#include "Gesture.h"

static const int VK_F10 = 0x79;
static const float kRadius = 20.0f;
static const DWORD kDelayAfterDeath = 800;
static const i32 kAnim = 80220;
static const int kMaxBones = 512;
static const int kMaxActive = 24;
static const int kRepeats = 1;                                 // how many times in a row the gesture is done
static const u64 kGestureMs = (u64)(kGestureFrames - 1) * 1000 / kGestureFps; // one gesture

// WorldChrMan: 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88 (rip-relative pointer)
static const u8 kWcmPattern[] = { 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0x74, 0x0F, 0x48, 0x39, 0x88 };
static const bool kWcmWild[] = { 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 };

static HANDLE g_module = nullptr;
static u64 g_wcmPtr = 0;
static HANDLE g_log = nullptr;

static bool Readable(const void* p, u64 n) { return p && !IsBadReadPtr(p, n); }
static u64 Ptr(u64 addr) { return Readable((void*)addr, 8) ? *(u64*)addr : 0; }

static u64 Find(const u8* pattern, const bool* wild, u64 len)
{
    u8* base = (u8*)GetModuleHandleW(nullptr);
    i32 peOffset = *(i32*)(base + 0x3C);
    u32 size = *(u32*)(base + peOffset + 0x50); // OptionalHeader.SizeOfImage (PE32+)
    for (u64 i = 0; i + len <= size; i++)
    {
        u64 k = 0;
        while (k < len && (wild[k] || base[i + k] == pattern[k])) k++;
        if (k == len) return (u64)(base + i);
    }
    return 0;
}

// --- log (ASCII, appended next to the DLL; only written from the worker thread) ---
static void LogOpen()
{
    wchar_t path[520];
    DWORD n = GetModuleFileNameW(g_module, path, 512);
    if (n == 0 || n >= 512) return;
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
    const wchar_t name[] = L"PointDownMe.log";
    for (int i = 0; i < (int)(sizeof(name) / sizeof(wchar_t)); i++) path[n + i] = name[i];
    // Release build: no log file is created (the startup sequence is kept as it was tested in game)
    (void)path;
}
static char g_line[512];
static int g_len = 0;
static void Put(const char* s) { while (*s && g_len < 500) g_line[g_len++] = *s++; }
static void PutW(const wchar_t* s, int max) { for (int i = 0; i < max && s[i] && g_len < 500; i++) g_line[g_len++] = s[i] < 128 ? (char)s[i] : '?'; }
static void PutInt(i32 v)
{
    char b[16]; int n = 0; bool neg = v < 0; u32 u = neg ? (u32)(-(v + 1)) + 1 : (u32)v;
    do { b[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) Put("-");
    while (n) { char c[2] = { b[--n], 0 }; Put(c); }
}
static void Flush()
{
    g_line[g_len++] = '\r'; g_line[g_len++] = '\n';
    if (g_log) { DWORD w; WriteFile(g_log, g_line, (DWORD)g_len, &w, nullptr); }
    g_len = 0;
}

// --- quaternion math (x y z w, Hamilton product, as Havok) ---
struct Q { float x, y, z, w; };
struct V { float x, y, z; };
static float Sqrt(float v)
{
    if (v <= 0) return 0;
    float r = v > 1 ? v : 1;
    for (int i = 0; i < 24; i++) r = 0.5f * (r + v / r);
    return r;
}
static Q Mul(Q a, Q b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static Q Conj(Q a) { return { -a.x, -a.y, -a.z, a.w }; }
static Q Norm(Q a)
{
    float l = Sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
    if (l < 1e-6f) return { 0, 0, 0, 1 };
    return { a.x / l, a.y / l, a.z / l, a.w / l };
}
// Normalized linear blend of two rotations (shortest way); close to slerp for neighbouring frames
static Q Nlerp(Q a, Q b, float t)
{
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = { -b.x, -b.y, -b.z, -b.w };
    return Norm({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
}
static V Rot(Q q, V v)
{
    Q p = Mul(Mul(q, { v.x, v.y, v.z, 0 }), Conj(q));
    return { p.x, p.y, p.z };
}
static bool StrEq(const char* a, const char* b)
{
    for (int i = 0; i < 64; i++) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
    return false;
}

// --- characters currently pointing (prepared by the worker thread, read by OnPose on the game's threads) ---
struct Active
{
    volatile u64 importer;   // 0 = free slot
    u64 skeleton;
    int bones;
    u64 start;               // GetTickCount64 at start
    i16 parents[kMaxBones];
    i16 gesture[kMaxBones];  // index into kGestureNames, or -1
    Q bind[kMaxBones];       // model-space bind rotation of driven bones
    // Work buffers for OnPose: one character's importer is updated by one thread at a time. Kept here, not on the
    // stack: the DLL has no stack probes (no C runtime), so large stack frames are avoided.
    Q mr[kMaxBones];
    V mt[kMaxBones];
};
static Active g_active[kMaxActive];

// Called on a game thread right after the game updated this importer's pose
extern "C" void __stdcall OnPose(u64 importer)
{
    for (int a = 0; a < kMaxActive; a++)
    {
        Active& act = g_active[a];
        if (act.importer != importer) continue;
        u64 skel = *(u64*)(importer + 0x48);
        if (skel != act.skeleton || *(i32*)(skel + 0x38) != act.bones) { act.importer = 0; return; }
        u64 elapsed = GetTickCount64() - act.start;
        if (elapsed >= kGestureMs * kRepeats) { act.importer = 0; return; }
        // Position in the gesture in frames, blended between the two nearest frames (smooth at any frame rate)
        u64 inGesture = elapsed % kGestureMs;
        int frame = (int)(inGesture * kGestureFps / 1000);
        float blend = (float)(inGesture * kGestureFps % 1000) / 1000.0f;
        if (frame >= kGestureFrames - 1) { frame = kGestureFrames - 2; blend = 1.0f; }
        float* local = *(float**)(importer + 0x50);
        float* model = *(float**)(importer + 0x60);
        if (!local || !model) return;
        const float* g = kGesture + (u64)frame * kGestureBones * 4;
        const float* g2 = g + kGestureBones * 4;
        Q* mr = act.mr;
        V* mt = act.mt;
        for (int j = 0; j < act.bones; j++)
        {
            float* l = local + j * 12;
            float* m = model + j * 12;
            int p = act.parents[j];
            Q parentRot = p >= 0 ? mr[p] : Q{ 0, 0, 0, 1 };
            Q lr = { l[4], l[5], l[6], l[7] };
            int gi = act.gesture[j];
            if (gi >= 0)
            {
                Q delta = Nlerp({ g[gi * 4], g[gi * 4 + 1], g[gi * 4 + 2], g[gi * 4 + 3] },
                                { g2[gi * 4], g2[gi * 4 + 1], g2[gi * 4 + 2], g2[gi * 4 + 3] }, blend);
                Q want = Norm(Mul(delta, act.bind[j]));
                lr = Norm(Mul(Conj(parentRot), want));
                l[4] = lr.x; l[5] = lr.y; l[6] = lr.z; l[7] = lr.w;
            }
            mr[j] = p >= 0 ? Norm(Mul(parentRot, lr)) : lr;
            V lt = { l[0], l[1], l[2] };
            if (p >= 0) { V r = Rot(parentRot, lt); mt[j] = { mt[p].x + r.x, mt[p].y + r.y, mt[p].z + r.z }; }
            else mt[j] = lt;
            m[0] = mt[j].x; m[1] = mt[j].y; m[2] = mt[j].z; m[3] = l[3];
            m[4] = mr[j].x; m[5] = mr[j].y; m[6] = mr[j].z; m[7] = mr[j].w;
            m[8] = l[8]; m[9] = l[9]; m[10] = l[10]; m[11] = l[11];
        }
        return;
    }
}

// Prepares a character for the pose override. Returns the number of skeleton bones driven by the gesture.
static int StartPose(u64 importer)
{
    u64 skel = Ptr(importer + 0x48);
    if (!Readable((void*)(skel + 0x18), 0x30)) return 0;
    i32 n = *(i32*)(skel + 0x38);
    u64 parents = Ptr(skel + 0x20), bones = Ptr(skel + 0x30), ref = Ptr(skel + 0x40);
    if (n <= 0 || n > kMaxBones || !Readable((void*)parents, n * 2) || !Readable((void*)bones, n * 0x10) || !Readable((void*)ref, n * 0x30)) return 0;
    if (!Readable((void*)Ptr(importer + 0x50), n * 0x30) || !Readable((void*)Ptr(importer + 0x60), n * 0x30)) return 0;
    int slot = -1;
    for (int a = 0; a < kMaxActive; a++) if (g_active[a].importer == importer) { g_active[a].importer = 0; slot = a; break; }
    if (slot < 0) for (int a = 0; a < kMaxActive; a++) if (g_active[a].importer == 0) { slot = a; break; }
    if (slot < 0) return 0;
    Active& act = g_active[slot];
    act.skeleton = skel;
    act.bones = n;
    int driven = 0;
    static Q bindModel[kMaxBones]; // worker thread only
    for (int j = 0; j < n; j++)
    {
        act.parents[j] = *(i16*)(parents + j * 2);
        const float* r = (const float*)(ref + j * 0x30);
        Q lr = { r[4], r[5], r[6], r[7] };
        int p = act.parents[j];
        bindModel[j] = p >= 0 && p < j ? Norm(Mul(bindModel[p], lr)) : lr;
        if (p >= j) { act.parents[j] = -1; } // unexpected order: treat as a root, never read ahead
        act.bind[j] = bindModel[j];
        act.gesture[j] = -1;
        const char* name = (const char*)Ptr(bones + j * 0x10);
        if (!Readable(name, 1)) continue;
        for (int gi = 0; gi < kGestureBones; gi++)
        {
            if (StrEq(name, kGestureNames[gi])) { act.gesture[j] = (i16)gi; driven++; break; }
        }
    }
    if (driven == 0) return 0;
    act.start = GetTickCount64();
    act.importer = importer; // publish last
    return driven;
}

// Vtable slot 3 of CS::CSFD4LocationHkaPoseImporter: stub "call original, then OnPose(this)"
static bool g_hooked = false;
static bool InstallHook(u64 importer)
{
    u64 vt = Ptr(importer);
    if (!vt) return false;
    u64* slot = (u64*)(vt + 3 * 8);
    u64 orig = *slot;
    u8* stub = (u8*)VirtualAlloc(nullptr, 0x100, 0x3000, 0x40); // MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
    if (!stub) return false;
    u64* origSlot = (u64*)(stub + 0x80);
    u64* hookSlot = (u64*)(stub + 0x88);
    *origSlot = orig;
    *hookSlot = (u64)&OnPose;
    int i = 0;
    const u8 head[] = {
        0x53,                         // push rbx
        0x56,                         // push rsi
        0x48, 0x83, 0xEC, 0x28,       // sub rsp, 28h (aligned for the calls)
        0x48, 0x89, 0xCB,             // mov rbx, rcx
    };
    for (u8 b : head) stub[i++] = b;
    stub[i++] = 0xFF; stub[i++] = 0x15;   // call [rip+origSlot]
    i32 d1 = (i32)((u64)origSlot - (u64)(stub + i + 4)); memcpy(stub + i, &d1, 4); i += 4;
    const u8 mid[] = {
        0x48, 0x89, 0xC6,             // mov rsi, rax
        0x48, 0x89, 0xD9,             // mov rcx, rbx
    };
    for (u8 b : mid) stub[i++] = b;
    stub[i++] = 0xFF; stub[i++] = 0x15;   // call [rip+hookSlot]
    i32 d2 = (i32)((u64)hookSlot - (u64)(stub + i + 4)); memcpy(stub + i, &d2, 4); i += 4;
    const u8 tail[] = {
        0x48, 0x89, 0xF0,             // mov rax, rsi
        0x48, 0x83, 0xC4, 0x28,       // add rsp, 28h
        0x5E,                         // pop rsi
        0x5B,                         // pop rbx
        0xC3,                         // ret
    };
    for (u8 b : tail) stub[i++] = b;
    DWORD old;
    if (!VirtualProtect(slot, 8, 0x04, &old)) return false;
    *slot = (u64)stub;
    VirtualProtect(slot, 8, old, &old);
    return true;
}

// --- characters (offsets from The Grand Archives table) ---
static u64 Modules(u64 chr) { return Ptr(chr + 0x190); }
static i32 Hp(u64 chr)
{
    u64 data = Ptr(Modules(chr));
    return Readable((void*)(data + 0x138), 4) ? *(i32*)(data + 0x138) : -1;
}
static bool Pos(u64 chr, float* out)
{
    u64 phys = Ptr(Modules(chr) + 0x68);
    if (!Readable((void*)(phys + 0x70), 12)) return false;
    memcpy(out, (void*)(phys + 0x70), 12);
    return true;
}
static const wchar_t* Model(u64 chr)
{
    u64 data = Ptr(Modules(chr));
    return Readable((void*)(data + 0x1A2), 16) ? (const wchar_t*)(data + 0x1A2) : L"?";
}
static u64 Player() { return Ptr(Ptr(g_wcmPtr) + 0x1E508); }
static bool IsNetPlayer(u64 chr)
{
    u64 net = Ptr(Ptr(g_wcmPtr) + 0x10EF8);
    for (int i = 0; i < 6; i++) if (net && Ptr(net + 0x10 * i) == chr) return true;
    return false;
}
static bool OnPlayerSkeleton(u64 importer)
{
    const char* name = (const char*)Ptr(Ptr(importer + 0x48) + 0x18);
    return Readable(name, 6) && StrEq(name, "c0000");
}

// Player-skeleton characters asked to play 80220 again after each gesture (worker thread only)
static u64 g_repeatChr[64];
static u64 g_repeatVt[64];    // vtable and event module at the start: the same object must still be there
static u64 g_repeatEvent[64];
static int g_repeatCount = 0;
static u64 g_repeatStart = 0;
static int g_repeatDone = 0;

static void RequestAnim(u64 chr)
{
    u64 eventModule = Ptr(Modules(chr) + 0x58);
    if (Readable((void*)(eventModule + 0x18), 4)) *(volatile i32*)(eventModule + 0x18) = kAnim;
}

static void RepeatAnimations()
{
    if (g_repeatCount == 0 || g_repeatDone >= kRepeats) return;
    u64 elapsed = GetTickCount64() - g_repeatStart;
    if (elapsed < kGestureMs * (u64)g_repeatDone) return;
    for (int i = 0; i < g_repeatCount; i++)
    {
        u64 chr = g_repeatChr[i];
        // still the same loaded, living character (it may unload while pointing)
        if (Ptr(chr) == g_repeatVt[i] && Ptr(Modules(chr) + 0x58) == g_repeatEvent[i] && Hp(chr) > 0) RequestAnim(chr);
    }
    g_repeatDone++;
}

static void PointDown(const char* why)
{
    u64 player = Player();
    float pp[3];
    if (!player || !Pos(player, pp)) return;
    Put(why); Flush();
    int animated = 0, posed = 0, skipped = 0;
    g_repeatCount = 0;
    static const int kSets[] = { 0x1E270, 0x1CC60 }; // open field, legacy dungeon
    for (int s = 0; s < 2; s++)
    {
        u64 set = Ptr(Ptr(g_wcmPtr) + kSets[s]);
        if (!Readable((void*)(set + 0x10), 0x10)) continue;
        i32 count = *(i32*)(set + 0x10);
        u64 arr = Ptr(set + 0x18);
        if (count <= 0 || count > 4096 || !arr) continue;
        for (int i = 0; i < count; i++)
        {
            u64 chr = Ptr(arr + 0x10ull * i);
            if (!chr || chr == player || IsNetPlayer(chr) || Hp(chr) <= 0) continue;
            float cp[3];
            if (!Pos(chr, cp)) continue;
            float dx = cp[0] - pp[0], dy = cp[1] - pp[1], dz = cp[2] - pp[2];
            float dist = Sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > kRadius) continue;
            u64 importer = Ptr(chr + 0x398);
            Put("  c"); PutW(Model(chr), 8); Put(" dist "); PutInt((i32)dist); Put("m: ");
            if (importer && OnPlayerSkeleton(importer))
            {
                if (Readable((void*)(Ptr(Modules(chr) + 0x58) + 0x18), 4))
                {
                    if (g_repeatCount < 64)
                    {
                        g_repeatChr[g_repeatCount] = chr;
                        g_repeatVt[g_repeatCount] = Ptr(chr);
                        g_repeatEvent[g_repeatCount] = Ptr(Modules(chr) + 0x58);
                        g_repeatCount++;
                    }
                    animated++; Put("player skeleton, animation 80220");
                }
                else { skipped++; Put("no animation request field"); }
            }
            else if (importer)
            {
                if (!g_hooked) g_hooked = InstallHook(importer);
                int driven = g_hooked ? StartPose(importer) : 0;
                if (driven > 0) { posed++; Put("pose override, "); PutInt(driven); Put(" bones"); }
                else { skipped++; Put(g_hooked ? "no matching bones" : "hook failed"); }
            }
            else { skipped++; Put("no pose"); }
            Flush();
        }
    }
    g_repeatStart = GetTickCount64();
    g_repeatDone = 0;
    RepeatAnimations(); // first request now, the others after each gesture
    Put("  animation "); PutInt(animated); Put(", pose override "); PutInt(posed); Put(", skipped "); PutInt(skipped); Flush();
}

static bool GameFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static DWORD __stdcall Worker(void*)
{
    LogOpen();
    for (int i = 0; i < 180 && !g_wcmPtr; i++)
    {
        u64 at = Find(kWcmPattern, kWcmWild, sizeof(kWcmPattern));
        if (at) g_wcmPtr = at + 7 + *(i32*)(at + 3);
        else Sleep(1000);
    }
    if (!g_wcmPtr) { Put("game code not found (other game version?); disabled"); Flush(); return 0; }
    Put("PointDownMe 1.0.1 ready"); Flush();
    i32 lastHp = -1;
    bool wasDown = false;
    for (;;)
    {
        Sleep(50);
        RepeatAnimations();
        u64 player = Player();
        i32 hp = player ? Hp(player) : -1;
        if (lastHp > 0 && hp == 0)
        {
            Sleep(kDelayAfterDeath);
            PointDown("player died:");
        }
        lastHp = hp;
        bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (down && !wasDown && GameFocused()) PointDown("F10:");
        wasDown = down;
    }
}

extern "C" BOOL __stdcall DllMain(HANDLE module, DWORD reason, void*)
{
    if (reason == 1) // DLL_PROCESS_ATTACH
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return 1;
}
