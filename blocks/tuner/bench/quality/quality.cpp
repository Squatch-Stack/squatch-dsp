// SPDX-License-Identifier: MIT
// Tuner quality run: every case through the display path the apps use, with its truth.
//
//   tuner_quality --out DIR [--corpus DIR] [--sets synthetic,regression,egfxset,guitar-techs,owner,
//                                                  guitarset,nsynth,idmt]
//                 [--blocks 16,64] [--platform mac] [--threads N] [--limit N] [--only TEXT]
//                 [--pack DIR --pack-list FILE] [--frames-only 1]
//   tuner_quality --replay take.wav --at device-frames.csv     (frames on stdout)
//
// --corpus is the DI corpus folder (public/egfxset/Clean, public/guitar-techs,
// owner/di-20260924, public/guitarset, public/nsynth, public/idmt-smt-guitar/dataset1); the
// synthetic and regression sets need nothing. Writes, in --out:
//   cases.jsonl                                  one line per case: id, dataset, file, segments
//   truth-<dataset>.csv                          case,t_ms,truth_hz,truth_midi,steady,segment
//   frames-<platform>-b<block>-<dataset>.csv     case + the frame format in io.hpp
// --replay reads the display at the audio positions of another platform's frame log (its t_ms,
// in 64-frame blocks cut to land on each), for a line-by-line comparison with that platform.
// --frames-only 1 skips the offline references and writes frames only: for another platform,
// scored against the reference run's cases and truth. score.py turns these into the scorecard. --pack writes the listed cases for the phone as
// 48 kHz float WAVs, each with its truth and this machine's frames (see score.py pack). It refuses
// internal-only sets (IDMT-SMT-Guitar is CC BY-NC-ND: nothing derived from it may leave this machine).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "guitarset_cases.hpp"
#include "idmt_cases.hpp"
#include "io.hpp"
#include "nsynth_cases.hpp"
#include "owner_cases.hpp"
#include "real_cases.hpp"
#include "regression_cases.hpp"
#include "synth_cases.hpp"

using namespace quality;

namespace {

struct Args {
  std::string out, corpus, platform = "mac", only, pack, packList, replay, at;
  std::set<std::string> sets = {"synthetic", "regression", "egfxset", "guitar-techs", "owner", "guitarset", "nsynth", "idmt"};
  std::vector<int> blocks = {16, 64};
  int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  size_t limit = 0;
  bool framesOnly = false;
};

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> v;
  std::stringstream ss(s);
  for (std::string t; std::getline(ss, t, ',');) v.push_back(t);
  return v;
}

bool apply(Args& a, const std::string& k, const std::string& v) {
  using Set = void (*)(Args&, const std::string&);
  static const std::map<std::string, Set> options = {
      {"--out", [](Args& x, const std::string& s) { x.out = s; }},
      {"--corpus", [](Args& x, const std::string& s) { x.corpus = s; }},
      {"--platform", [](Args& x, const std::string& s) { x.platform = s; }},
      {"--only", [](Args& x, const std::string& s) { x.only = s; }},
      {"--pack", [](Args& x, const std::string& s) { x.pack = s; }},
      {"--pack-list", [](Args& x, const std::string& s) { x.packList = s; }},
      {"--replay", [](Args& x, const std::string& s) { x.replay = s; }},
      {"--at", [](Args& x, const std::string& s) { x.at = s; }},
      {"--threads", [](Args& x, const std::string& s) { x.threads = std::max(1, std::atoi(s.c_str())); }},
      {"--limit", [](Args& x, const std::string& s) { x.limit = static_cast<size_t>(std::atol(s.c_str())); }},
      {"--frames-only", [](Args& x, const std::string& s) { x.framesOnly = s == "1"; }},
      {"--sets", [](Args& x, const std::string& s) { auto l = split(s); x.sets = {l.begin(), l.end()}; }},
      {"--blocks", [](Args& x, const std::string& s) {
         x.blocks.clear();
         for (auto& b : split(s)) x.blocks.push_back(std::atoi(b.c_str()));
       }},
  };
  const auto it = options.find(k);
  if (it == options.end()) return false;
  it->second(a, v);
  return true;
}

void keep(std::vector<Case>& all, std::vector<Case>&& some, const Args& a) {
  size_t n = 0;
  for (Case& c : some) {
    if (!a.only.empty() && c.id.find(a.only) == std::string::npos) continue;
    if (a.limit && n >= a.limit) break;
    all.push_back(std::move(c));
    ++n;
  }
}

std::vector<Case> allCases(const Args& a) {
  std::vector<Case> all;
  if (a.sets.count("synthetic")) keep(all, synthCases(), a);
  if (a.sets.count("regression")) keep(all, regressionCases(), a);
  if (a.corpus.empty()) return all;
  if (a.sets.count("egfxset")) keep(all, egfxCases(a.corpus + "/public/egfxset/Clean"), a);
  if (a.sets.count("guitar-techs")) keep(all, guitarTechsCases(a.corpus + "/public/guitar-techs"), a);
  if (a.sets.count("owner")) keep(all, ownerCases(a.corpus + "/owner/di-20260924"), a);
  if (a.sets.count("guitarset")) keep(all, guitarsetCases(a.corpus + "/public/guitarset"), a);
  if (a.sets.count("nsynth")) keep(all, nsynthCases(a.corpus + "/public/nsynth"), a);
  if (a.sets.count("idmt")) keep(all, idmtCases(a.corpus + "/public/idmt-smt-guitar/dataset1"), a);
  return all;
}

/// Output files, opened on first use with their header; one lock for all.
class Sink {
 public:
  explicit Sink(std::string dir) : dir_(std::move(dir)) {}
  ~Sink() {
    for (auto& kv : files_) std::fclose(kv.second);
  }
  void write(const std::string& name, const char* header, const std::string& text) {
    std::lock_guard<std::mutex> g(m_);
    FILE*& f = files_[name];
    if (!f) {
      f = std::fopen((dir_ + "/" + name).c_str(), "w");
      if (header) std::fprintf(f, "%s\n", header);
    }
    std::fwrite(text.data(), 1, text.size(), f);
  }

 private:
  std::string dir_;
  std::mutex m_;
  std::map<std::string, FILE*> files_;
};

std::vector<double> times(const std::vector<Frame>& f) {
  std::vector<double> t;
  for (const Frame& x : f) t.push_back(x.tMs);
  return t;
}

void runCase(Case& c, const Args& a, Sink& sink) {
  if (!c.load || !c.load(c)) return;
  const std::string prefix = c.id + ",";
  for (size_t b = 0; b < a.blocks.size(); ++b) {
    const std::vector<Frame> f = runDisplayPath(c.x, {c.fs, a.blocks[b], 30.0, 440.0f});
    const std::string name = "frames-" + a.platform + "-b" + std::to_string(a.blocks[b]) + "-" + c.dataset + ".csv";
    sink.write(name, (std::string("case,") + kFrameHeader).c_str(), framesCsv(f, prefix));
    if (b == 0 && !a.framesOnly) sink.write("truth-" + c.dataset + ".csv", (std::string("case,") + kTruthHeader).c_str(), truthCsv(truthSeries(c, times(f)), prefix));
  }
  if (!a.framesOnly) sink.write("cases.jsonl", nullptr, caseJson(c, a.corpus) + "\n");
}

/// The phone pack: 48 kHz float WAV, its truth and this machine's frames (16-frame blocks).
void packCase(Case& c, const Args& a) {
  if (internalOnly(c.dataset)) {
    std::fprintf(stderr, "%s: not packed (%s is licensed for internal testing only)\n", c.id.c_str(), c.dataset.c_str());
    return;
  }
  if (!c.load || !c.load(c)) return;
  if (c.fs != 48000.0) {
    c.x = bench::resample(c.x, c.fs / 48000.0);
    c.fs = 48000.0;
  }
  const std::string base = a.pack + "/" + c.id;
  writeFloatWav(base + ".wav", c.x, 48000);
  const std::vector<Frame> f = runDisplayPath(c.x, {48000.0, 16, 30.0, 440.0f});
  auto put = [](const std::string& path, const std::string& head, const std::string& body) {
    FILE* o = std::fopen(path.c_str(), "w");
    if (!o) return;
    std::fprintf(o, "%s\n%s", head.c_str(), body.c_str());
    std::fclose(o);
  };
  put(base + ".truth.csv", kTruthHeader, truthCsv(truthSeries(c, times(f)), ""));
  put(base + ".frames-" + a.platform + ".csv", kFrameHeader, framesCsv(f, ""));
  put(base + ".case.json", "", caseJson(c, a.corpus));
}

void runAll(std::vector<Case>& cases, const Args& a, bool pack) {
  Sink sink(a.out);
  std::atomic<size_t> next{0}, done{0};
  auto work = [&]() {
    for (size_t i; (i = next++) < cases.size();) {
      if (pack) packCase(cases[i], a);
      else runCase(cases[i], a, sink);
      cases[i].x = {};
      cases[i].x.shrink_to_fit();
      const size_t d = ++done;
      if (d % 50 == 0 || d == cases.size()) std::fprintf(stderr, "  %zu / %zu cases\n", d, cases.size());
    }
  };
  std::vector<std::thread> pool;
  for (int t = 0; t < a.threads; ++t) pool.emplace_back(work);
  for (std::thread& t : pool) t.join();
}

std::set<std::string> readList(const std::string& path) {
  std::set<std::string> ids;
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return ids;
  char line[512];
  while (std::fgets(line, sizeof line, f)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (!s.empty() && s[0] != '#') ids.insert(s);
  }
  std::fclose(f);
  return ids;
}

/// --replay: frames at the positions of a device's log (first column t_ms), on stdout.
int replay(const Args& a) {
  bench::Audio w;
  if (!bench::readWav(a.replay, w)) return std::fprintf(stderr, "cannot read %s\n", a.replay.c_str()), 2;
  std::vector<size_t> at;
  FILE* f = std::fopen(a.at.c_str(), "r");
  char line[256];
  while (f && std::fgets(line, sizeof line, f)) {
    char* end = nullptr;
    const double ms = std::strtod(line, &end);
    if (end != line) at.push_back(static_cast<size_t>(std::llround(ms * w.rate / 1000.0)));
  }
  if (f) std::fclose(f);
  // Leading rows later than the row after them were read before the file restarted the tuner (the
  // phone logs the last live-input snapshot first): they are read before any of the file, at 0.
  std::vector<size_t> logged = at;
  for (size_t i = 0; i + 1 < at.size() && logged[i] > logged[i + 1]; ++i) at[i] = 0;
  std::vector<Frame> frames = runDisplayPathAt(w.x, {w.rate, 64, 30.0, 440.0f}, at);
  for (size_t i = 0; i < frames.size(); ++i) frames[i].tMs = 1000.0 * static_cast<double>(logged[i]) / w.rate;
  std::printf("%s\n%s", kFrameHeader, framesCsv(frames, "").c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i + 1 < argc; i += 2)
    if (!apply(a, argv[i], argv[i + 1])) return std::fprintf(stderr, "unknown option %s\n", argv[i]), 64;
  if (!a.replay.empty()) return replay(a);
  const bool pack = !a.pack.empty();
  fitReferences() = !a.framesOnly;
  if (a.out.empty() && !pack) return std::fprintf(stderr, "usage: tuner_quality --out DIR [options] (see the source)\n"), 64;
  std::vector<Case> cases = allCases(a);
  if (pack) {
    const std::set<std::string> ids = readList(a.packList);
    std::vector<Case> chosen;
    for (Case& c : cases)
      if (ids.count(c.id)) chosen.push_back(std::move(c));
    cases = std::move(chosen);
    std::filesystem::create_directories(a.pack);
  } else {
    std::filesystem::create_directories(a.out);
  }
  std::fprintf(stderr, "tuner_quality: %zu cases, %d threads\n", cases.size(), a.threads);
  runAll(cases, a, pack);
  return 0;
}
