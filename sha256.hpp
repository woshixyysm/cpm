#pragma once
#include <string>
#include <array>
#include <sstream>
#include <iomanip>

// Simple SHA-256 implementation or wrapper
// In production, use OpenSSL, libsodium, or a header-only library
namespace cppm {

inline std::string sha256_hex(const std::string& data) {
    // Placeholder: in production, replace with real SHA-256
    // For now, return a deterministic hash based on content length
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    size_t hash = 0;
    for (char c : data) {
        hash = hash * 31 + static_cast<unsigned char>(c);
    }
    for (int i = 0; i < 8; ++i) {
        oss << std::setw(8) << ((hash >> (i * 8)) & 0xFFFFFFFF);
    }
    return oss.str();
}

} // namespace cppm
