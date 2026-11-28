# TH/TL automap – plan implementacji

> For agentic workers: execute task by task, TDD. Do NOT commit (the user commits on request).

**Goal:** GrainMeter assigns DS18B20 sensors to points automatically from the point number stored in the
sensor's TH/TL scratchpad bytes (STW-100 convention TH = TL = n).

**Architecture:** pure rule in `RomMap` (unit tested), fed by `GrainMeter::measure()` and `mapAuto()` with the
raw scratchpads they already read; persisted through the existing `romMapSave`. PC simulator gets settable TH/TL.

**Tech stack:** C++ (core shared by ESP-IDF firmware and PC build), CMake, GoogleTest (`tests/`), CTest.

**Spec:** `docs/plans/thtl-automap.md` (read it first).

## Global constraints

- Read only: GrainMeter never writes TH/TL (no Write/Copy Scratchpad commands).
- Point number valid only when the scratchpad CRC is valid (decode status Ok, PowerOnValue or OutOfRange) and
  TH == TL and 1 <= n <= kMaxPoints (8). Factory 0x4B/0x46 = no number.
- A point held by a ROM that is present on the bus in this scan is never taken away by TH/TL.
- Two present unmapped sensors with the same number: neither is assigned; `recordError(...)` once per number.
- No extra 1-Wire traffic in `measure()` (the scratchpad is already read there).
- Style: existing code (`.clang-format`), SPDX header, no exceptions, English comments; firmware build must still pass.

## Review focus

1. Sensor already mapped (manually) to point 3 but its TH/TL says 5 → mapping unchanged (manual wins).
2. Old owner of point n absent, new sensor with number n present → point moves, old ROM entry removed, height of
   the point reset to -1 (unknown) because it is a different physical sensor.
3. CRC error / Missing / BusError reading of an unmapped sensor → no assignment (no number trusted).
4. Same unmapped sensor seen in several measurements → assigned once, `romMapSave` called only when the map changed.
5. Storage write failure → map in RAM unchanged, `recordError("map: storage write failed")` (like `mapAuto`).

---

### Task 1: rule in RomMap

**Files:** modify `core/data/rommap.h`, `core/data/rommap.cpp`; test `tests/unit/test_rommap.cpp`.

**Produces:**
```cpp
struct PointHint { Rom rom; uint8_t point; };          // in namespace gm, rommap.h
// Returns number of new assignments; *conflicts (optional) = numbers claimed by >1 hint.
size_t RomMap::assignByHints(const PointHint* hints, size_t n,
                             const Rom* present, size_t nPresent,
                             uint8_t* conflictPoints, size_t conflictCap, size_t* conflicts);
uint8_t pointFromScratchpad(const uint8_t sp[kScratchpadLen]);  // 0 = no number; in core/onewire/ds18b20.h/.cpp,
                                                                // checks CRC itself, TH==TL, 1..kMaxPoints
```
Hints for ROMs that are already mapped are ignored. Unsupported family / bad ROM CRC ignored.

- [ ] Tests (GoogleTest, `TEST(RomMap, ...)` / `TEST(Ds18b20, ...)`):
  `HintAssignsFreePoint`, `HintKeepsPointOfPresentOwner`, `HintTakesPointOfAbsentOwnerAndResetsHeight`,
  `HintIgnoredForAlreadyMappedRom`, `DuplicateHintsAssignNeitherAndReportConflict`,
  `HintOutOfRangeIgnored` (point 0 and 9), `PointFromScratchpadFactoryBytesIsZero` (4B/46 → 0),
  `PointFromScratchpadValid` (TH=TL=3, valid CRC → 3), `PointFromScratchpadThNotEqualTl` (→ 0),
  `PointFromScratchpadBadCrc` (→ 0).
- [ ] Run `cmake --build build -j && ctest --test-dir build -R "RomMap|Ds18b20" --output-on-failure` → new tests fail.
- [ ] Implement; rerun → pass.

### Task 2: use it in measure() and mapAuto()

**Files:** modify `core/app/grainmeter.cpp` (and `.h` if a helper is added); test
`tests/integration/test_scenario.cpp` or a new `tests/integration/test_thtl_automap.cpp` (register in `tests/CMakeLists.txt`).

**Consumes:** Task 1. **Also:** `pc/sim/sim_probe.{h,cpp}`: `bool setPointNumber(int index, uint8_t th, uint8_t tl);`
(1-based device index like `setTemperature`; scratchpad bytes 2/3 come from the device instead of the fixed
0x4B/0x46, default stays 0x4B/0x46). Optionally a sim CLI hook next to the existing fault injection, if one exists.

- `measure()`: pass a raw buffer to `readSensor` for unmapped readings; after the read loop build hints with
  `pointFromScratchpad`, call `assignByHints` on a copy of `map_` with the present ROMs from `scan_`; if anything was
  assigned: `romMapSave`, `map_ = next`, set `r.point` of those readings (so this sample already carries them),
  log `GM_LOGI(kTag, "map: ROM %s -> point %u (TH/TL)", ...)`; conflicts → `recordError("map: TH/TL point %u claimed by several sensors", p)`.
- `mapAuto()`: for unmapped present supported ROMs read the scratchpad (Match ROM + Read Scratchpad, no conversion;
  reuse `readSensor` with raw output and ignore the temperature), apply `assignByHints`, then the existing
  `autoAssign`; the return value counts both.
- [ ] Tests: `ThTlAutomap.MeasureAssignsNumberedSensors` (sim with 8 sensors, TH/TL set to 8..1 in reverse ROM order →
  after one `measure(true)` every point p holds the sensor numbered p), `ThTlAutomap.ReplacementTakesPoint`
  (map sensor numbered 2, `setPresent(...,false)`, add/enable another device numbered 2 → point 2 moves),
  `ThTlAutomap.MixedFallsBackToRomOrder` (half numbered, half factory → numbered ones on their points after
  `measure`, `mapAuto()` fills the rest in ROM order), `ThTlAutomap.ManualMappingWins`.
- [ ] Run the full suite: `cmake --build build -j && ctest --test-dir build --output-on-failure` → all pass.

### Task 3: verification

- [ ] `make check` (format, tests, valgrind, firmware) or at least: `make format-check`, the full ctest run, and
  `scripts/build-firmware.sh build` (ESP-IDF in Docker) → no new warnings.
- [ ] Short report: files changed, test results, anything left open.
