#ifndef FIRMIUS_SHARED_FRONTMATTER_PARSER_HPP
#define FIRMIUS_SHARED_FRONTMATTER_PARSER_HPP

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace firmius::shared {

struct FrontmatterValue {
  using Array = std::vector<std::string>;
  using Value = std::variant<std::string, bool, Array>;

  Value value;
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
};

} // namespace firmius::shared

#endif
