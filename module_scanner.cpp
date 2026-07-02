#include "module_scanner.hpp"
#include "sha256.hpp"
#include <cctype>

namespace cppm {

std::string ModuleScanner::stripCommentsAndLiterals(const std::string& src) {
    std::string out;
    out.reserve(src.size());

    enum class St { Code, LineComment, BlockComment, Str, Char, RawStr };
    St state = St::Code;
    std::string rawDelim; // for R"delim(...)delim"

    for (size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        char n = (i + 1 < src.size()) ? src[i + 1] : '\0';

        switch (state) {
        case St::Code:
            if (c == '/' && n == '/') { state = St::LineComment; out += ' '; out += ' '; ++i; continue; }
            if (c == '/' && n == '*') { state = St::BlockComment; out += ' '; out += ' '; ++i; continue; }
            if (c == '"') {
                // Detect raw-string prefix: previous non-out char was 'R' immediately before the quote
                // AND the char before that 'R' is not alnum/_ (so it's a real prefix, not e.g. `barR"`).
                bool isRaw = false;
                if (!out.empty() && out.back() == 'R') {
                    char before = (out.size() >= 2) ? out[out.size() - 2] : '\0';
                    if (!(std::isalnum(static_cast<unsigned char>(before)) || before == '_')) {
                        isRaw = true;
                    }
                }
                if (isRaw) {
                    size_t j = i + 1;
                    std::string delim;
                    while (j < src.size() && src[j] != '(') { delim += src[j]; ++j; }
                    rawDelim = delim;
                    state = St::RawStr;
                    out += c; // keep opening quote
                    // copy delimiter + '(' verbatim (they're harmless / part of syntax)
                    for (size_t k = i + 1; k <= j && k < src.size(); ++k) out += src[k];
                    i = j;
                    continue;
                }
                state = St::Str; out += c; continue;
            }
            if (c == '\'') { state = St::Char; out += c; continue; }
            out += c;
            continue;

        case St::LineComment:
            if (c == '\n') { state = St::Code; out += c; } else { out += ' '; }
            continue;

        case St::BlockComment:
            if (c == '*' && n == '/') { state = St::Code; out += ' '; out += ' '; ++i; }
            else { out += (c == '\n' ? '\n' : ' '); }
            continue;

        case St::Str:
            if (c == '\\' && n != '\0') { out += ' '; out += ' '; ++i; continue; }
            if (c == '"') { state = St::Code; out += c; continue; }
            out += (c == '\n' ? '\n' : ' ');
            continue;

        case St::Char:
            if (c == '\\' && n != '\0') { out += ' '; out += ' '; ++i; continue; }
            if (c == '\'') { state = St::Code; out += c; continue; }
            out += (c == '\n' ? '\n' : ' ');
            continue;

        case St::RawStr: {
            std::string closer = ")" + rawDelim + "\"";
            if (src.compare(i, closer.size(), closer) == 0) {
                out += closer;
                i += closer.size() - 1;
                state = St::Code;
            } else {
                out += (c == '\n' ? '\n' : ' ');
            }
            continue;
        }
        }
    }
    return out;
}

ModuleScanner::ScanResult ModuleScanner::scanFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ScanResult r;
        r.errors.push_back("Could not open file: " + path.string());
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return scanSource(ss.str(), path);
}

ModuleScanner::ScanResult ModuleScanner::scanSource(const std::string& rawSrc,
                                                      const std::filesystem::path& path) {
    ScanResult result;
    const std::string src = stripCommentsAndLiterals(rawSrc);

    ModuleUnit unit;
    unit.filePath = path;
    unit.contentHash = sha256_hex(rawSrc); // hash ORIGINAL bytes — cache key must reflect real content

    static const std::regex moduleDeclRe(
        R"(^\s*(export\s+)?module\s+([A-Za-z_][A-Za-z0-9_.]*)\s*(?::\s*([A-Za-z_][A-Za-z0-9_.]*))?\s*;)"
    );
    static const std::regex importDeclRe(
        R"(^\s*(export\s+)?import\s+(?:(:)\s*([A-Za-z_][A-Za-z0-9_.]*)|([A-Za-z_][A-Za-z0-9_.]*)|(<[^>]+>)|("[^"]+"))\s*;)"
    );

    bool foundModuleDecl = false;
    std::smatch m;

    size_t pos = 0;
    while (pos < src.size()) {
        size_t semi = src.find(';', pos);
        if (semi == std::string::npos) break;
        std::string stmt = src.substr(pos, semi - pos + 1);
        pos = semi + 1;

        size_t s0 = stmt.find_first_not_of(" \t\r\n");
        if (s0 == std::string::npos) continue;
        std::string trimmed = stmt.substr(s0);

        if (trimmed.rfind("module", 0) == 0 || trimmed.rfind("export module", 0) == 0) {
            if (std::regex_search(stmt, m, moduleDeclRe)) {
                if (foundModuleDecl) {
                    result.errors.push_back(
                        "Multiple module declarations found in " + path.string());
                    continue;
                }
                foundModuleDecl = true;
                bool isExport = m[1].matched;
                std::string name = m[2].str();
                std::string partition = m[3].matched ? m[3].str() : "";

                unit.logicalName = name;
                if (!partition.empty()) {
                    unit.partitionName = partition;
                    unit.kind = isExport ? UnitKind::Partition : UnitKind::ImplPartition;
                } else {
                    unit.kind = isExport ? UnitKind::PrimaryInterface : UnitKind::Implementation;
                }
            }
            continue;
        }

        if (trimmed.rfind("import", 0) == 0 || trimmed.rfind("export import", 0) == 0) {
            if (std::regex_search(stmt, m, importDeclRe)) {
                ModuleDependency dep;
                dep.isExportImport = m[1].matched;

                if (m[2].matched) {
                    dep.isPartition = true;
                    dep.name = unit.logicalName + ":" + m[3].str();
                } else if (m[4].matched) {
                    dep.name = m[4].str();
                } else if (m[5].matched || m[6].matched) {
                    std::string hdr = m[5].matched ? m[5].str() : m[6].str();
                    dep.name = "header:" + hdr;
                }
                if (!dep.name.empty()) unit.imports.push_back(std::move(dep));
            }
            continue;
        }
    }

    if (!foundModuleDecl) {
        // Ordinary translation unit — nothing to register in the module graph.
        return result;
    }

    // Implicit self-dependency: implementation units (and non-exported
    // partition implementation units) implicitly import their primary
    // module interface per [module.unit].
    if (unit.kind == UnitKind::Implementation || unit.kind == UnitKind::ImplPartition) {
        ModuleDependency self;
        self.name = unit.logicalName;
        self.isPartition = false;
        unit.imports.push_back(self);
    }

    result.units.push_back(std::move(unit));
    return result;
}

} // namespace cppm