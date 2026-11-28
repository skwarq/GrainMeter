// SPDX-License-Identifier: MIT
#ifndef GM_PC_SIM_PROBE_H
#define GM_PC_SIM_PROBE_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "onewire/onewire.h"
#include "sim/sim_clock.h"
#include "util/json.h"

namespace gmpc {

// Bit-level simulation of the STW-100 multipoint probe: N DS18B20-like
// devices on one wired-AND bus. It answers reset/presence, Search ROM,
// Match/Skip ROM, Convert T (busy until 750 ms of simulated time passed) and
// Read Scratchpad, so the shared search/CRC/decoding code runs unchanged.
class SimProbe : public gm::OneWireBus {
 public:
  struct Device {
    gm::Rom rom;
    bool present;
    int16_t raw;     // last converted value, 1/16 degC
    double tempC;    // "physical" temperature
    bool converted;  // false -> scratchpad holds 85.0 power-on value
    int crcFaults;   // corrupt next N scratchpad reads (-1 = always)
    bool fixedTemp;
    uint8_t th = 0x4B;
    uint8_t tl = 0x46;
  };

  SimProbe(SimClock& clock, int sensors);

  // gm::OneWireBus
  bool reset() override;
  void writeBit(bool bit) override;
  bool readBit() override;
  void sleepMs(uint32_t ms) override;

  // Fault injection (index = 1-based device index on the probe, as listed
  // in the "sim.probe.devices" diagnostics; not the measuring point).
  bool injectCrc(int point, int count);
  bool setPresent(int point, bool present);
  bool setTemperature(int point, double tempC);  // fixed value, e.g. 0.0
  bool setPointNumber(int index, uint8_t th, uint8_t tl);
  void setUnsupportedDevice(bool on);  // adds a DS18S20 (0x10)
  void setShorted(bool on);            // DATA stuck low

  size_t deviceCount();
  void writeJson(gm::JsonWriter& w);
  static gm::Rom makeRom(uint8_t family, uint32_t serial);

 private:
  enum class Mode {
    Idle,
    RomCmd,
    Search,
    Match,
    FuncCmd,
    ReadScratch,
    Converting
  };

  void onByte(uint8_t value);
  void updateTemperatures();
  void buildScratchpad(Device& d, uint8_t sp[9]);
  bool romBit(const Device& d, int bit) const;

  std::recursive_mutex mu_;
  SimClock& clock_;
  std::vector<Device> devices_;
  std::vector<bool> active_;
  Mode mode_;
  int bitCount_;
  uint8_t byte_;
  int searchBit_;
  int searchPhase_;
  int matchBit_;
  uint64_t convertStartMs_;
  std::vector<std::vector<uint8_t>> scratch_;  // per device, for current read
  int readBitPos_;
  bool shorted_;
  bool unsupported_;
};

}  // namespace gmpc

#endif
