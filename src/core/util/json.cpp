#include "core/util/json.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>

namespace studiocast::util::json {

bool Value::IsNull() const { return std::holds_alternative<std::nullptr_t>(v); }
bool Value::IsBool() const { return std::holds_alternative<bool>(v); }
bool Value::IsNumber() const { return std::holds_alternative<double>(v); }
bool Value::IsString() const { return std::holds_alternative<std::string>(v); }
bool Value::IsObject() const { return std::holds_alternative<Object>(v); }
bool Value::IsArray() const { return std::holds_alternative<Array>(v); }

const bool *Value::AsBool() const { return std::get_if<bool>(&v); }
const double *Value::AsNumber() const { return std::get_if<double>(&v); }
const std::string *Value::AsString() const {
  return std::get_if<std::string>(&v);
}
const Value::Object *Value::AsObject() const { return std::get_if<Object>(&v); }
const Value::Array *Value::AsArray() const { return std::get_if<Array>(&v); }

namespace {
struct Parser {
  const std::string &s;
  std::size_t i = 0;
  std::string *err{};

  void SkipWs() {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      ++i;
  }

  char Peek() const { return i < s.size() ? s[i] : '\0'; }

  bool Consume(char c) {
    SkipWs();
    if (Peek() != c)
      return false;
    ++i;
    return true;
  }

  bool Expect(char c, const char *msg) {
    SkipWs();
    if (Peek() != c) {
      if (err)
        *err = FormatError(msg);
      return false;
    }
    ++i;
    return true;
  }

  std::string FormatError(const char *msg) const {
    std::ostringstream oss;
    oss << "JSON parse error at byte " << i << ": " << msg;
    return oss.str();
  }

  static std::optional<int> HexNibble(char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
      return 10 + (c - 'A');
    return std::nullopt;
  }

  static void AppendUtf8(std::string *out, std::uint32_t cp) {
    if (cp <= 0x7F) {
      out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out->push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseString(std::string *out) {
    SkipWs();
    if (Peek() != '"') {
      if (err)
        *err = FormatError("expected string");
      return false;
    }
    ++i;
    out->clear();
    while (i < s.size()) {
      char c = s[i++];
      if (c == '"')
        return true;
      if (static_cast<unsigned char>(c) < 0x20) {
        if (err)
          *err = FormatError("control character in string");
        return false;
      }
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (i >= s.size()) {
        if (err)
          *err = FormatError("unterminated escape sequence");
        return false;
      }
      const char e = s[i++];
      switch (e) {
      case '"':
        out->push_back('"');
        break;
      case '\\':
        out->push_back('\\');
        break;
      case '/':
        out->push_back('/');
        break;
      case 'b':
        out->push_back('\b');
        break;
      case 'f':
        out->push_back('\f');
        break;
      case 'n':
        out->push_back('\n');
        break;
      case 'r':
        out->push_back('\r');
        break;
      case 't':
        out->push_back('\t');
        break;
      case 'u': {
        if (i + 4 > s.size()) {
          if (err)
            *err = FormatError("short \\u escape");
          return false;
        }
        std::uint32_t cp = 0;
        for (int k = 0; k < 4; ++k) {
          const auto n = HexNibble(s[i + static_cast<std::size_t>(k)]);
          if (!n) {
            if (err)
              *err = FormatError("invalid hex in \\u escape");
            return false;
          }
          cp = (cp << 4) | static_cast<std::uint32_t>(*n);
        }
        i += 4;

        // Handle surrogate pairs.
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          // Expect a second \uXXXX.
          if (i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            i += 2;
            std::uint32_t cp2 = 0;
            for (int k = 0; k < 4; ++k) {
              const auto n = HexNibble(s[i + static_cast<std::size_t>(k)]);
              if (!n) {
                if (err)
                  *err = FormatError("invalid hex in surrogate escape");
                return false;
              }
              cp2 = (cp2 << 4) | static_cast<std::uint32_t>(*n);
            }
            i += 4;
            if (cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
              const std::uint32_t hi = cp - 0xD800;
              const std::uint32_t lo = cp2 - 0xDC00;
              cp = 0x10000 + ((hi << 10) | lo);
            } else {
              if (err)
                *err = FormatError("invalid low surrogate");
              return false;
            }
          } else {
            if (err)
              *err = FormatError("missing low surrogate");
            return false;
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          if (err)
            *err = FormatError("unexpected low surrogate");
          return false;
        }

        AppendUtf8(out, cp);
        break;
      }
      default:
        if (err)
          *err = FormatError("unknown escape sequence");
        return false;
      }
    }
    if (err)
      *err = FormatError("unterminated string");
    return false;
  }

  bool ParseNumber(double *out) {
    SkipWs();
    const std::size_t start = i;
    if (Peek() == '-')
      ++i;
    if (Peek() == '0') {
      ++i;
    } else {
      if (!std::isdigit(static_cast<unsigned char>(Peek()))) {
        if (err)
          *err = FormatError("invalid number");
        return false;
      }
      while (std::isdigit(static_cast<unsigned char>(Peek())))
        ++i;
    }
    if (Peek() == '.') {
      ++i;
      if (!std::isdigit(static_cast<unsigned char>(Peek()))) {
        if (err)
          *err = FormatError("invalid fraction");
        return false;
      }
      while (std::isdigit(static_cast<unsigned char>(Peek())))
        ++i;
    }
    if (Peek() == 'e' || Peek() == 'E') {
      ++i;
      if (Peek() == '+' || Peek() == '-')
        ++i;
      if (!std::isdigit(static_cast<unsigned char>(Peek()))) {
        if (err)
          *err = FormatError("invalid exponent");
        return false;
      }
      while (std::isdigit(static_cast<unsigned char>(Peek())))
        ++i;
    }
    const std::string tmp = s.substr(start, i - start);
    char *endp = nullptr;
    const double val = std::strtod(tmp.c_str(), &endp);
    if (!endp || endp == tmp.c_str()) {
      if (err)
        *err = FormatError("failed to parse number");
      return false;
    }
    *out = val;
    return true;
  }

  bool ParseLiteral(const char *lit) {
    SkipWs();
    const std::size_t n = std::strlen(lit);
    if (i + n > s.size())
      return false;
    if (s.compare(i, n, lit) != 0)
      return false;
    i += n;
    return true;
  }

  bool ParseValue(Value *out) {
    SkipWs();
    const char c = Peek();
    if (c == '{')
      return ParseObject(out);
    if (c == '[')
      return ParseArray(out);
    if (c == '"') {
      std::string str;
      if (!ParseString(&str))
        return false;
      *out = Value(std::move(str));
      return true;
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      double num = 0;
      if (!ParseNumber(&num))
        return false;
      *out = Value(num);
      return true;
    }
    if (ParseLiteral("true")) {
      *out = Value(true);
      return true;
    }
    if (ParseLiteral("false")) {
      *out = Value(false);
      return true;
    }
    if (ParseLiteral("null")) {
      *out = Value(nullptr);
      return true;
    }
    if (err)
      *err = FormatError("unexpected token");
    return false;
  }

  bool ParseObject(Value *out) {
    if (!Expect('{', "expected '{'"))
      return false;
    Value::Object obj;
    SkipWs();
    if (Consume('}')) {
      *out = Value(std::move(obj));
      return true;
    }
    while (true) {
      std::string key;
      if (!ParseString(&key))
        return false;
      if (!Expect(':', "expected ':' after object key"))
        return false;
      Value val;
      if (!ParseValue(&val))
        return false;
      obj.emplace(std::move(key), std::move(val));
      SkipWs();
      if (Consume('}'))
        break;
      if (!Expect(',', "expected ',' or '}' in object"))
        return false;
    }
    *out = Value(std::move(obj));
    return true;
  }

  bool ParseArray(Value *out) {
    if (!Expect('[', "expected '['"))
      return false;
    Value::Array arr;
    SkipWs();
    if (Consume(']')) {
      *out = Value(std::move(arr));
      return true;
    }
    while (true) {
      Value val;
      if (!ParseValue(&val))
        return false;
      arr.emplace_back(std::move(val));
      SkipWs();
      if (Consume(']'))
        break;
      if (!Expect(',', "expected ',' or ']' in array"))
        return false;
    }
    *out = Value(std::move(arr));
    return true;
  }
};
} // namespace

bool Parse(const std::string &text, Value *out, std::string *error) {
  if (!out)
    return false;
  Parser p{text, 0, error};
  Value v;
  if (!p.ParseValue(&v))
    return false;
  p.SkipWs();
  if (p.i != text.size()) {
    if (error)
      *error = p.FormatError("trailing characters");
    return false;
  }
  *out = std::move(v);
  return true;
}

std::string Minify(const std::string &text) {
  std::string out;
  out.reserve(text.size());

  bool inString = false;
  bool escape = false;
  for (char c : text) {
    if (!inString) {
      if (std::isspace(static_cast<unsigned char>(c)))
        continue;
      out.push_back(c);
      if (c == '"') {
        inString = true;
        escape = false;
      }
      continue;
    }

    // in string
    out.push_back(c);
    if (escape) {
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      inString = false;
    }
  }
  return out;
}

std::string EscapeString(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c0 : s) {
    const unsigned char uc = static_cast<unsigned char>(c0);
    const char c = c0;
    switch (c) {
    case '"':
      out.append("\\\"");
      break;
    case '\\':
      out.append("\\\\");
      break;
    case '\b':
      out.append("\\b");
      break;
    case '\f':
      out.append("\\f");
      break;
    case '\n':
      out.append("\\n");
      break;
    case '\r':
      out.append("\\r");
      break;
    case '\t':
      out.append("\\t");
      break;
    default:
      if (uc < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(uc));
        out.append(buf);
      } else {
        out.push_back(c);
      }
    }
  }
  return out;
}

} // namespace studiocast::util::json
