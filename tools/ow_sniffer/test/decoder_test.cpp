// SPDX-License-Identifier: MIT
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "dns_reply.h"
#include "ow_decoder.h"

namespace {
bool collect(void* context, const char* line) {
  static_cast<std::vector<std::string>*>(context)->emplace_back(line);
  return true;
}

bool compare(const std::vector<std::string>& actual,
             const std::vector<std::string>& expected) {
  bool ok = actual.size() == expected.size();
  if (!ok)
    std::cerr << "line count: " << actual.size() << " expected "
              << expected.size() << '\n';
  for (size_t i = 0; i < actual.size(); ++i) {
    std::cout << actual[i];
    if (i >= expected.size()) continue;
    double at = 0, want = 0;
    const auto a = actual[i].find("s ");
    const auto e = expected[i].find("s ");
    if (std::sscanf(actual[i].c_str(), "%lf", &at) != 1 ||
        std::sscanf(expected[i].c_str(), "%lf", &want) != 1 ||
        std::abs(at - want) > 0.000101 ||
        actual[i].substr(a) != expected[i].substr(e)) {
      std::cerr << "EXPECTED: " << expected[i];
      ok = false;
    }
  }
  return ok;
}

void byte(ow::Decoder& d, uint8_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    const double low = value & (1u << i) ? 19 : 80;
    d.feed({0, low});
    d.feed({1, 100 - low});
  }
}

bool synthetic() {
  std::vector<std::string> lines;
  ow::Decoder d(collect, &lines);
  // A sub-3us high spike inside reset and low spike inside idle are merged.
  d.feed({0, 250});
  d.feed({1, 0.5});
  d.feed({0, 249.5});
  d.feed({1, 29});
  d.feed({0, 0.5});
  d.feed({1, 0.5});
  d.feed({0, 117});
  d.feed({1, 1000});
  byte(d, 0x55);
  const uint8_t rom[] = {0x10, 1, 2, 3, 4, 5, 6, 7};
  for (uint8_t v : rom) byte(d, v);
  byte(d, 0xbe);
  uint8_t scratch[9] = {0xfd, 0xff, 0x4b, 0x46, 0xff, 0xff, 0x0c, 0x10, 0};
  scratch[8] = ow::crc8(scratch, 8);
  for (uint8_t v : scratch) byte(d, v);
  d.feed({1, 8000000000.0});  // longer than uint32_t microseconds
  d.feed({0, 490});
  d.feed({1, 100});
  byte(d, 0xcc);
  byte(d, 0xbe);
  scratch[0] = 0xff;
  scratch[1] = 0xff;
  for (uint8_t v : scratch) byte(d, v);
  d.feed({0, 490});
  d.feed({1, 100});
  if (!d.finish() || lines.size() != 2 ||
      lines[0].find("pres=Y(117us)") == std::string::npos ||
      lines[0].find(" T=-1.5000C crc=ok\n") == std::string::npos ||
      lines[1].find(" T=-0.0625C crc=BAD\n") == std::string::npos)
    return false;
  double time = 0;
  std::sscanf(lines[1].c_str(), "%lf", &time);
  if (time < 8000) return false;

  lines.clear();
  ow::Decoder search(collect, &lines);
  search.feed({0, 490});
  search.feed({1, 30});
  search.feed({0, 117});
  search.feed({1, 1000});
  byte(search, 0xf0);
  for (unsigned i = 0; i < 64; ++i) {
    // Read pair 1,0 deliberately differs from the direction; CRC must be BAD.
    for (bool bit : {true, false, true}) {
      search.feed({0, bit ? 19.0 : 30.0});
      search.feed({1, bit ? 81.0 : 70.0});
    }
  }
  search.feed({0, 490});
  search.feed({1, 100});
  if (!search.finish() || lines.size() != 1 ||
      lines[0].find("ROM=ff ff ff ff ff ff ff ff (192 bits) crc=BAD") ==
          std::string::npos)
    return false;

  lines.clear();
  ow::Decoder partial(collect, &lines);
  partial.feed({0, 401});
  partial.feed({1, 100});
  partial.feed({0, 24});  // threshold is inclusive for zero
  partial.feed({1, 100});
  partial.feed({0, 350});  // neither bit nor reset
  partial.feed({1, 100});
  partial.feed({0, 490});
  partial.feed({1, 100});
  if (!partial.finish() || lines.size() != 1 ||
      lines[0] != "0.0000s RST pres=N partial=1bits invalid=Y\n")
    return false;
  ow::Decoder invalid(collect, &lines);
  return !invalid.feed({2, 10}) && !invalid.feed({0, 0});
}

bool dns() {
  uint8_t query[] = {0x12, 0x34, 1,   0,   0,   1, 0, 0, 0, 0, 0,
                     0,    3,    'a', 'b', 'c', 0, 0, 1, 0, 1};
  uint8_t reply[64]{};
  uint8_t ip_bytes[] = {192, 168, 4, 1};
  uint32_t ip;
  std::memcpy(&ip, ip_bytes, 4);
  const int n =
      ow::dnsBuildReply(query, sizeof(query), ip, reply, sizeof(reply));
  if (n != static_cast<int>(sizeof(query) + 16) || reply[7] != 1 ||
      std::memcmp(reply + n - 4, ip_bytes, 4))
    return false;
  query[18] = 28;  // AAAA: empty answer, not a bogus IPv4 AAAA record
  if (ow::dnsBuildReply(query, sizeof(query), ip, reply, sizeof(reply)) !=
          sizeof(query) ||
      reply[7] != 0)
    return false;
  query[12] = 63;  // malformed label
  return ow::dnsBuildReply(query, sizeof(query), ip, reply, sizeof(reply)) == 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  std::ifstream input(argv[1]);
  if (!input) {
    std::cerr << "cannot open fixture\n";
    return 1;
  }
  std::vector<std::string> actual;
  ow::Decoder d(collect, &actual);
  std::string row;
  while (std::getline(input, row)) {
    if (row.empty() || row[0] == '#') continue;
    unsigned level;
    double duration;
    if (std::sscanf(row.c_str(), "%u,%lf", &level, &duration) != 2 ||
        level > 1 || !d.feed({static_cast<uint8_t>(level), duration})) {
      std::cerr << "invalid CSV record: " << row << '\n';
      return 1;
    }
  }
  if (!d.finish()) return 1;
  const std::vector<std::string> expected = {
      "0.0000s RST pres=Y(117us) F0 SEARCH ROM=28 d0 5b cf 05 00 00 a9 (192 "
      "bits) crc=ok\n",
      "0.0277s RST pres=Y(117us) F0 SEARCH ROM=28 6b 40 d0 05 00 00 f8 (192 "
      "bits) crc=ok\n",
      "0.0554s RST pres=Y(117us)\n",
      "0.0571s RST pres=Y(117us) F0 SEARCH ROM=28 d0 5b cf 05 00 00 a9 (192 "
      "bits) crc=ok\n",
      "0.0849s RST pres=Y(117us) F0 SEARCH ROM=28 6b 40 d0 05 00 00 f8 (192 "
      "bits) crc=ok\n",
      "0.1128s RST pres=Y(116us) 55 28 D0 5B CF 05 00 00 A9 BE 50 05 4B 46 7F "
      "FF 0C 10 T=85.0000C\n",
      "0.1287s RST pres=Y(117us) 55 28 6B 40 D0 05 00 00 F8 BE 50 05 4B 46 7F "
      "FF 0C 10 T=85.0000C\n",
      "2.2324s RST pres=Y(117us) 55 00 00 00 00 00 00 00 00 44\n",
      "2.9414s RST pres=Y(117us) 55 00 00 00 00 00 00 00 00 BE FF FF FF FF FF "
      "FF FF FF T=-0.0625C\n"};
  const bool fixture_ok = compare(actual, expected);
  const bool synthetic_ok = synthetic();
  const bool dns_ok = dns();
  if (!synthetic_ok) std::cerr << "synthetic decoder tests failed\n";
  if (!dns_ok) std::cerr << "DNS tests failed\n";
  return fixture_ok && synthetic_ok && dns_ok ? 0 : 1;
}
