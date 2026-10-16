// SPDX-License-Identifier: MIT
// A plugin's saved state: versioned text, one value per line, parameters by their contract key
// (so a state survives id changes and reads the same on every target). Main thread only; it
// allocates freely.
//
//   squatch-state 1
//   type cc.squatch.gate 1          <- the block's type id and state version
//   version 1.1.0
//   param threshold -60
//   ...
//   file ir /path/to/cab.wav        <- blocks with a file (the cabinet's impulse response)
//   learned 0 53515347...           <- blocks that learn: instance 0's learned state, in hex
//   end
//
// Reading refuses another type or a newer state version; unknown lines are skipped, and a
// parameter the state does not mention takes its default. Streams are read and written in a
// loop, because hosts may move any number of bytes per call (clap-validator's buffered tests).
#pragma once
#include <clap/stream.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <squatch/dsp/block.hpp>
#include <string>
#include <vector>

namespace squatch::clap {

constexpr std::size_t kMaxStateBytes = 1 << 16;
constexpr const char* kStateMagic = "squatch-state 1";

struct StateValues {
  std::vector<float> values;         // by dense id, every descriptor
  std::string file;                  // empty: none
  std::vector<std::string> learned;  // by block instance, the bytes; empty: nothing learned
};

constexpr std::size_t kMaxInstances = 8;

inline std::string toHex(const std::string& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) out += kDigits[c >> 4], out += kDigits[c & 15];
  return out;
}

/// False for an odd length or anything but hex digits.
inline bool fromHex(const std::string& hex, std::string& out) {
  const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  if (hex.size() % 2) return false;
  out.clear();
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = digit(hex[i]), lo = digit(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out += static_cast<char>(hi * 16 + lo);
  }
  return true;
}

inline std::string formatState(const dsp::BlockInfo& info, dsp::Span<const dsp::ParamDesc> params,
                               const StateValues& s, const char* fileKey) {
  std::string out = std::string(kStateMagic) + "\ntype " + info.typeId + " " + std::to_string(info.stateVersion) +
                    "\nversion " + info.version + "\n";
  char line[160];
  for (const dsp::ParamDesc& p : params) {
    if (!p.settable()) continue;
    std::snprintf(line, sizeof line, "param %s %.9g\n", p.key, static_cast<double>(s.values[p.id]));
    out += line;
  }
  if (fileKey && !s.file.empty()) out += std::string("file ") + fileKey + " " + s.file + "\n";
  for (std::size_t i = 0; i < s.learned.size(); ++i)
    if (!s.learned[i].empty()) out += "learned " + std::to_string(i) + " " + toHex(s.learned[i]) + "\n";
  return out + "end\n";
}

namespace state_detail {

inline bool header(const std::string& line, const dsp::BlockInfo& info) {
  char type[128] = {};
  unsigned version = 0;
  if (std::sscanf(line.c_str(), "type %127s %u", type, &version) != 2) return false;
  return std::strcmp(type, info.typeId) == 0 && version >= 1 && version <= info.stateVersion;
}

inline void param(const std::string& line, dsp::Span<const dsp::ParamDesc> params, StateValues& s) {
  char key[64] = {};
  char number[64] = {};
  if (std::sscanf(line.c_str(), "param %63s %63s", key, number) != 2) return;
  const dsp::ParamDesc* p = dsp::findKey(params, key);
  if (p && p->settable()) s.values[p->id] = p->clamp(std::strtof(number, nullptr));
}

inline void file(const std::string& line, const char* fileKey, StateValues& s) {
  const std::string prefix = std::string("file ") + (fileKey ? fileKey : "") + " ";
  if (fileKey && line.compare(0, prefix.size(), prefix) == 0) s.file = line.substr(prefix.size());
}

/// "learned N HEX": false when it is malformed (the whole state is then refused).
inline bool learned(const std::string& line, StateValues& s) {
  const std::size_t space = line.find(' ', 8);
  if (space == std::string::npos || space == 8 || space - 8 > 1) return false;
  const std::size_t n = static_cast<std::size_t>(line[8] - '0');
  std::string bytes;
  if (n >= kMaxInstances || !fromHex(line.substr(space + 1), bytes) || bytes.empty()) return false;
  if (s.learned.size() <= n) s.learned.resize(n + 1);
  s.learned[n] = std::move(bytes);
  return true;
}

}  // namespace state_detail

namespace state_detail {

struct Parse {
  const dsp::BlockInfo& info;
  dsp::Span<const dsp::ParamDesc> params;
  const char* fileKey;
  StateValues next;
  bool typed = false, ended = false, bad = false;
};

inline bool starts(const std::string& line, const char* word) { return line.rfind(word, 0) == 0; }

inline void line(Parse& p, const std::string& text, int lineNo) {
  if (lineNo == 0) {
    p.bad = text != kStateMagic;
  } else if (starts(text, "type ")) {
    p.typed = header(text, p.info);
    p.bad = !p.typed;
  } else if (starts(text, "param ")) {
    param(text, p.params, p.next);
  } else if (starts(text, "file ")) {
    file(text, p.fileKey, p.next);
  } else if (starts(text, "learned ")) {
    p.bad = !learned(text, p.next);
  } else {
    p.ended = text == "end";
  }
}

}  // namespace state_detail

/// False (and `s` untouched) for anything that is not a state of this block type and version.
inline bool parseState(const std::string& text, const dsp::BlockInfo& info, dsp::Span<const dsp::ParamDesc> params,
                       const char* fileKey, StateValues& s) {
  state_detail::Parse p{info, params, fileKey, {}};
  for (const dsp::ParamDesc& d : params) p.next.values.push_back(d.def);
  std::size_t at = 0;
  for (int n = 0; at < text.size() && !p.ended && !p.bad; ++n) {
    const std::size_t eol = std::min(text.find('\n', at), text.size());
    state_detail::line(p, text.substr(at, eol - at), n);
    at = eol + 1;
  }
  if (!p.typed || !p.ended || p.bad) return false;
  s = std::move(p.next);
  return true;
}

/// Writes all of `data`, however few bytes the stream takes per call.
inline bool writeAll(const clap_ostream_t* stream, const std::string& data) {
  std::size_t done = 0;
  while (done < data.size()) {
    const int64_t n = stream->write(stream, data.data() + done, data.size() - done);
    if (n <= 0) return false;
    done += static_cast<std::size_t>(n);
  }
  return true;
}

/// Reads to the end of the stream (at most kMaxStateBytes), however few bytes arrive per call.
inline bool readAll(const clap_istream_t* stream, std::string& out) {
  char buf[4096];
  out.clear();
  for (;;) {
    const int64_t n = stream->read(stream, buf, sizeof buf);
    if (n < 0) return false;
    if (n == 0) return true;
    out.append(buf, static_cast<std::size_t>(n));
    if (out.size() > kMaxStateBytes) return false;
  }
}

}  // namespace squatch::clap
