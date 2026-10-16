// SPDX-License-Identifier: MIT
// Tests for the GuitarSet, NSynth and IDMT loaders (guitarset_cases.hpp, nsynth_cases.hpp,
// idmt_cases.hpp) on small made-up inputs: no dataset is needed. Included by test_quality.cpp,
// which defines CHECK; call testPublicCases().
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "guitarset_cases.hpp"
#include "idmt_cases.hpp"
#include "nsynth_cases.hpp"

namespace quality {
namespace publictest {

// Both JAMS layouts, strings, escapes and nesting.
inline void testJson() {
  const json::Value v = json::parse(R"({"a": [1, -2.5e1, true, null, "x\"y"], "b": {"c": "d"}, "e": []})");
  CHECK(v.kind == json::Value::Object && v["a"].size() == 5, "parsed an object with a 5-element array");
  CHECK(v["a"].items[1].number() == -25.0 && v["a"].items[2].b && v["a"].items[3].isNull() && v["a"].items[4].text() == "x\"y", "array values");
  CHECK(v["b"]["c"].text() == "d" && v["e"].size() == 0 && v["missing"].isNull(), "members, empty array, missing member");
  CHECK(json::parse("{\"a\": [1, 2}").isNull() && json::parse("[1] x").isNull(), "malformed JSON is a null root");
}

// A JAMS file with one string's notes (list layout) and contour (column layout): the notes become
// segments, the contour inside each note its annotated truth, an octave slip is dropped, and a gap
// ends the note's contour.
// A JAMS file with two notes on string 0 (out of order), a contour with an unvoiced point, an
// octave slip and a gap, and a layer the loader must ignore.
inline std::string writeJams(const std::string& dir) {
  const std::string path = dir + "/t.jams";
  FILE* f = std::fopen(path.c_str(), "w");
  std::fprintf(f, "%s", R"({"annotations": [
    {"namespace": "note_midi", "annotation_metadata": {"data_source": "0"},
     "data": [{"time": 1.0, "duration": 0.5, "value": 40.1, "confidence": null},
              {"time": 0.2, "duration": 0.4, "value": 45.0, "confidence": null}]},
    {"namespace": "pitch_contour", "annotation_metadata": {"data_source": "0"},
     "data": {"time": [0.25, 0.3, 0.35, 1.0, 1.05, 1.1, 1.3, 1.35],
              "duration": [0, 0, 0, 0, 0, 0, 0, 0],
              "value": [{"voiced": true, "index": 0, "frequency": 110.0}, {"voiced": true, "index": 0, "frequency": 220.0},
                        {"voiced": false, "index": 0, "frequency": -110.0}, {"voiced": true, "index": 0, "frequency": 82.4},
                        {"voiced": true, "index": 0, "frequency": 82.5}, {"voiced": true, "index": 0, "frequency": 82.6},
                        {"voiced": true, "index": 0, "frequency": 82.7}, {"voiced": true, "index": 0, "frequency": 82.8}],
              "confidence": [null, null, null, null, null, null, null, null]}},
    {"namespace": "chord", "annotation_metadata": {"data_source": ""}, "data": []}]})");
  std::fclose(f);
  return path;
}

inline void testGuitarSetSegments(const guitarset::Labels& l) {
  const std::vector<Segment> g = guitarset::segments(l[0], 3.0);
  CHECK(g.size() == 2, "segments from note_midi: %zu", g.size());
  CHECK(g[0].midi == 45 && g[1].midi == 40, "segments in time order");
  CHECK(std::fabs(g[0].end - 0.6) < 1e-9, "a segment ends with its note");
  CHECK(g[0].contour.size() == 1 && g[0].truth == "note", "the octave slip at 0.30 s is dropped: one point left, no contour truth");
  CHECK(g[1].contour.size() == 3 && g[1].truth == "annotated", "the gap after 1.10 s ends the contour: %zu points", g[1].contour.size());
  Case c;
  c.segments = g;
  const std::vector<TruthRow> r = truthSeries(c, {1050.0, 1075.0, 1400.0});
  CHECK(std::fabs(r[0].hz - 82.5) < 1e-9 && r[0].midi == 40 && r[0].segment == 1, "annotated truth is the contour");
  CHECK(r[1].hz > 82.5 && r[1].hz < 82.6, "the contour is interpolated between points");
  CHECK(r[2].hz == 0.0 && r[2].midi == 40, "past the contour: the note only");
}

inline void testGuitarSetLabels(const std::string& dir) {
  guitarset::Labels l;
  CHECK(guitarset::readLabels(writeJams(dir), l), "JAMS read");
  CHECK(l[0].notes.size() == 2 && l[0].notes[0].t == 0.2, "notes sorted");
  CHECK(l[0].contour.size() == 7, "unvoiced point dropped: %zu", l[0].contour.size());
  CHECK(l[1].notes.empty(), "another string's labels stay apart");
  testGuitarSetSegments(l);
}

// A 3-channel 16-bit WAV: each channel is read on its own.
inline void testWavChannel(const std::string& dir) {
  const std::string path = dir + "/hex.wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  const int16_t frames[4][3] = {{100, 200, 300}, {-100, -200, -300}, {1, 2, 3}, {0, 16384, -16384}};
  const uint32_t data = sizeof frames, riff = 36 + data, fmtLen = 16, rate = 44100, byteRate = rate * 6;
  const uint16_t tag = 1, ch = 3, align = 6, bits = 16;
  std::fwrite("RIFF", 1, 4, f);
  std::fwrite(&riff, 4, 1, f);
  std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmtLen, 4, 1, f);
  std::fwrite(&tag, 2, 1, f);
  std::fwrite(&ch, 2, 1, f);
  std::fwrite(&rate, 4, 1, f);
  std::fwrite(&byteRate, 4, 1, f);
  std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f);
  std::fwrite("data", 1, 4, f);
  std::fwrite(&data, 4, 1, f);
  std::fwrite(frames, sizeof frames, 1, f);
  std::fclose(f);
  bench::Audio a, b;
  CHECK(readWavChannel(path, 2, a) && a.x.size() == 4 && a.rate == 44100.0f, "channel 2 read");
  CHECK(a.x[0] == 300 / 32768.0f && a.x[3] == -0.5f, "channel 2 samples: %g %g", static_cast<double>(a.x[0]), static_cast<double>(a.x[3]));
  CHECK(readWavChannel(path, 1, b) && b.x[3] == 0.5f && !readWavChannel(path, 3, b), "channel 1; no channel 3");
}

// NSynth names, and the 16 -> 48 kHz resampling keeps a partial's frequency exactly.
inline void testNSynth() {
  const nsynth::Name n = nsynth::parseName("guitar_acoustic_010-066-100");
  CHECK(n.instrument == "guitar_acoustic_010" && n.pitch == 66 && n.velocity == 100, "name parsed");
  CHECK(nsynth::parseName("bad").pitch == 0 && nsynth::value(n, "reverb") == "p066v100|reverb", "bad name; value");
  std::vector<float> x(16000 * 2);
  const double f0 = midiHz(47) * std::exp2(3.0 / 1200.0);
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / 16000.0;
    for (int k = 1; k <= 4; ++k) x[i] += static_cast<float>(0.3 / k * std::sin(2 * 3.141592653589793 * k * f0 * t));
  }
  const std::vector<float> y = bench::resample(x, 16000.0 / nsynth::kRate);
  // The same second of the note, read by the offline reference before and after resampling.
  const RefFit a = fitWindow({&x[8000], 16000, 16000.0}, {midiHz(47), 60.0, 8});
  const RefFit b = fitWindow({&y[24000], 48000, nsynth::kRate}, {midiHz(47), 60.0, 8});
  CHECK(y.size() == 3 * x.size() && a.ok && b.ok && std::fabs(cents(b.f0, a.f0)) < 0.001, "resampling moved the pitch %+.5f c", cents(b.f0, a.f0));
}

inline void testIdmt(const std::string& dir) {
  const std::string path = dir + "/n.xml";
  FILE* f = std::fopen(path.c_str(), "w");
  std::fprintf(f, "<instrumentRecording><globalParameter><audioFileName>G53-45105-1111-00006.wav</audioFileName></globalParameter>"
                  "<transcription><event><pitch>50</pitch><onsetSec>0.203</onsetSec><offsetSec>2.1</offsetSec><fretNumber>5</fretNumber>"
                  "<stringNumber>2</stringNumber></event></transcription></instrumentRecording>");
  std::fclose(f);
  const idmt::Label l = idmt::readLabel(path);
  CHECK(l.audio == "G53-45105-1111-00006.wav" && l.pitch == 50 && l.string == 2 && l.fret == 5 && l.onset == 0.203 && l.offset == 2.1, "label");
  CHECK(idmt::slug("Ibanez Power Strat Clean Bridge+Neck SC") == "ibanez-power-strat-clean-bridge+neck-sc", "slug");
  CHECK(internalOnly("idmt") && !internalOnly("guitarset") && !internalOnly("nsynth"), "only IDMT is internal-only");
}

}  // namespace publictest

inline void testPublicCases() {
  const std::string dir = (std::filesystem::temp_directory_path() / "tuner-quality-public-test").string();
  std::filesystem::create_directories(dir);
  publictest::testJson();
  publictest::testGuitarSetLabels(dir);
  publictest::testWavChannel(dir);
  publictest::testNSynth();
  publictest::testIdmt(dir);
  std::filesystem::remove_all(dir);
  std::printf("public-set loaders: GuitarSet JAMS and hex channels, NSynth names and resampling, IDMT labels\n");
}

}  // namespace quality
