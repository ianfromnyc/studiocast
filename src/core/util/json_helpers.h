#pragma once

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "core/util/json.h"

namespace studiocast::util::json::helpers {

enum class LookupStatus {
  missing,
  wrong_type,
  found,
};

enum class IntConversionMode {
  truncate,
  round,
  strict_integer,
};

inline const Value::Object *AsObjectOrNull(const Value *v) {
  return v ? v->AsObject() : nullptr;
}

inline const Value *Find(const Value::Object &obj, std::string_view key) {
  const auto it = obj.find(std::string(key));
  if (it == obj.end())
    return nullptr;
  return &it->second;
}

inline LookupStatus TryGetBool(const Value::Object &obj, std::string_view key,
                               bool *out) {
  const Value *v = Find(obj, key);
  if (!v)
    return LookupStatus::missing;
  const bool *b = v->AsBool();
  if (!b)
    return LookupStatus::wrong_type;
  if (out)
    *out = *b;
  return LookupStatus::found;
}

inline LookupStatus TryGetString(const Value::Object &obj, std::string_view key,
                                 std::string *out) {
  const Value *v = Find(obj, key);
  if (!v)
    return LookupStatus::missing;
  const std::string *s = v->AsString();
  if (!s)
    return LookupStatus::wrong_type;
  if (out)
    *out = *s;
  return LookupStatus::found;
}

inline LookupStatus TryGetNumber(const Value::Object &obj, std::string_view key,
                                 double *out) {
  const Value *v = Find(obj, key);
  if (!v)
    return LookupStatus::missing;
  const double *n = v->AsNumber();
  if (!n)
    return LookupStatus::wrong_type;
  if (out)
    *out = *n;
  return LookupStatus::found;
}

inline LookupStatus TryGetObject(const Value::Object &obj, std::string_view key,
                                 const Value::Object **out) {
  const Value *v = Find(obj, key);
  if (!v)
    return LookupStatus::missing;
  const Value::Object *o = v->AsObject();
  if (!o)
    return LookupStatus::wrong_type;
  if (out)
    *out = o;
  return LookupStatus::found;
}

inline bool ConvertNumberToInt(double n, IntConversionMode mode, int *out) {
  double r = n;
  switch (mode) {
  case IntConversionMode::truncate:
    if (out)
      *out = static_cast<int>(n);
    return true;
  case IntConversionMode::round:
    r = std::round(n);
    if (out)
      *out = static_cast<int>(r);
    return true;
  case IntConversionMode::strict_integer:
    r = std::round(n);
    if (std::fabs(n - r) > 1e-9)
      return false;
    if (out)
      *out = static_cast<int>(r);
    return true;
  }
  return false;
}

inline bool Fail(std::string *error, std::string_view message) {
  if (error)
    *error = std::string(message);
  return false;
}

inline void AddWarning(std::vector<std::string> *warnings,
                       std::string_view warning) {
  if (warnings)
    warnings->push_back(std::string(warning));
}

inline std::string JoinPath(std::string_view parent, std::string_view key) {
  if (parent.empty())
    return std::string(key);
  return std::string(parent) + "." + std::string(key);
}

} // namespace studiocast::util::json::helpers
