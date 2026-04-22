#include "audits/EditToolAudit.hpp"

#include "tools/FileEditTool.hpp"

#include <iostream>
#include <sstream>

namespace firmius::audits {

using firmius::core::FileEditTool;
using firmius::core::FileRangeTool;
using firmius::core::FileReplaceTool;
using firmius::core::FileWriteTool;
using firmius::shared::AuditResult;

std::string EditToolAudit::getId() const { return "edit_tool"; }

std::string EditToolAudit::getDescription() const {
    return "Verify the explicit patch/write/replace/range edit tool family contract expected by prompts and agents.";
}

AuditResult EditToolAudit::run(const std::vector<std::string>&) {
    AuditResult result;
    result.auditId = getId();

    FileEditTool patchTool;
    FileWriteTool writeTool;
    FileReplaceTool replaceTool;
    FileRangeTool rangeTool;
    const auto patchMeta = patchTool.getMetadata();
    const auto writeMeta = writeTool.getMetadata();
    const auto replaceMeta = replaceTool.getMetadata();
    const auto rangeMeta = rangeTool.getMetadata();
    const auto patchSchema = patchTool.getSchema()->toString();
    const auto writeSchema = writeTool.getSchema()->toString();
    const auto replaceSchema = replaceTool.getSchema()->toString();
    const auto rangeSchema = rangeTool.getSchema()->toString();

    std::ostringstream out;
    out << "patch_tool=" << patchMeta.name << "\n";
    out << "write_tool=" << writeMeta.name << "\n";
    out << "replace_tool=" << replaceMeta.name << "\n";
    out << "range_tool=" << rangeMeta.name << "\n";

    bool ok = true;
    auto requireContains = [&](const std::string& haystack,
                               const std::string& needle,
                               const std::string& label) {
        const bool found = haystack.find(needle) != std::string::npos;
        out << label << "=" << (found ? "yes" : "no") << "\n";
        ok = ok && found;
    };

    ok = ok && patchMeta.name == "Edit" && writeMeta.name == "EditWrite" &&
         replaceMeta.name == "EditReplace" && rangeMeta.name == "EditRange";
    requireContains(patchSchema, "\"patch\"", "patch_has_patch_field");
    requireContains(patchSchema, "\"validate_only\"", "patch_has_validate_only");
    requireContains(patchSchema, "---/+++ headers", "patch_requires_headers");
    requireContains(writeSchema, "\"content\"", "write_has_content_field");
    requireContains(replaceSchema, "\"replacements\"", "replace_has_replacements");
    requireContains(rangeSchema, "\"operations\"", "range_has_operations");
    requireContains(rangeSchema, "\"replace_range\"", "range_has_replace_range");
    requireContains(replaceSchema, "\"old_string\"", "replace_has_old_string");
    out << "legacy_old_string_top_level=" << (patchSchema.find("\"old_string\"") == std::string::npos ? "no" : "yes") << "\n";
    ok = ok && patchSchema.find("\"old_string\"") == std::string::npos;
    out << "legacy_files_envelope=" << (patchSchema.find("\"files\"") == std::string::npos ? "no" : "yes") << "\n";
    ok = ok && patchSchema.find("\"files\"") == std::string::npos;
    out << "legacy_delete_file=" << (patchSchema.find("\"delete_file\"") == std::string::npos ? "no" : "yes") << "\n";
    ok = ok && patchSchema.find("\"delete_file\"") == std::string::npos;
    requireContains(patchMeta.description, "Patch-first editing only", "patch_description_is_narrow");

    result.passed = ok;
    result.exitCode = ok ? 0 : 1;
    result.output = out.str();
    if (ok) {
        std::cout << "AUDIT PASSED: edit tool family contract looks explicit and narrow\n";
    } else {
        std::cerr << "AUDIT FAILED: edit tool family contract is missing expected narrow surfaces\n";
    }
    return result;
}

} // namespace firmius::audits
