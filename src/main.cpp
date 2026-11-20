// H3VR2 Infinite Ammo (native Scotland2 mod).
//
// Approach: hook the bolt's magazine FEED (CyclicBolt.Breech.LoadRoundFromMagazine) and,
// after each feed, top the magazine back up with a BRAND-NEW round of the same type. We do
// NOT hook the low-level RoundHolder.RemoveRound, because that also fires during manual
// round handling (so re-adding there locks you out of emptying a gun) and because recycling
// the same RoundData instance makes it go "spent" (one shot, then endless empty casings).
// Minting a fresh RoundData(def) keeps every chambered round live and leaves manual
// unloading untouched.
//
// Coverage, mag-fed path: standard magazine-fed + bolt guns feed through this hook.
//
// Breach loaders (break actions, derringers, trapdoor/rolling/falling-block single shots)
// have no magazine -- you hand-load rounds straight into chambers, and after firing the
// spent casing just sits in the chamber (RoundData.fired == true) until you extract it.
// So we hook BreachLoadingFirearm.HammerImpact and, after each shot, flip every loaded
// round back to fired == false. The chambered round never goes spent, so it keeps firing
// live with no reload. No minting / re-adding here, so nothing can duplicate or lock
// extraction, and this is scoped to breach guns, so the mag-fed path above is untouched.
//
// Revolvers are their own class with no per-shot method to hook, so they go through the
// universal FirearmBase.Fire(Chamber,Barrel) chokepoint instead. We hook Fire, but only
// act when the firearm is actually a Revolver (its class pointer, cached the first time a
// revolver is loaded) -- then we flip that chamber's round back to live. Every other gun
// type (mag-fed, self-loading, breach) falls straight through Fire untouched.
//
// Duplication ("spawn lock"): H3VR2 already ships the first game's quickbelt duplication
// feature on Pocket (m_spawnLocked, ToggleSpawnLock, the per-item allowSpawnLocking flag),
// but Pocket.SpawnLockAllowed() gates it off by game mode. We force that static gate to
// return true, which re-enables toggling dupe mode on any pocket (press Primary/A-X while
// hovering it). The game's own per-item rules still apply, so guns stay non-dupeable while
// ammo, grenades, toys and powerups can be duplicated.
//
// All addresses are the dump.cs RVA field, added to the libil2cpp.so load base.

#include "scotland2/shared/modloader.h"
#include "flamingo/shared/capi.h"

#include <android/log.h>
#include <link.h>      // dl_iterate_phdr
#include <dlfcn.h>     // dlsym (il2cpp_object_new)
#include <cstring>
#include <cstdint>
#include <pthread.h>
#include <unistd.h>    // usleep

#define MOD_EXPORT extern "C" __attribute__((visibility("default")))
#define LOG_TAG "h3vr2infammo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// ===== dump.cs RVAs / struct offsets =====
static constexpr std::uintptr_t RVA_LoadRoundFromMag = 0x4E7D604; // CyclicBolt.Breech.LoadRoundFromMagazine(Magazine,int):bool
static constexpr std::uintptr_t RVA_GetRound         = 0x4E348D0; // RoundHolder.GetRound(int):RoundData
static constexpr std::uintptr_t RVA_LoadRound        = 0x4E34954; // RoundHolder.LoadRound(RoundData):void
static constexpr std::uintptr_t RVA_RoundDataCtor    = 0x4E31BB4; // RoundData..ctor(ItemDef def)
static constexpr std::uintptr_t RVA_ObjectNew        = 0x486CD94; // native il2cpp_object_new export (from libil2cpp dynsyms)
static constexpr std::uintptr_t RVA_HammerImpact     = 0x4DC0968; // BreachLoadingFirearm.HammerImpact(int,bool)
static constexpr std::uintptr_t RVA_GetLoadedRounds  = 0x4DC0810; // BreachLoadingFirearm.GetLoadedRounds():RoundData[]
static constexpr std::uintptr_t RVA_Fire             = 0x4DE3390; // FirearmBase.Fire(Chamber,Barrel)
static constexpr std::uintptr_t RVA_RevolverAwake    = 0x4E21DE8; // Revolver.Awake() -- cache the class on spawn
static constexpr std::uintptr_t RVA_ChamberLoadedRnd = 0x4E6B878; // Chamber.LoadedRound():RoundData
static constexpr std::uintptr_t RVA_SpawnLockAllowed = 0x4C76E8C; // Pocket.SpawnLockAllowed():bool (static gate)
static constexpr std::size_t MAG_ROUNDHOLDER_OFFSET  = 0x260;     // Magazine.roundHolder
static constexpr std::size_t ROUNDDATA_DEF_OFFSET    = 0x10;      // RoundData.def (ItemDef)
static constexpr std::size_t ROUNDDATA_FIRED_OFFSET  = 0x20;      // RoundData.fired (bool)
static constexpr std::size_t ARRAY_COUNT_OFFSET      = 0x18;      // Il2CppArray.max_length (64-bit)
static constexpr std::size_t ARRAY_DATA_OFFSET       = 0x20;      // Il2CppArray.vector  (64-bit)

// ---- libil2cpp base ----
static std::uintptr_t g_base = 0;
static int phdrCb(struct dl_phdr_info* info, size_t, void*) {
    if (info->dlpi_name && std::strstr(info->dlpi_name, "libil2cpp.so")) {
        g_base = static_cast<std::uintptr_t>(info->dlpi_addr); return 1;
    }
    return 0;
}
static bool findBase() { if (!g_base) dl_iterate_phdr(phdrCb, nullptr); return g_base != 0; }
static void* real(std::uintptr_t rva) { return reinterpret_cast<void*>(g_base + rva); }

// ---- game method pointers ----
typedef bool  (*LRFM_t)(void* self, void* mag, int index, void* method);
typedef void* (*GetRound_t)(void* holder, int index, void* method);
typedef void  (*LoadRound_t)(void* holder, void* round, void* method);
typedef void  (*RoundCtor_t)(void* self, void* def, void* method);
typedef void* (*ObjNew_t)(void* klass);
typedef void  (*HammerImpact_t)(void* self, int index, bool fire, void* method);
typedef void* (*GetLoadedRounds_t)(void* self, void* method);
typedef void  (*Fire_t)(void* self, void* chamber, void* barrel, void* method);
typedef void  (*RevAwake_t)(void* self, void* method);
typedef void* (*LoadedRound_t)(void* chamber, void* method);
typedef bool  (*SpawnLockAllowed_t)(void* method);

static LRFM_t            LRFM_orig         = nullptr;
static GetRound_t        p_GetRound        = nullptr;
static LoadRound_t       p_LoadRound       = nullptr;
static RoundCtor_t       p_RoundCtor       = nullptr;
static ObjNew_t          p_ObjNew          = nullptr;
static HammerImpact_t    HI_orig           = nullptr;
static GetLoadedRounds_t p_GetLoadedRounds = nullptr;
static Fire_t            Fire_orig         = nullptr;
static RevAwake_t        RevAwake_orig     = nullptr;
static LoadedRound_t     p_LoadedRound     = nullptr;
static SpawnLockAllowed_t SLA_orig         = nullptr;
static void*             g_revolverClass   = nullptr;
static bool              g_logged          = false;
static bool              g_loggedBreach    = false;
static bool              g_loggedRevolver  = false;
static bool              g_loggedSpawnLock = false;

// Mint a fresh live RoundData of the same type as `tmpl` and drop it into `holder`.
static void topUp(void* holder, void* tmpl) {
    if (!holder || !tmpl || !p_ObjNew || !p_RoundCtor || !p_LoadRound) return;
    void* klass = *reinterpret_cast<void**>(tmpl); // il2cpp object header: first word is Il2CppClass*
    void* def   = *reinterpret_cast<void**>(reinterpret_cast<char*>(tmpl) + ROUNDDATA_DEF_OFFSET);
    if (!klass || !def) return;
    void* fresh = p_ObjNew(klass);
    if (!fresh) return;
    p_RoundCtor(fresh, def, nullptr);    // new RoundData(def): fired == false
    p_LoadRound(holder, fresh, nullptr); // back into the magazine
}

static bool LoadRoundFromMagazine_hook(void* self, void* mag, int index, void* method) {
    void* holder = mag ? *reinterpret_cast<void**>(reinterpret_cast<char*>(mag) + MAG_ROUNDHOLDER_OFFSET) : nullptr;
    void* tmpl   = (holder && p_GetRound) ? p_GetRound(holder, 0, nullptr) : nullptr; // peek a round for its type
    bool  result = LRFM_orig(self, mag, index, method);                               // let the bolt feed (depletes mag by 1)
    if (result && tmpl) {
        topUp(holder, tmpl);                                                          // replace it with a fresh live round
        if (!g_logged) { LOGI("fed + topped up magazine with a fresh round"); g_logged = true; }
    }
    return result;
}

// Breach loaders: after the hammer fires, un-spend every round still in a chamber so the
// gun keeps firing live with no reload.
static void HammerImpact_hook(void* self, int index, bool fire, void* method) {
    HI_orig(self, index, fire, method);                         // let the shot happen first
    if (!fire || !self || !p_GetLoadedRounds) return;
    void* arr = p_GetLoadedRounds(self, nullptr);               // RoundData[] of chambered rounds
    if (!arr) return;
    uint32_t n = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(arr) + ARRAY_COUNT_OFFSET);
    void** elems = reinterpret_cast<void**>(reinterpret_cast<char*>(arr) + ARRAY_DATA_OFFSET);
    for (uint32_t i = 0; i < n; ++i) {
        if (elems[i]) *reinterpret_cast<bool*>(reinterpret_cast<char*>(elems[i]) + ROUNDDATA_FIRED_OFFSET) = false;
    }
    if (!g_loggedBreach) { LOGI("breach: reset %u chambered round(s) to live", n); g_loggedBreach = true; }
}

// A revolver spawning runs Awake; remember its il2cpp class so the Fire hook can tell
// revolvers apart from every other firearm.
static void RevolverAwake_hook(void* self, void* method) {
    if (self && !g_revolverClass) { g_revolverClass = *reinterpret_cast<void**>(self); LOGI("revolver class cached: %p", g_revolverClass); }
    RevAwake_orig(self, method);
}

// Universal fire chokepoint. Act only for revolvers: after the shot, un-spend the round in
// the chamber that just fired so the cylinder never empties.
static void Fire_hook(void* self, void* chamber, void* barrel, void* method) {
    Fire_orig(self, chamber, barrel, method);
    if (!self || !chamber || !g_revolverClass || !p_LoadedRound) return;
    if (*reinterpret_cast<void**>(self) != g_revolverClass) return;   // revolvers only
    void* round = p_LoadedRound(chamber, nullptr);
    if (!g_loggedRevolver) { LOGI("revolver Fire: round=%p", round); g_loggedRevolver = true; }
    if (round) *reinterpret_cast<bool*>(reinterpret_cast<char*>(round) + ROUNDDATA_FIRED_OFFSET) = false;
}

// Re-enable the game's built-in quickbelt duplication by forcing its game-mode gate open.
// Per-item/per-pocket rules still apply, so guns stay non-dupeable.
static bool SpawnLockAllowed_hook(void* method) {
    (void)method;
    if (!g_loggedSpawnLock) { LOGI("spawnlock gate forced open"); g_loggedSpawnLock = true; }
    return true;
}

static bool g_installed = false;
static void installAll(const char* via) {
    if (g_installed) return;
    if (!findBase()) return;
    g_installed = true;
    p_GetRound  = reinterpret_cast<GetRound_t>(real(RVA_GetRound));
    p_LoadRound = reinterpret_cast<LoadRound_t>(real(RVA_LoadRound));
    p_RoundCtor = reinterpret_cast<RoundCtor_t>(real(RVA_RoundDataCtor));
    p_ObjNew    = reinterpret_cast<ObjNew_t>(real(RVA_ObjectNew)); // base+RVA, not dlsym (namespace-isolated)
    LOGI("[%s] base=0x%lx il2cpp_object_new=%p; installing feed hook", via, (unsigned long)g_base, (void*)p_ObjNew);

    void* target   = real(RVA_LoadRoundFromMag);
    auto  nameInfo = flamingo_make_name("LoadRoundFromMag");
    auto  priority = flamingo_make_priority(nullptr, 0, nullptr, 0, false);
    auto  meta     = flamingo_make_install_metadata(true, false, true);
    auto  res = flamingo_install_hook_full(reinterpret_cast<void*>(&LoadRoundFromMagazine_hook),
                                           reinterpret_cast<uint32_t*>(target), reinterpret_cast<void**>(&LRFM_orig),
                                           10, FLAMINGO_CDECL, nameInfo, priority, meta);
    LOGI("feed hook %s at %p", res.result == FLAMINGO_INSTALL_OK ? "installed" : "FAILED", target);

    // ---- breach-loader hook ----
    p_GetLoadedRounds = reinterpret_cast<GetLoadedRounds_t>(real(RVA_GetLoadedRounds));
    void* bTarget   = real(RVA_HammerImpact);
    auto  bName     = flamingo_make_name("HammerImpact");
    auto  bPriority = flamingo_make_priority(nullptr, 0, nullptr, 0, false);
    auto  bMeta     = flamingo_make_install_metadata(true, false, true);
    auto  bRes = flamingo_install_hook_full(reinterpret_cast<void*>(&HammerImpact_hook),
                                            reinterpret_cast<uint32_t*>(bTarget), reinterpret_cast<void**>(&HI_orig),
                                            10, FLAMINGO_CDECL, bName, bPriority, bMeta);
    LOGI("breach hook %s at %p", bRes.result == FLAMINGO_INSTALL_OK ? "installed" : "FAILED", bTarget);

    // ---- revolver hooks ----
    p_LoadedRound = reinterpret_cast<LoadedRound_t>(real(RVA_ChamberLoadedRnd));
    void* rTarget   = real(RVA_RevolverAwake);
    auto  rName     = flamingo_make_name("RevolverAwake");
    auto  rPriority = flamingo_make_priority(nullptr, 0, nullptr, 0, false);
    auto  rMeta     = flamingo_make_install_metadata(true, false, true);
    auto  rRes = flamingo_install_hook_full(reinterpret_cast<void*>(&RevolverAwake_hook),
                                            reinterpret_cast<uint32_t*>(rTarget), reinterpret_cast<void**>(&RevAwake_orig),
                                            10, FLAMINGO_CDECL, rName, rPriority, rMeta);
    LOGI("revolver awake hook %s at %p", rRes.result == FLAMINGO_INSTALL_OK ? "installed" : "FAILED", rTarget);

    void* fTarget   = real(RVA_Fire);
    auto  fName     = flamingo_make_name("Fire");
    auto  fPriority = flamingo_make_priority(nullptr, 0, nullptr, 0, false);
    auto  fMeta     = flamingo_make_install_metadata(true, false, true);
    auto  fRes = flamingo_install_hook_full(reinterpret_cast<void*>(&Fire_hook),
                                            reinterpret_cast<uint32_t*>(fTarget), reinterpret_cast<void**>(&Fire_orig),
                                            10, FLAMINGO_CDECL, fName, fPriority, fMeta);
    LOGI("fire hook %s at %p", fRes.result == FLAMINGO_INSTALL_OK ? "installed" : "FAILED", fTarget);

    // ---- duplication: force the spawn-lock gate open ----
    void* sTarget   = real(RVA_SpawnLockAllowed);
    auto  sName     = flamingo_make_name("SpawnLockAllowed");
    auto  sPriority = flamingo_make_priority(nullptr, 0, nullptr, 0, false);
    auto  sMeta     = flamingo_make_install_metadata(true, false, true);
    auto  sRes = flamingo_install_hook_full(reinterpret_cast<void*>(&SpawnLockAllowed_hook),
                                            reinterpret_cast<uint32_t*>(sTarget), reinterpret_cast<void**>(&SLA_orig),
                                            10, FLAMINGO_CDECL, sName, sPriority, sMeta);
    LOGI("spawnlock hook %s at %p", sRes.result == FLAMINGO_INSTALL_OK ? "installed" : "FAILED", sTarget);
}

// libil2cpp isn't mapped yet at dlopen; poll on a detached pthread, then install.
static void* installPoller(void*) {
    for (int i = 0; i < 1200 && !g_installed; ++i) {
        if (findBase()) { installAll("ctor-poll"); return nullptr; }
        usleep(50000);
    }
    return nullptr;
}

__attribute__((constructor))
static void on_dlopen() {
    LOGI("constructor; polling until libil2cpp.so maps");
    pthread_t t;
    if (pthread_create(&t, nullptr, installPoller, nullptr) == 0) pthread_detach(t);
}

MOD_EXPORT void setup(CModInfo* info) {
    info->id = MOD_ID; info->version = VERSION; info->version_long = 0;
    installAll("setup");
}
MOD_EXPORT void load()      { installAll("load"); }
MOD_EXPORT void late_load() { installAll("late_load"); }
