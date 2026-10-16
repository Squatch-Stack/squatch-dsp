// SPDX-License-Identifier: MIT
// Stability in a real room: does the reading flicker, restart, jump or freeze a bad value?
//
// The same notes are scored clean and "in a room", so a change can be judged on both:
//   - synthetic plucks with known truth (bench/synth.hpp: stiff string, pluck glide, pick noise),
//   - real guitar DI (Guitar-TECHS single notes, found by onset; the reference is each note's
//     settled pitch in the clean run, so the room versions are scored against the same value),
//   - room conditions built from a recording of a real room's ambience (ROOM_WAV): its noise
//     looped under the note at a set SNR, a small-room reverb, and a laptop-microphone high-pass
//     that removes most of a low string's fundamental.
// Recordings with no reference (a live session) are scored on the measures that need none.
//
//   eval_stability [--room room.wav] [--session take.wav ...] [--di di.wav ...]
//
// Every measure is per note, over the note's own window; the tables report medians and totals.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "onsets.hpp"
#include "squatch/tuner/tuner.hpp"
#include "stability.hpp"
#include "synth.hpp"
#include "wav.hpp"

using namespace bench;

namespace {

struct Args {
  std::string room;
  std::vector<std::string> sessions, di;
};

Args parse(int argc, char** argv) {
  Args a;
  std::vector<std::string>* list = nullptr;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    if (s == "--room" && i + 1 < argc) a.room = argv[++i];
    else if (s == "--session") list = &a.sessions;
    else if (s == "--di") list = &a.di;
    else if (list) list->push_back(s);
  }
  return a;
}

std::vector<Case> syntheticCases(float fs) {
  const double strings[] = {82.4069, 110.0, 146.832, 195.998, 246.942, 329.628};
  const double glides[] = {0.0, 4.0, 10.0};
  std::vector<Case> out;
  std::mt19937 rng(7);
  for (double f0 : strings)
    for (double g : glides) {
      ToneSpec s;
      s.f0 = f0 * std::exp2(((static_cast<int>(f0) % 7) - 3) / 1200.0);  // a few cents off, like a real string
      s.glideCents = g;
      s.seconds = 3.5;
      s.t60 = f0 < 120 ? 4.0 : 2.5;
      s.inharmonicity = f0 < 120 ? 0.0002 : 0.00008;
      s.snrDb = 50;
      Case c;
      c.name = "synth " + std::to_string(static_cast<int>(f0)) + " Hz glide " + std::to_string(static_cast<int>(g));
      c.x = synthesize(s, fs, rng);
      c.fs = fs;
      c.notes.push_back({s.preroll, s.preroll + s.seconds, s.f0});
      out.push_back(std::move(c));
    }
  return out;
}

std::vector<Case> diCases(const std::vector<std::string>& paths, size_t maxNotes) {
  std::vector<Case> out;
  for (const auto& p : paths) {
    Audio a;
    if (!readWav(p, a)) { std::fprintf(stderr, "cannot read %s\n", p.c_str()); continue; }
    const std::vector<double> on = findOnsets(a.x, a.rate, 1.5);
    Case c;
    c.name = p.substr(p.find_last_of('/') + 1);
    c.x = std::move(a.x);
    c.fs = a.rate;
    for (size_t i = 0; i < on.size() && c.notes.size() < maxNotes; ++i) {
      const double end = i + 1 < on.size() ? std::min(on[i + 1] - 0.05, on[i] + 3.5) : on[i] + 3.5;
      c.notes.push_back({on[i], end, 0.0});  // reference filled from the clean run
    }
    out.push_back(std::move(c));
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = parse(argc, argv);
  const float fs = 48000.0f;
  std::vector<float> room;
  if (!args.room.empty()) {
    Audio r;
    if (readWav(args.room, r)) room = firstSeconds(r, 10.0, fs);
  }
  std::vector<Case> cases = syntheticCases(fs);
  const char* maxEnv = std::getenv("DI_NOTES");
  for (auto& c : diCases(args.di, maxEnv ? static_cast<size_t>(std::atoi(maxEnv)) : 40)) cases.push_back(std::move(c));
  fillReferences(cases);  // real notes: settled pitch of the clean run

  std::printf("# Stability: clean and in a room\n\n");
  std::printf("Room noise from `%s`. Each row sums or takes the median over every note in the set.\n\n",
              args.room.empty() ? "(none: room rows skipped)" : args.room.c_str());
  printHeader();
  for (const Condition& cond : conditions(!room.empty()))
    for (const char* set : {"synth", "DI"}) printRow(set, cond, scoreSet(cases, set, cond, room));

  for (const auto& p : args.sessions) {
    Audio a;
    if (!readWav(p, a)) continue;
    printSession(p, a);
  }
  return 0;
}
