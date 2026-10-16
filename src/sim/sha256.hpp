#pragma once
// SHA-256 (FIPS 180-4): the fingerprint of a mod's native code that the player's consent is tied to.
#include <cstddef>
#include <cstdint>
#include <string>

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t size);
    void update(const std::string& s) { update(s.data(), s.size()); }
    std::string hex();  // finishes: 64 lowercase hex digits

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

inline std::string sha256Hex(const std::string& data) {
    Sha256 s;
    s.update(data);
    return s.hex();
}
