#ifndef FIRMIUS_SHARED_FRONTMATTER_PARSER_HPP
#define FIRMIUS_SHARED_FRONTMATTER_PARSER_HPP

#include <map>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace firmius::shared {

struct FrontmatterValue {
  struct Array : std::vector<FrontmatterValue> {
    using std::vector<FrontmatterValue>::vector;
  };
  struct Map : std::map<std::string, FrontmatterValue> {
    using std::map<std::string, FrontmatterValue>::map;
  };
  using Value = std::variant<std::string, bool, int64_t, Array, Map>;

  Value value;

  FrontmatterValue() : value(std::string{}) {}
  FrontmatterValue(const std::string &v) : value(v) {}
  FrontmatterValue(std::string &&v) : value(std::move(v)) {}
  FrontmatterValue(bool v) : value(v) {}
  FrontmatterValue(int64_t v) : value(v) {}
  FrontmatterValue(const Array &v) : value(v) {}
  FrontmatterValue(Array &&v) : value(std::move(v)) {}
  FrontmatterValue(const Map &v) : value(v) {}
  FrontmatterValue(Map &&v) : value(std::move(v)) {}
};

struct FrontmatterDocument {
  std::map<std::string, FrontmatterValue> values;
  std::string body;
};

class FrontmatterParser {
public:
  static FrontmatterDocument parseMarkdown(std::string_view markdown,
                                           const std::string &source = {});
  static std::map<std::string, FrontmatterValue>
  parse(std::string_view frontmatter, const std::string &source = {});

  static const FrontmatterValue *
  find(const FrontmatterDocument &document, const std::string &key);
  static std::optional<std::string>
  getString(const FrontmatterDocument &document, const std::string &key);
  static std::optional<bool>
  getBool(const FrontmatterDocument &document, const std::string &key);
  static std::vector<std::string>
  getStringArray(const FrontmatterDocument &document, const std::string &key);
  static std::optional<int64_t>
  getInt(const FrontmatterDocument &document, const std::string &key);
  static std::optional<FrontmatterValue::Map>
  getMap(const FrontmatterDocument &document, const std::string &key);
  static std::optional<FrontmatterValue::Array>
  getArray(const FrontmatterDocument &document, const std::string &key);
};

} // namespace firmius::shared

#endif
