// SPDX-License-Identifier: MIT
// hosts/common/hold.hpp and the display's rounding (tuner_frames.hpp roundTenth) driven from stdin,
// for hold_parity.mjs to compare with hold.js and the page's formatCents.
//   add <samples> <rate> <valid> <midi> <cents> <unc>   a reading at that audio position
//   held <midi>                         prints the held cents (%.17g: exact) or "nan"
//   shown <midi>                        prints the held number as a display row shows it, or "--.-"
//   holding <samples>                   prints 1 if a let-go note is still up at that position, else 0
//   round <cents>                       prints <cents> as a display row shows it
//   reset
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hosts/common/hold.hpp"
#include "hosts/common/tuner_frames.hpp"

// One decimal, as formatFrameCsv prints the number.
static void printShown(double c) {
  if (c != c) std::printf("--.-\n");
  else std::printf("%.1f\n", static_cast<double>(squatch::host::roundTenth(c)));
}

// held / shown <midi>; false at the end of the input.
static bool query(const char* op, const squatch::host::Hold& h) {
  int midi = 0;
  if (std::scanf("%d", &midi) != 1) return false;
  const double c = h.held(midi);
  if (op[0] == 's') printShown(c);
  else if (c != c) std::printf("nan\n");
  else std::printf("%.17g\n", c);
  return true;
}

// add <samples> <rate> <valid> <midi> <cents> <unc>
static void add(squatch::host::Hold& h) {
  unsigned long long samples;
  double rate;
  int valid, midi;
  float cents, unc;
  if (std::scanf("%llu %lf %d %d %f %f", &samples, &rate, &valid, &midi, &cents, &unc) == 6)
    h.add({midi, cents, unc, valid != 0, samples, rate});
}

// One command; false at the end of the input.
static bool command(const char* op, squatch::host::Hold& h) {
  if (!std::strcmp(op, "add")) {
    add(h);
  } else if (!std::strcmp(op, "holding")) {
    unsigned long long samples;
    if (std::scanf("%llu", &samples) != 1) return false;
    std::printf("%d\n", h.holding(samples) ? 1 : 0);
  } else if (!std::strcmp(op, "held") || !std::strcmp(op, "shown")) {
    return query(op, h);
  } else if (!std::strcmp(op, "round")) {
    char text[64];
    if (std::scanf("%63s", text) != 1) return false;
    printShown(std::strtod(text, nullptr));
  } else if (!std::strcmp(op, "reset")) {
    h.reset();
  }
  return true;
}

int main() {
  squatch::host::Hold h;
  char op[16];
  while (std::scanf("%15s", op) == 1 && command(op, h)) {
  }
  return 0;
}
