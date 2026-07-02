// bmi_cache.hpp
#pragma once
#include <string>
#include <filesystem>
#include <optional>

namespace cppm {

struct CacheKeyInputs {
    std::string compilerId;      // "clang", "gcc", "msvc"
    std::string compilerVersion; // "18.1.0"
    std::string flagsNormalized; // sorted/canonicalized flag string
    std::string sourceContentHash; // from ModuleUnit::contentHash
    std::string dependencyBmiFingerprint; // hash of upstream BMI keys, for transitive invalidation
};

class BmiCacheManager {
public:
    explicit BmiCacheManager(std::filesystem::path userCacheRoot,
                              std::optional<std::filesystem::path> systemCacheRoot = std::nullopt)
        : userRoot_(std::move(userCacheRoot)), systemRoot_(std::move(systemCacheRoot)) {}

    std::string computeKey(const CacheKeyInputs& in) const {
        std::string combined = in.compilerId + "|" + in.compilerVersion + "|" +
                                in.flagsNormalized + "|" + in.sourceContentHash + "|" +
                                in.dependencyBmiFingerprint;
        return sha256Hex(combined);
    }

    // Checks user cache first, then falls back to shared/system cache (read-only).
    std::optional<std::filesystem::path> lookup(const std::string& key) const {
        auto userPath = userRoot_ / key.substr(0, 2) / (key + ".bmi");
        if (std::filesystem::exists(userPath)) return userPath;
        if (systemRoot_) {
            auto sysPath = *systemRoot_ / key.substr(0, 2) / (key + ".bmi");
            if (std::filesystem::exists(sysPath)) return sysPath;
        }
        return std::nullopt;
    }

    std::filesystem::path store(const std::string& key, const std::filesystem::path& producedBmi) const {
        auto dest = userRoot_ / key.substr(0, 2) / (key + ".bmi");
        std::filesystem::create_directories(dest.parent_path());
        std::filesystem::copy_file(producedBmi, dest,
            std::filesystem::copy_options::overwrite_existing);
        return dest;
    }

private:
    std::filesystem::path userRoot_;
    std::optional<std::filesystem::path> systemRoot_;
    static std::string sha256Hex(const std::string& data); // impl via OpenSSL/libsodium
};

} // namespace cppm