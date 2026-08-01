# Dreadnought Test Bench

An offline-revival patch for **Dreadnought** (Yager / Grey Box), whose official
servers were shut down. This is a fork of
[gwog's Dreadnought Revival Patch](https://github.com/SyST3MDeV/Dreadnought)
that focuses on getting the **frontend menus** working offline — buying ships,
owning them, building fleets, and per-ship tech trees — rather than only
launching straight into a map.

**You must already own Dreadnought on Steam.** No game files are distributed
here, and none ever will be. See [What is deliberately not in this repo](#what-is-deliberately-not-in-this-repo).

> **Status: work in progress, and genuinely rough.** This is a test bench, not a
> finished patch. Read [Known issues](#known-issues) before expecting it to work.

---

## Credits

This project stands on other people's work:

- **gwog** ([SyST3MDeV/Dreadnought](https://github.com/SyST3MDeV/Dreadnought)) —
  the original Revival Patch, and the reason any of this runs at all: the
  `wer.dll` injection shim, the launcher, and the Visual Studio project
  scaffolding. Every fix in this fork loads through his shim.
- **CorrM** — [Cheat Gear](https://github.com/CorrM/Cheat-Gear) SDK Generator,
  which produced the ~2,000 headers in `SDK/`.
- **nohbdy** — the DLL shim generation script.
- **MinHook** (Tsuda Kageyu) and **kiero** — vendored hooking libraries.

For the sake of an accurate record: the frontend revival work in this fork is
largely new. `dllmain.cpp` has grown from gwog's 946 lines to roughly 12,900,
and per-line blame attributes about 327 of those to him. That is not a small
contribution — it is the ignition system — but the menu, purchase, fleet and
tech tree systems documented below were built here.

## License

**AGPL-3.0**, inherited from the upstream project. See `LICENSE.txt`.

This matters in practice: if you distribute a built `Dreadnought.dll` to anyone,
the AGPL requires you to make the corresponding source available to them under
the same license. Forking and publishing your source — as this repo does — is
the intended path.

---

## What works

Verified in game, not just in logs:

| Feature | State |
|---|---|
| Reaching the hangar offline | works |
| Ship research / purchase | works, updates live |
| Owned ships persist across sessions | works (`dread_fleet_save.json`) |
| Fleet: add ship | works, refreshes live |
| Fleet: remove ship | works |
| Game mode list (12 modes) | populated |
| Map table (22 MP + 25 PVE) | recovered |
| Per-ship tech trees | works — counts, module rails, real stats |

## Known issues

- **Getting into a match does not work yet.** Pressing PLAY enters a
  matchmaking queue that can never be answered. A local-travel path is
  implemented but unverified.
- Ship preview art renders as white boxes for many ships.
- Some costs display as `-1`.
- Battle bonus values are garbage (the source array is empty).
- Item mark suffixes show `N` (Roman zero) where a mark could not be resolved.
- Tech tree rails show one item each rather than a full researchable tree — the
  real tree data was server-side and is gone. See
  [How this works](#how-this-works).
- In-match: much of the HUD is missing and projectile VFX do not render.
- Fleets are auto-filled at startup rather than restored from the save.

---

## Building

**Requirements**
- Visual Studio 2022 or newer with the C++ desktop workload
- Windows 10/11 x64

**Steps**
1. Clone this repo.
2. Open `Dreadnought/Dreadnought.sln`.
3. Build the `Dreadnought` project, configuration **Release**, platform **x64**.

Output: `Dreadnought/x64/Release/Dreadnought.dll`

Command line equivalent:

```powershell
msbuild Dreadnought\Dreadnought.sln /p:Configuration=Release /p:Platform=x64 /t:Dreadnought
```

The solution also contains `DreadnoughtLauncher` and `wer` projects, inherited
from upstream. `wer.dll` is the injection shim — the game loads it on startup,
and it loads `Dreadnought.dll`.

## Installing

1. Install Dreadnought from Steam and let it fully update.
2. In Steam: right-click Dreadnought → **Manage → Browse Local Files**.
3. Navigate to `DreadGame\DreadGame\Binaries\Win64`.
4. Copy in `Dreadnought.dll` and `wer.dll`.
5. Launch `DreadGame-Win64-Shipping.exe` directly from that folder.
6. Press **OK** if an EasyAntiCheat popup appears.

Launching the shipping executable directly is what this fork's development and
testing has used, and the shim loads reliably that way. Upstream instead
replaces `DreadnoughtLauncher.exe` in the game root; that path is untested here.

### Confirming it loaded

The mod writes `dread_mod_log.txt` next to the executable. If that file appears
and contains `[HOOK]` lines, the DLL is live. If there is no log file at all,
the shim did not load.

---

## How this works

Dreadnought's backend was two services: a small HTTPS REST gateway, and a raw
TCP service called **Mmogbrain** that owned ship purchases, loadouts, fleets and
matchmaking. Mmogbrain is gone, and no packet captures of it are known to exist,
so the protocol would have to be reverse engineered from nothing.

Almost every offline bug in this game has the same shape. The client validates
an action locally, fires a request, and then **changes nothing** until a server
callback that can never arrive. So a fix is usually three parts:

1. make the local validation pass,
2. perform the state change the server callback would have performed,
3. refresh the UI — which itself usually takes more than one call, because the
   widgets cache their state.

The good news is that a lot of the authored data survived inside the game's own
packages: the game mode list, the full map table with real travel paths, the
fleet tier rules, and the per-ship module lists. Much of the work here is
finding that data and wiring it back into the systems that used to be fed by the
server.

What did **not** survive is anything the server generated rather than shipped —
most visibly the real per-ship tech trees, which arrived as a JSON payload. What
you see instead is reconstructed from each ship's default loadout, which is why
the rails hold one item each.

---

## What is deliberately not in this repo

No game content, ever. Dreadnought is copyrighted by its owners, and none of
the following is redistributable:

- `.pak` files or anything extracted from them (`.uasset`, `.umap`, `.locres`)
- `DreadGame-Win64-Shipping.exe` or any other game binary
- Ghidra projects or databases containing a disassembly of the game executable
- Third-party SDKs with their own redistribution terms (e.g. Steamworks)

If you are working on this, keep your extraction output, Ghidra project and
tooling **outside** the repo directory so they cannot be committed by accident.

**On the `SDK/` headers:** these are ~2,000 generated headers describing the
game's class layout, produced from its own reflection data by CorrM's generator.
They contain no game assets or engine code. They are inherited from the upstream
repo, which has published them from the start.

---

## Contributing

If you pick this up, two things are worth knowing.

**Don't trust the logs alone.** This codebase has been worked on by a lot of
different hands and there is a long history of changes that logged success while
the screen stayed broken. A log line saying a hook fired is not evidence the
feature works. Confirm it on screen.

**Don't guess at addresses.** Native `UFunction`s exist twice: an autogenerated
exec thunk, and the real C++ body. A name lookup gives you the *thunk*, and
hooking a thunk with a plain member-function signature will quietly corrupt
`UObject` headers and the Blueprint bytecode stream. Decompile first and confirm
which one you have. Hooking by name (swapping `UFunction::Func`) is usually the
safer option, since it catches Blueprint calls however they are dispatched.
