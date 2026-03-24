#pragma once

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace studiocast::util::json {

struct Value {
  using Object = std::map<std::string, Value>;
  using Array = std::vector<Value>;

  std::variant<std::nullptr_t, bool, double, std::string, Object, Array> v;

  Value() : v(nullptr) {}
  explicit Value(std::nullptr_t) : v(nullptr) {}
  explicit Value(bool b) : v(b) {}
  explicit Value(double n) : v(n) {}
  explicit Value(std::string s) : v(std::move(s)) {}
  explicit Value(Object o) : v(std::move(o)) {}
  explicit Value(Array a) : v(std::move(a)) {}

  bool IsNull() const;
  bool IsBool() const;
  bool IsNumber() const;
  bool IsString() const;
  bool IsObject() const;
  bool IsArray() const;

  const bool *AsBool() const;
  const double *AsNumber() const;
  const std::string *AsString() const;
  const Object *AsObject() const;
  const Array *AsArray() const;
};

// Strict JSON parser for objects/arrays/strings/numbers/bools/null.
// - Returns false on syntax error with a human-readable message.
// - Accepts and ignores whitespace between tokens.
bool Parse(const std::string &text, Value *out, std::string *error);

// Removes whitespace outside of JSON strings. Useful for sending JSON over
// line-based IPC.
std::string Minify(const std::string &text);

// Escapes a string for embedding in JSON.
std::string EscapeString(const std::string &s);

} // namespace studiocast::util::json
