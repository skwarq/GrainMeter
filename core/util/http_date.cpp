// SPDX-License-Identifier: MIT
#include "util/http_date.h"

#include <string.h>

namespace gm {

static bool digits(const char* s, int n, int* out) {
  int v = 0;
  for (int i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  *out = v;
  return true;
}

// Days from 1970-01-01 to y-m-d (proleptic Gregorian, H. Hinnant).
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

bool parseHttpDate(const char* s, uint32_t* epoch) {
  // "Www, DD Mon YYYY HH:MM:SS GMT" = 29 characters
  static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  if (s == nullptr || strlen(s) < 29 || s[3] != ',' || s[4] != ' ' ||
      s[7] != ' ' || s[11] != ' ' || s[16] != ' ' || s[19] != ':' ||
      s[22] != ':' || s[25] != ' ' || strncmp(s + 26, "GMT", 3) != 0) {
    return false;
  }
  int day, year, hh, mm, ss;
  if (!digits(s + 5, 2, &day) || !digits(s + 12, 4, &year) ||
      !digits(s + 17, 2, &hh) || !digits(s + 20, 2, &mm) ||
      !digits(s + 23, 2, &ss)) {
    return false;
  }
  int month = 0;
  for (int i = 0; i < 12; ++i) {
    if (strncmp(s + 8, kMonths + 3 * i, 3) == 0) month = i + 1;
  }
  if (month == 0 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60 ||
      year < 2020 || year > 2105) {
    return false;
  }
  const int64_t t =
      daysFromCivil(year, month, day) * 86400 + hh * 3600 + mm * 60 + ss;
  if (t <= 0 || t > 0xFFFFFFFFLL) return false;
  *epoch = static_cast<uint32_t>(t);
  return true;
}

}  // namespace gm
