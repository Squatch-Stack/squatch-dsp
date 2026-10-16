// SPDX-License-Identifier: MIT
// A minimal JSON reader for dataset labels (GuitarSet's JAMS, NSynth's examples.json). Not for the
// audio path: it builds a tree, accepts only well-formed JSON, and reports failure as a null root.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace quality {
namespace json {

struct Value {
  enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<Value> items;       // Array elements, or Object values
  std::vector<std::string> keys;  // Object keys, parallel to items

  bool isNull() const { return kind == Null; }
  /// The member `k` of an object, or a shared null.
  const Value& operator[](const std::string& k) const {
    static const Value none;
    if (kind != Object) return none;
    for (size_t i = 0; i < keys.size(); ++i)
      if (keys[i] == k) return items[i];
    return none;
  }
  double number(double fallback = 0.0) const { return kind == Number ? num : fallback; }
  const std::string& text() const { return str; }
  size_t size() const { return items.size(); }
};

namespace detail {

struct Parser {
  const char* p;
  const char* end;
  bool ok = true;

  void ws() {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
  }
  bool eat(char c) {
    ws();
    if (p < end && *p == c) {
      ++p;
      return true;
    }
    return false;
  }
  bool word(const char* w, size_t n) {
    if (static_cast<size_t>(end - p) < n || std::string(p, n) != std::string(w, n)) return ok = false;
    p += n;
    return true;
  }
  // The character an escape stands for (\n, \t, ...; anything else stands for itself).
  static char unescape(char c) {
    static const char from[] = "ntrbf", to[] = "\n\t\r\b\f";
    const char* at = std::strchr(from, c);
    return at && c ? to[at - from] : c;
  }
  // One escape, p on the character after the backslash; leaves p after it.
  void escape(std::string& s) {
    if (*p == 'u') {  // labels are ASCII; keep a placeholder rather than decode UTF-16
      s += '?';
      p += std::min<long>(4, end - p - 1);
    } else {
      s += unescape(*p);
    }
    ++p;
  }
  std::string string() {
    std::string s;
    if (!eat('"')) {
      ok = false;
      return s;
    }
    while (p < end && *p != '"') {
      if (*p == '\\' && p + 1 < end) {
        ++p;
        escape(s);
      } else {
        s += *p++;
      }
    }
    if (p >= end) ok = false;
    else ++p;
    return s;
  }
  Value value(int depth) {
    Value v;
    ws();
    if (p >= end || depth > 64) {
      ok = false;
      return v;
    }
    if (*p == '{') return object(depth);
    if (*p == '[') return array(depth);
    if (*p == '"') {
      v.kind = Value::String;
      v.str = string();
      return v;
    }
    if (*p == 't') {
      word("true", 4);
      v.kind = Value::Bool;
      v.b = true;
      return v;
    }
    if (*p == 'f') {
      word("false", 5);
      v.kind = Value::Bool;
      return v;
    }
    if (*p == 'n') {
      word("null", 4);
      return v;
    }
    char* stop = nullptr;
    v.num = std::strtod(p, &stop);
    if (stop == p) {
      ok = false;
      return v;
    }
    p = stop;
    v.kind = Value::Number;
    return v;
  }
  Value array(int depth) {
    Value v;
    v.kind = Value::Array;
    ++p;
    if (eat(']')) return v;
    do v.items.push_back(value(depth + 1));
    while (ok && eat(','));
    if (!eat(']')) ok = false;
    return v;
  }
  Value object(int depth) {
    Value v;
    v.kind = Value::Object;
    ++p;
    if (eat('}')) return v;
    do {
      v.keys.push_back(string());
      if (!eat(':')) ok = false;
      v.items.push_back(value(depth + 1));
    } while (ok && eat(','));
    if (!eat('}')) ok = false;
    return v;
  }
};

}  // namespace detail

/// Parses `text`; a null Value if it is not well-formed JSON.
inline Value parse(const std::string& text) {
  detail::Parser ps{text.data(), text.data() + text.size()};
  Value v = ps.value(0);
  ps.ws();
  if (!ps.ok || ps.p != ps.end) return Value{};
  return v;
}

inline std::string slurp(const std::string& path) {
  std::string s;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return s;
  char buf[1 << 16];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
  std::fclose(f);
  return s;
}

inline Value parseFile(const std::string& path) { return parse(slurp(path)); }

}  // namespace json
}  // namespace quality
