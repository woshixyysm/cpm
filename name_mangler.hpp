#pragma once
#include <string>
#include <cctype>
#include <unordered_map>
#include <stdexcept>

namespace cppm {

// Maps registry-style scoped package names ("@scope/module-name") to legal
// C++ module identifiers ("scope.module_name"). C++ module names are
// sequences of dot-separated identifiers ([A-Za-z_][A-Za-z0-9_]*); registry
// names permit characters like '-' that aren't legal there, so we sanitize
// deterministically and keep a reverse lookup table for diagnostics/registry
// queries (since mangling is not always losslessly invertible).
class NameMangler {
public:
    static std::string sanitizeSegment(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                out += c;
            } else {
                out += '_'; // '-', '.', etc. collapse to '_'
            }
        }
        if (out.empty()) out = "_";
        if (std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
        return out;
    }

    std::string mangle(const std::string& scope, const std::string& name) {
        std::string s = sanitizeSegment(scope);
        std::string n = sanitizeSegment(name);
        std::string mangled = scope.empty() ? n : (s + "." + n);

        auto it = reverse_.find(mangled);
        if (it != reverse_.end() && it->second != (scope + "/" + name)) {
            throw std::runtime_error(
                "Module name collision: '" + mangled + "' already claimed by '" +
                it->second + "', cannot also map '" + scope + "/" + name + "'");
        }
        reverse_[mangled] = scope + "/" + name;
        return mangled;
    }

    // Returns the original "scope/name" that produced a given mangled
    // identifier, if known to this mangler instance.
    std::optional<std::string> unmangle(const std::string& mangledName) const {
        auto it = reverse_.find(mangledName);
        if (it == reverse_.end()) return std::nullopt;
        return it->second;
    }

private:
    std::unordered_map<std::string, std::string> reverse_; // mangled -> "scope/name"
};

} // namespace cppm