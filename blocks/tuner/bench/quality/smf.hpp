// SPDX-License-Identifier: MIT
// The notes of a Standard MIDI File (format 0 or 1), in seconds, for the labelled real sets
// (Guitar-TECHS ships one MIDI file per recording, one track per string). Bench code only.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../wav.hpp"

namespace quality {

struct MidiNote {
  double start = 0.0, end = 0.0;  // seconds
  int pitch = 0, velocity = 0, track = 0;
};

namespace smfdetail {

struct Event {
  uint32_t tick;
  int track, kind, a, b;  // kind: 0x90 / 0x80 status nibble, or 0x51 for a tempo change
};

inline uint32_t varLen(const std::vector<unsigned char>& d, size_t& i) {
  uint32_t v = 0;
  while (i < d.size()) {
    const unsigned char c = d[i++];
    v = (v << 7) | (c & 0x7Fu);
    if (!(c & 0x80u)) break;
  }
  return v;
}

/// One track being read: the bytes, the position, the running status and the tick.
struct TrackReader {
  const std::vector<unsigned char>& d;
  size_t i = 0;
  int rs = 0, track = 0;
  uint32_t tick = 0;
};

/// A meta event (tempo is the only one kept) or a SysEx, skipped.
inline void metaOrSysex(TrackReader& r, std::vector<Event>& out) {
  if (r.d[r.i] == 0xFF) {
    const int type = r.d[r.i + 1];
    r.i += 2;
    const uint32_t len = varLen(r.d, r.i);
    if (type == 0x51 && len == 3) out.push_back({r.tick, r.track, 0x51, (r.d[r.i] << 16) | (r.d[r.i + 1] << 8) | r.d[r.i + 2], 0});
    r.i += len;
    return;
  }
  ++r.i;
  r.i += varLen(r.d, r.i);
}

/// A note on (velocity > 0) or off at the reader's position.
inline void note(const TrackReader& r, int status, std::vector<Event>& out) {
  const bool on = status == 0x90 && r.d[r.i + 1] > 0;
  out.push_back({r.tick, r.track, on ? 0x90 : 0x80, r.d[r.i], r.d[r.i + 1]});
}

/// One event at the reader's position (after its delta time). Appends note and tempo events.
inline void event(TrackReader& r, std::vector<Event>& out) {
  const int st = r.d[r.i];
  if (st == 0xFF || st == 0xF0 || st == 0xF7) return metaOrSysex(r, out);
  if (st & 0x80) r.rs = st, ++r.i;
  const int hi = r.rs & 0xF0;
  if (hi == 0x90 || hi == 0x80) note(r, hi, out);
  r.i += (hi == 0xC0 || hi == 0xD0) ? 1u : 2u;
}

inline std::vector<Event> events(const std::vector<unsigned char>& b, int& division) {
  std::vector<Event> out;
  if (b.size() < 14 || std::string(b.begin(), b.begin() + 4) != "MThd") return out;
  division = (b[12] << 8) | b[13];
  size_t pos = 14;
  for (int track = 0; pos + 8 <= b.size(); ++track) {
    const size_t body = pos + 8;
    const size_t be = (size_t(b[pos + 4]) << 24) | (size_t(b[pos + 5]) << 16) | (size_t(b[pos + 6]) << 8) | b[pos + 7];  // big-endian
    const std::vector<unsigned char> d(b.begin() + static_cast<long>(body), b.begin() + static_cast<long>(std::min(b.size(), body + be)));
    TrackReader r{d};
    r.track = track;
    while (r.i < d.size()) {
      r.tick += varLen(d, r.i);
      if (r.i < d.size()) event(r, out);
    }
    pos = body + be;
  }
  std::stable_sort(out.begin(), out.end(), [](const Event& x, const Event& y) { return x.tick < y.tick; });
  return out;
}

}  // namespace smfdetail

/// Every note, sorted by start time. Tempo changes from any track apply to all (format 1).
inline std::vector<MidiNote> readMidi(const std::string& path) {
  int division = 480;
  const std::vector<smfdetail::Event> ev = smfdetail::events(bench::wavdetail::slurp(path), division);
  std::vector<MidiNote> notes, open;
  double sec = 0.0, usPerQ = 500000.0;
  uint32_t last = 0;
  for (const smfdetail::Event& e : ev) {
    sec += (e.tick - last) * usPerQ / 1e6 / division;
    last = e.tick;
    if (e.kind == 0x51) { usPerQ = e.a; continue; }
    if (e.kind == 0x90) { open.push_back({sec, sec, e.a, e.b, e.track}); continue; }
    for (size_t k = 0; k < open.size(); ++k)
      if (open[k].pitch == e.a && open[k].track == e.track) {
        open[k].end = sec;
        notes.push_back(open[k]);
        open.erase(open.begin() + static_cast<long>(k));
        break;
      }
  }
  std::sort(notes.begin(), notes.end(), [](const MidiNote& x, const MidiNote& y) { return x.start < y.start; });
  return notes;
}

}  // namespace quality
