#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cppm {

struct ExportedPartitionInfo {
    std::string name; // partition name only, e.g. "impl_detail"
    std::string file; // relative path
};

struct PackageManifest {
    std::string scope;                 // "acme"  (may be empty for unscoped packages)
    std::string name;                  // "vectormath"
    std::string version;               // "1.2.0"
    std::string primaryModuleName;     // mangled: "acme.vectormath"
    std::vector<ExportedPartitionInfo> partitions;
    std::vector<std::string> moduleDependencies; // logical module names (not lib names)
    std::string cxxStandard;           // "20" | "23"
    std::vector<std::string> compilerConstraints; // e.g. ">=clang-17", ">=msvc-19.38"
    std::vector<std::string> sourceFiles; // relative paths to .cppm/.ixx/.cpp

    std::string packageId() const {
        std::string base = scope.empty() ? name : (scope + "/" + name);
        return base + "@" + version;
    }

    // Minimal hand-rolled JSON writer/reader — no external dependency.
    // For production use, swap in nlohmann::json or toml++.
    std::string toJson() const {
        std::ostringstream o;
        o << "{\n";
        o << "  \"scope\": \"" << scope << "\",\n";
        o << "  \"name\": \"" << name << "\",\n";
        o << "  \"version\": \"" << version << "\",\n";
        o << "  \"primaryModuleName\": \"" << primaryModuleName << "\",\n";
        o << "  \"cxxStandard\": \"" << cxxStandard << "\",\n";

        o << "  \"partitions\": [";
        for (size_t i = 0; i < partitions.size(); ++i) {
            o << "{\"name\": \"" << partitions[i].name << "\", \"file\": \"" << partitions[i].file << "\"}";
            if (i + 1 < partitions.size()) o << ", ";
        }
        o << "],\n";

        auto writeArr = [&](const char* key, const std::vector<std::string>& v) {
            o << "  \"" << key << "\": [";
            for (size_t i = 0; i < v.size(); ++i) {
                o << "\"" << v[i] << "\"";
                if (i + 1 < v.size()) o << ", ";
            }
            o << "]";
        };
        writeArr("moduleDependencies", moduleDependencies);
        o << ",\n";
        writeArr("compilerConstraints", compilerConstraints);
        o << ",\n";
        writeArr("sourceFiles", sourceFiles);
        o << "\n}\n";
        return o.str();
    }

    static PackageManifest fromJsonFile(const std::filesystem::path& path) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("Cannot open manifest: " + path.string());
        std::ostringstream ss;
        ss << in.rdbuf();
        return fromJsonString(ss.str());
    }

    // Extremely small, forgiving JSON subset parser sufficient for the
    // manifest schema above. Not a general JSON parser.
    static PackageManifest fromJsonString(const std::string& text) {
        PackageManifest m;
        m.scope = extractString(text, "scope");
        m.name = extractString(text, "name");
        m.version = extractString(text, "version");
        m.primaryModuleName = extractString(text, "primaryModuleName");
        m.cxxStandard = extractString(text, "cxxStandard");
        m.moduleDependencies = extractStringArray(text, "moduleDependencies");
        m.compilerConstraints = extractStringArray(text, "compilerConstraints");
        m.sourceFiles = extractStringArray(text, "sourceFiles");
        m.partitions = extractPartitions(text);
        return m;
    }

private:
    static std::string extractString(const std::string& text, const std::string& key) {
        std::string pattern = "\"" + key + "\"";
        size_t p = text.find(pattern);
        if (p == std::string::npos) return "";
        size_t colon = text.find(':', p);
        size_t q1 = text.find('"', colon + 1);
        size_t q2 = text.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) return "";
        return text.substr(q1 + 1, q2 - q1 - 1);
    }

    static std::vector<std::string> extractStringArray(const std::string& text, const std::string& key) {
        std::vector<std::string> result;
        std::string pattern = "\"" + key + "\"";
        size_t p = text.find(pattern);
        if (p == std::string::npos) return result;
        size_t open = text.find('[', p);
        size_t close = text.find(']', open);
        if (open == std::string::npos || close == std::string::npos) return result;
        std::string body = text.substr(open + 1, close - open - 1);

        size_t i = 0;
        while (i < body.size()) {
            size_t q1 = body.find('"', i);
            if (q1 == std::string::npos) break;
            size_t q2 = body.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            result.push_back(body.substr(q1 + 1, q2 - q1 - 1));
            i = q2 + 1;
        }
        return result;
    }

    static std::vector<ExportedPartitionInfo> extractPartitions(const std::string& text) {
        std::vector<ExportedPartitionInfo> result;
        size_t p = text.find("\"partitions\"");
        if (p == std::string::npos) return result;
        size_t open = text.find('[', p);
        size_t close = text.find(']', open);
        if (open == std::string::npos || close == std::string::npos) return result;
        std::string body = text.substr(open + 1, close - open - 1);

        size_t i = 0;
        while (true) {
            size_t objStart = body.find('{', i);
            if (objStart == std::string::npos) break;
            size_t objEnd = body.find('}', objStart);
            if (objEnd == std::string::npos) break;
            std::string obj = body.substr(objStart, objEnd - objStart + 1);
            ExportedPartitionInfo pi;
            pi.name = extractString(obj, "name");
            pi.file = extractString(obj, "file");
            result.push_back(pi);
            i = objEnd + 1;
        }
        return result;
    }
};

} // namespace cppm