#pragma once

// The fork's switches, each declared once at namespace scope by the module that reads it
// (docs/swtor/adr/0001-switches.md). Declaring registers it while the DLL loads and reads nothing; the session
// header (src/mullion/mullion_session.hpp) lists every registered switch with its value on its `switches` line, so
// a new switch appears there by being declared.
//
//   sw::Switch<bool> kFoo{"MULLION_FOO", true, sw::notZero};   // kFoo() reads the environment now
//
// A switch keeps no cache: each read parses the environment, as env::getEnvVar does (MULLION_ falling back to
// DXMT_). A caller that wants its value once keeps a `static const` of its own.
//
// Pure C++ (no Windows, no Metal): tests/host/session.cpp builds it natively against its own registry. Each DLL
// that links it has its own process registry.

#include "util_env.hpp"
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mullion::sw {

// a variable's raw text, "" when unset
using Source = std::string (*)(const char *name);

class Registry {
public:
  explicit Registry(Source source) : source_(source) {}
  Registry(const Registry &) = delete;
  Registry &operator=(const Registry &) = delete;

  std::string
  requested(const char *name) const {
    return source_(name);
  }

  // `value` writes the switch's value as the header prints it. false, and nothing registered, when `name` already
  // has a switch: one declaration per name
  bool
  add(const char *name, std::function<std::string()> value) {
    std::lock_guard lock(mutex_);
    return switches_.emplace(name, std::move(value)).second;
  }

  // every switch with its value now, sorted by name (byte order)
  std::vector<std::pair<std::string, std::string>>
  list() const {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::string, std::string>> listed;
    for (auto &[name, value] : switches_)
      listed.emplace_back(name, value());
    return listed;
  }

private:
  Source source_;
  mutable std::mutex mutex_;
  std::map<std::string, std::function<std::string()>> switches_;
};

// the process's registry, reading the environment; this DLL's own
inline Registry &
processRegistry() {
  static Registry registry(&env::getEnvVar);
  return registry;
}

// how the header writes a switch's value: a bool as on or off, a number as its digits, a string as it is (the header
// then applies its path rule)
template <class T>
std::string
Text(const T &value) {
  if constexpr (std::is_same_v<T, bool>)
    return value ? "on" : "off";
  else if constexpr (std::is_same_v<T, std::string>)
    return value;
  else
    return std::to_string(value);
}

// a switch's grammar: its value parsed from the raw text, or the fallback
template <class T> using Parser = T (*)(const std::string &raw, const T &fallback);

template <class T> class Switch {
public:
  Switch(const char *name, T fallback, Parser<T> parser, Registry &registry = processRegistry())
      : name_(name), fallback_(std::move(fallback)), parser_(parser), registry_(registry) {
    registry_.add(name_, [this] { return Text((*this)()); });
  }
  Switch(const Switch &) = delete;
  Switch &operator=(const Switch &) = delete;

  // the value now: one read of the environment
  T
  operator()() const {
    return parser_(registry_.requested(name_), fallback_);
  }

  const char *
  name() const {
    return name_;
  }

private:
  const char *name_;
  T fallback_;
  Parser<T> parser_;
  Registry &registry_;
};

// the grammars Mullion's switches have: `!= "0"`, on unless exactly "0", and `== "1"`, on only for exactly "1"; the
// fallback when unset or empty
inline bool
notZero(const std::string &raw, const bool &fallback) {
  return raw.empty() ? fallback : raw != "0";
}

inline bool
exactlyOne(const std::string &raw, const bool &fallback) {
  return raw.empty() ? fallback : raw == "1";
}

} // namespace mullion::sw
