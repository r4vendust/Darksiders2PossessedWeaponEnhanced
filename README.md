# Darksiders 2 - Possessed Weapon Enhanced

Extends the INVALID ID fix for Possessed Weapons with a per-weapon prestige
system that pushes Critical Damage past the vanilla hardcap — with no leaks
between weapons.

**Version:** 1.0
**Author:** r4vendust
**Target:** Darksiders II Deathinitive Edition (Epic Games tested)
**Requires:** [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (x64)

---

## Features

- Fix INVALID ID (blocks the 7th attribute slot)
- Per-weapon prestige with persistence across sessions
- Prestige boost visible in the weapon tooltip
- Prestige boost applied in **combat damage**, per weapon
- No CD leak between primary and secondary
- Non-weapon items (armor, potions) never affect the boost
- Automatic detection — no manual configuration or hover required
- Configurable bonus per level and absolute cap

---

## Install

1. Download the latest release and grab `Darksiders2PossessedWeaponEnhanced.asi`.
2. Get **Ultimate ASI Loader x64** and rename the DLL to `dxgi.dll`.
3. Place both files in the **game root** (next to `Darksiders2.exe`):

[TRES_CRASES]
Darksiders2/
├── Darksiders2.exe
├── dxgi.dll                                ← ASI Loader
└── Darksiders2PossessedWeaponEnhanced.asi
[TRES_CRASES]

4. Run the game once. Two config files will auto-generate:
   - `Darksiders2PossessedWeaponEnhanced.ini` — mod settings
   - `Darksiders2PrestigeData.ini` — prestige data (do not edit)

### Verify install

Launch with [DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview)
running. You should see:

[TRES_CRASES]
[Config] Loaded - Bonus=0.2872 | MaxCD=500%
[Prestige] Loaded N baseline records
[EquipHook] Installed.
[Enhanced] v1.0 loaded.
[TRES_CRASES]

If `[EquipHook] Pattern mismatch` appears, the mod doesn't recognize your
game binary. See **Compatibility**.

---

## Configuration

### `Darksiders2PossessedWeaponEnhanced.ini`

[TRES_CRASES]ini
# Bonus per prestige level (0.2872 = +28.72% per level)
PrestigeBonusPerLevel=0.2872

# Absolute cap for displayed CD/ACD on tooltip (500 = 500%)
MaxCDPercent=500.0
[TRES_CRASES]

| Key | Description | Range |
|---|---|---|
| `PrestigeBonusPerLevel` | Multiplier per prestige level | 0.0 – 100.0 |
| `MaxCDPercent` | Hard cap on the boosted CD value | 0.0 – 100000.0 |

Changes take effect on the next game launch.

### `Darksiders2PrestigeData.ini`

Auto-managed by the mod. Format:

[TRES_CRASES]
# Format: <raw_cd_value> <counter_at_cap>
47 84
52 102
[TRES_CRASES]

Each line maps a weapon's raw CD value to the feed counter it had when it
first hit the cap. Prestige = `current_counter - stored_baseline`. Do not
edit unless you know what you're doing.

---

## How It Works

### Prestige

Possessed Weapons have an internal counter that increments by 1 every time
you feed them an item. This counter survives saves and is stable across
sessions.

1. When the weapon is first read by the mod, its **raw CD** is cached.
2. When you feed the weapon **past the CD cap**, the mod registers a
   **baseline** — the current counter value at that moment.
3. From that point on: `prestige = current_counter - baseline`.
4. Each prestige point grants `1 + prestige × PrestigeBonusPerLevel` on top
   of the raw CD.

### Combat Boost

The mod hooks the exact instruction that the engine uses to store the
**currently equipped possessed weapon** (`mov [rdi+0x370], rbx` at
`Darksiders2.exe+0x493914`). Once the equipped weapon is known, a 60 Hz
thread reads `[container+0x370]` every frame to detect when the weapon is
unequipped, and applies the boost directly to the shared CD field
(`playerStruct + 0x890`).

Because the engine itself tells the mod which possessed weapon is equipped,
there is no ambiguity between primary and secondary, no leak, and no need
for manual configuration.

---

## Known Limitations

- **Character Stats panel** may show values one UI frame behind. The weapon
  tooltip and combat damage are always correct.
- **Epic Games** Deathinitive Edition is confirmed. **Steam** Deathinitive
  expected to work (same binary revision), untested. Classic (non-Definitive)
  is **not** supported — different binary.

---

## Compatibility

| Version | Status |
|---|---|
| Darksiders II Deathinitive Edition (Epic Games) | ✅ Tested |
| Darksiders II Deathinitive Edition (Steam) | ⚠️ Expected to work, untested |
| Darksiders II (original, non-Definitive) | ❌ Not supported |

---

## Uninstall

Delete `dxgi.dll` and `Darksiders2PossessedWeaponEnhanced.asi` from the game
root. Your save files are untouched. Optionally delete the two `.ini` files.

---

## Credits

- **r4vendust** — original [darksiders2-possessed-weapon-fix](https://github.com/r4vendust/darksiders2-possessed-weapon-fix) and this extended version
- **Akumakuja28** — Cheat Engine reference table used during reverse engineering

---

## Technical documentation

Detailed offsets, hook list, and engineering notes: [TECHNICAL.md](TECHNICAL.md)

---

## License

Free to use, modify, and redistribute. No warranty. Back up your saves
before installing any mod.