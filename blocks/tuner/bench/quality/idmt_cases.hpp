// SPDX-License-Identifier: MIT
// IDMT-SMT-Guitar (Kehling et al., DAFx 2014; V2), dataset1 single notes,
// as quality cases. Nothing from it is in this repository.
//
// LICENCE: CC BY-NC-ND 4.0. Non-commercial, no derivatives. INTERNAL TESTING ONLY: never
// redistribute it, never ship or publish audio derived from it (so never in the phone pack:
// quality.cpp refuses to pack it), never commit any of it. Scores computed on it stay internal.
//
// Layout, inside the DI corpus folder:
//   public/idmt-smt-guitar/dataset1/<setup>/audio/<file>.wav        44.1 kHz mono, 2.5 s
//   public/idmt-smt-guitar/dataset1/<setup>/annotation/<file>.xml   pitch, onsetSec, offsetSec,
//                                                                   stringNumber, fretNumber
// Four setups (a guitar and a pickup), 78 notes each: six strings, frets 0-12, plucked, no
// expression. IDMT numbers strings from the low E (stringNumber 1 = E2); the case value uses
// EGFxSet's numbering (s1 = high E) so the two sets break down alike.
//
// Truth: the note is the XML pitch, from its labelled onset to its labelled offset; the pitch is
// the offline reference (reference.hpp), as for EGFxSet.
//   family  the setup, as a slug of its folder name (fender-strat-clean-neck-sc, ...)
//   value   s<string>f<fret>
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "json_lite.hpp"
#include "real_cases.hpp"

namespace quality {

/// Datasets whose licence forbids sharing anything derived from them.
inline bool internalOnly(const std::string& dataset) { return dataset == "idmt"; }

namespace idmt {

struct Label {
  std::string audio;
  int pitch = 0, string = 0, fret = -1;
  double onset = 0.0, offset = 0.0;
};

/// The text of the first <tag>...</tag> in `xml`, or "".
inline std::string tag(const std::string& xml, const std::string& name) {
  const std::string open = "<" + name + ">", close = "</" + name + ">";
  const size_t a = xml.find(open);
  if (a == std::string::npos) return "";
  const size_t b = xml.find(close, a);
  return b == std::string::npos ? "" : xml.substr(a + open.size(), b - a - open.size());
}

/// One annotation file (one event); pitch 0 if it cannot be read.
inline Label readLabel(const std::string& path) {
  const std::string x = json::slurp(path);
  Label l;
  if (x.find("<event>") == std::string::npos) return l;
  l.audio = tag(x, "audioFileName");
  l.pitch = std::atoi(tag(x, "pitch").c_str());
  l.string = std::atoi(tag(x, "stringNumber").c_str());
  l.fret = std::atoi(tag(x, "fretNumber").c_str());
  l.onset = std::atof(tag(x, "onsetSec").c_str());
  l.offset = std::atof(tag(x, "offsetSec").c_str());
  return l;
}

/// "Ibanez Power Strat Clean Bridge+Neck SC" -> "ibanez-power-strat-clean-bridge+neck-sc".
inline std::string slug(const std::string& s) {
  std::string o;
  for (char ch : s) o += ch == ' ' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return o;
}

}  // namespace idmt

/// Every labelled note of every setup in `root` (public/idmt-smt-guitar/dataset1).
/// One labelled IDMT note as a case: its file, and the segment its label marks, fitted on load.
inline Case idmtCase(const std::filesystem::path& setup, const idmt::Label& l) {
  Case c;
  c.dataset = "idmt";
  c.family = idmt::slug(setup.filename().string());
  c.value = "s" + std::to_string(7 - l.string) + "f" + std::to_string(l.fret);
  c.id = "idmt-" + c.family + "-" + c.value;
  c.file = (setup / "audio" / l.audio).string();
  c.source = "IDMT-SMT-Guitar dataset1 (Kehling et al., DAFx 2014), CC BY-NC-ND 4.0: internal testing only";
  c.load = [l](Case& self) {
    if (!loadWav(self)) return false;
    const double fileEnd = static_cast<double>(self.x.size()) / self.fs;
    Segment g;
    g.onset = std::min(l.onset, fileEnd);
    g.end = std::min(fileEnd, std::max(l.offset, g.onset + 0.3));
    g.midi = l.pitch;
    fitSegment(self.x, self.fs, g);
    self.segments = {g};
    return true;
  };
  return c;
}

inline bool usable(const idmt::Label& l) { return l.pitch > 0 && l.string >= 1 && l.string <= 6 && !l.audio.empty(); }

/// Every usable labelled note of one guitar setup (a folder with annotation/*.xml and audio/).
inline void setupCases(const std::filesystem::path& setup, std::vector<Case>& out) {
  namespace fs = std::filesystem;
  const fs::path ann = setup / "annotation";
  if (!fs::is_directory(ann)) return;
  for (const auto& f : fs::directory_iterator(ann)) {
    if (f.path().extension() != ".xml") continue;
    const idmt::Label l = idmt::readLabel(f.path().string());
    if (usable(l)) out.push_back(idmtCase(setup, l));
  }
}

inline std::vector<Case> idmtCases(const std::string& root) {
  namespace fs = std::filesystem;
  std::vector<Case> out;
  if (!fs::is_directory(root)) return out;
  for (const auto& setup : fs::directory_iterator(root)) setupCases(setup.path(), out);
  std::sort(out.begin(), out.end(), [](const Case& a, const Case& b) { return a.id < b.id; });
  return out;
}

}  // namespace quality
