# H3VR2 Infinite Ammo (+ No Recoil + Quickbelt Duplication)

A native IL2CPP mod for **Hot Dogs, Horseshoes & Hand Grenades 2** ("Footlong",
`com.rustltd.h3vr2`) on **Meta Quest 3**. It gives every firearm type infinite ammo, removes
felt recoil, and re-enables the game's own quickbelt duplication feature.

Built directly on **[Scotland2](https://github.com/sc2ad/scotland2)** +
**[flamingo](https://github.com/QuestPackageManager/Flamingo)** — no BeatSaber‑Hook — because
flamingo's C API plus hand-resolved IL2CPP offsets sidesteps the libil2cpp version mismatch that
breaks beatsaber-hook on this game.

> ⚠️ **Version-locked.** Every address here is an RVA from the dump of build **`20261115D`**.
> A game update will move them. See [Updating offsets](#updating-offsets).

---

## Features

### Infinite ammo — all firearm types
Magazines never deplete, chambered rounds never go to spent casings, and everything fires live.
Each gun family feeds differently, so each gets its own narrow hook:

| Gun family | Mechanism |
|---|---|
| **Magazine-fed / self-loading** | Hook `CyclicBolt.Breech.LoadRoundFromMagazine`; after each feed, mint a **fresh** `RoundData` of the same type back into the magazine so it never empties. |
| **Bolt-action** | A different breech class (`BoltAction.BoltActionBreech`): hook `LoadRoundFromOwnMagazine` and top its connected magazine back up the same way. |
| **Pump / tube shotgun** | Chambers off a shell lifter (`MagazineCarrierLifter.RoundCarrier`): hook `CyclicBolt.Breech.LoadRoundFromRoundCarrier` and top up the lifter's **upstream tube magazine** (`RoundCarrier.connectedMagazine`) — never the lifter itself, which would make the gun re-chamber continuously and spawn duplicate round objects. |
| **Breach / break-action / derringer / trapdoor / rolling- & falling-block** | Hook `BreachLoadingFirearm.HammerImpact`; after the shot, walk the loaded rounds and flip `fired` back to `false` so the chambered round never spends. |
| **Revolver** | Cache the `Revolver` class on spawn (`Awake`), then at the universal `FirearmBase.Fire` chokepoint un-spend the fired chamber's round — but **only** when the firearm is actually a revolver, so nothing else is touched. |

Minting fresh rounds (rather than recycling instances) is deliberate: it avoids the aliasing bug
where a reused `RoundData` goes spent on both the magazine copy and the chambered copy (one shot,
then endless empty casings), and it leaves manual unloading untouched (no duplicate spent rounds
jamming the extractor).

### No recoil
Every gun's felt kick and muzzle climb flows through the `Recoil` component's four public impulse
methods (`AddRecoilImpulseAtPosition`, `AddRecoilLinearImpulse`, `AddRecoilAngularImpulse`,
`AddRecoilLinearAndAngularImpulse`). The mod no-ops all four, so the recoil simulation never
integrates any impulse — **zero kick, zero climb, for every firearm at once**. This is *felt*
recoil only: blowback and bolt cycling run off the breech's own momentum, so semi- and full-auto
guns still cycle normally.

### Quickbelt duplication ("spawn lock")
H3VR2 **already ships** the first game's duplication feature on the `Pocket` class
(`m_spawnLocked`, `ToggleSpawnLock`, the per-item `allowSpawnLocking` flag), but
`Pocket.SpawnLockAllowed()` gates it off by game mode. The mod forces that static gate to return
`true`, re-enabling it everywhere.

**Use it:** hover a hand over a quickbelt pocket and press **Primary** (the **A / X** face button).
The pocket recolors to show it's locked; now pulling an item from it leaves a duplicate behind.

The game's own per-item rules still apply, so **guns won't duplicate** while **ammo, magazines,
grenades, toys and powerups** will — no custom allow-list needed.

---

## Install

**Requires** `libflamingo.so` present in the modloader `libs` dir (the bundled `.qmod` lists
flamingo as a dependency so QuestPatcher can fetch it).

### Current method — manual `libs` drop (what actually works today)
Scotland2 v0.1.4 can't find the Unity hook location on Unity 6000.3, so it never calls
`late_load` and the late-mods folder is dead. The mod loads instead from the early **`libs`**
phase (its `__attribute__((constructor))` fires on `dlopen`, before libil2cpp maps, and a detached
poller installs the hooks once it does). Deploy the `.so` into Scotland2's staging `libs` dir and
it gets moved into the app and loaded on launch:

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$dst = "/sdcard/ModData/com.rustltd.h3vr2/Modloader/libs/libh3vr2infammo.so"
# push via /data/local/tmp — pushing straight to the app dir fails with a chown error
& $adb push .\build\libh3vr2infammo.so /data/local/tmp/libh3vr2infammo.so
& $adb shell "cp /data/local/tmp/libh3vr2infammo.so $dst"
& $adb shell am force-stop com.rustltd.h3vr2
& $adb shell monkey -p com.rustltd.h3vr2 -c android.intent.category.LAUNCHER 1
```

Then put the headset on and tap **Open** on Meta's unofficial-app dialog.

### `.qmod` (for when QuestPatcher support matures)
`h3vr2infammo.qmod` is a standard QuestPatcher package (its binary is a `libraryFile`, i.e. it
installs to `libs`, not late-mods). Install it with QuestPatcher once H3VR2 is a supported app.

---

## Build from source

**Toolchain:** [qpm](https://github.com/QuestPackageManager/QPM.CLI), Android **NDK r29**
(`29.0.14206865`), CMake ≥ 4.5, Ninja. (This repo was built with qpm 1.5.12, cmake 4.5.0,
ninja 1.13.2, C++23.)

```powershell
qpm restore              # fetches scotland2 + flamingo headers into extern/
.\build.ps1              # cmake + ninja -> build/libh3vr2infammo.so
```

`build.ps1` points CMake at the local cmake/ninja; adjust the two paths at the top if yours differ.

Package a `.qmod`:

```powershell
qpm qmod manifest        # fills mod.template.json -> mod.json
# then zip mod.json + build/libh3vr2infammo.so into h3vr2infammo.qmod
# (qpm qmod zip also works if you have PowerShell 7 / pwsh on PATH)
```

---

## How it loads

1. `__attribute__((constructor)) on_dlopen` runs the moment Scotland2 `dlopen`s the `.so` — but
   `libil2cpp.so` isn't mapped yet, so it spawns a detached `pthread` poller.
2. The poller waits for `libil2cpp.so` via `dl_iterate_phdr`, records its load base, and installs
   every hook (`base + RVA`).
3. `il2cpp_object_new` is resolved by **base + RVA**, *not* `dlsym` — namespace isolation makes
   `dlsym(RTLD_DEFAULT, "il2cpp_object_new")` return `0x0` here.

All logging is under logcat tag `h3vr2infammo`.

---

## Offsets (build `20261115D`)

RVAs are the `dump.cs` **RVA** field (not the file Offset), added to the libil2cpp.so load base.

| Const | RVA | Target |
|---|---|---|
| `RVA_LoadRoundFromMag` | `0x4E7D604` | `CyclicBolt.Breech.LoadRoundFromMagazine(Magazine,int)` |
| `RVA_LoadRoundFromOwnMag` | `0x4E6E30C` | `BoltAction.BoltActionBreech.LoadRoundFromOwnMagazine(int)` |
| `RVA_LoadRoundFromCarrier` | `0x4E7DA44` | `CyclicBolt.Breech.LoadRoundFromRoundCarrier(RoundCarrier)` |
| `RVA_GetRound` | `0x4E348D0` | `RoundHolder.GetRound(int)` |
| `RVA_LoadRound` | `0x4E34954` | `RoundHolder.LoadRound(RoundData)` |
| `RVA_RoundDataCtor` | `0x4E31BB4` | `RoundData..ctor(ItemDef)` |
| `RVA_ObjectNew` | `0x486CD94` | `il2cpp_object_new` (libil2cpp export) |
| `RVA_HammerImpact` | `0x4DC0968` | `BreachLoadingFirearm.HammerImpact(int,bool)` |
| `RVA_GetLoadedRounds` | `0x4DC0810` | `BreachLoadingFirearm.GetLoadedRounds()` |
| `RVA_Fire` | `0x4DE3390` | `FirearmBase.Fire(Chamber,Barrel)` |
| `RVA_RevolverAwake` | `0x4E21DE8` | `Revolver.Awake()` |
| `RVA_ChamberLoadedRnd` | `0x4E6B878` | `Chamber.LoadedRound()` |
| `RVA_SpawnLockAllowed` | `0x4C76E8C` | `Pocket.SpawnLockAllowed()` (static gate) |
| `RVA_RecoilAtPos` | `0x4E179C0` | `Recoil.AddRecoilImpulseAtPosition(Vector3,Vector3)` |
| `RVA_RecoilLinear` | `0x4E1D650` | `Recoil.AddRecoilLinearImpulse(Vector3)` |
| `RVA_RecoilAngular` | `0x4E1D8F0` | `Recoil.AddRecoilAngularImpulse(Vector3)` |
| `RVA_RecoilLinAng` | `0x4E1DB6C` | `Recoil.AddRecoilLinearAndAngularImpulse(Vector3,Vector3)` |

Struct offsets: `Magazine.roundHolder 0x260`, `BoltAction.BoltActionBreech.connectedMagazine 0x60`,
`MagazineCarrierLifter.RoundCarrier.connectedMagazine 0x20`, `RoundData.def 0x10`,
`RoundData.fired 0x20`, `Il2CppArray` count `0x18` / vector `0x20` (64-bit).

### Updating offsets
On a game update, re-dump with [Il2CppDumper](https://github.com/Perfare/Il2CppDumper)
(`libil2cpp.so` + `global-metadata.dat`), find each method in `dump.cs`, and replace the RVAs in
[`src/main.cpp`](src/main.cpp). Verify struct offsets (the `// 0x..` field comments) haven't moved.
`il2cpp_object_new`'s RVA comes from the libil2cpp export table (`llvm-readelf --dyn-syms`).

---

## Credits / license

- [Scotland2](https://github.com/sc2ad/scotland2) — modloader
- [flamingo](https://github.com/QuestPackageManager/Flamingo) — hooking engine
- [Il2CppDumper](https://github.com/Perfare/Il2CppDumper) — metadata/offset extraction

Personal project, provided as-is for educational/modding use. Not affiliated with or endorsed by
Rust Ltd. Use on software you own.
