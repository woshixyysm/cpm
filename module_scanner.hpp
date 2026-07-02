// module_scanner.hpp
#pragma once
#include "module_unit.hpp"
#include <regex>
#include <fstream>
#include <sstream>
#include <optional>

namespace cppm {

class ModuleScanner {
public:
    // Strips // and /* */ comments and string/char literal contents,
    // replacing their interiors with spaces so column positions and
    // line counts are preserved (important for diagnostics).
    static std::string stripCommentsAndLiterals(const std::string& src);

    struct ScanResult {
        std::vector<ModuleUnit> units; // usually 1, but a file could (rarely) be scanned for multiple decls
        std::vector<std::string> errors;
    };

    static ScanResult scanFile(const std::filesystem::path& path);

    static ScanResult scanSource(const std::string& rawSrc, const std::filesystem::path& path);

private:
    // Placeholder — wire to a real SHA-256 (e.g. OpenSSL EVP or a header-only impl).
    static std::string sha256(const std::string& data);
};

} // namespace cppm