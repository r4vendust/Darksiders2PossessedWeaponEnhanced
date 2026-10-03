```markdown
# Changelog

## [1.0.0] — 2026-10-03

### Added
- Per-weapon prestige system with persistent baselines
- Slot-aware boost: each weapon applies its own CD in combat
- Equip hook at `+0x493914` capturing the engine's own "equipped possessed
  weapon" pointer (`container + 0x370`)
- Combat boost applied via the shared `playerStruct + 0x890` field, driven
  by the live container value

### Fixed
- CD leak between primary and secondary weapons
- Non-weapon items (armor, potions) accidentally removing the boost
- Boost not applying in a fresh session until hover

### Known Limitations
- Character Stats panel may show values one UI frame behind
- Tested only on the Epic Games Deathinitive Edition

### Development History (abridged)
- v100 — initial prototype (INVALID ID fix + first prestige)
- v101–v105 — added slot-aware boost and iterated through crashes and leaks
- v106–v110 — diagnostic builds to map the UI hook and transient pointers
- v111–v131 — heuristic-based slot association (working but with edge cases)
- v132–v133 — reverse-engineered the real equip function via Cheat Engine
- v134 — replaced all heuristics with the equip hook
- v135 — live field read to handle item equips
- v1.0 — final release, clean build