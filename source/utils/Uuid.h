/**
 * @file Uuid.h
 * @brief Lightweight UUIDv4 generator used for session identifiers.
 *
 * The implementation uses std::random_device + std::mt19937_64 and produces
 * a string in canonical 8-4-4-4-12 hex form. It is not cryptographically
 * strong, which is fine for opaque session identifiers; for any token that
 * needs to be unguessable we will use a dedicated secure source.
 */
#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <string>

namespace idtx
{
namespace utils
{

inline std::string GenerateUuidV4()
{
    static thread_local std::mt19937_64 rng{std::random_device{}()};

    std::array<std::uint8_t, 16> bytes{};
    std::uint64_t a = rng();
    std::uint64_t b = rng();
    std::memcpy(bytes.data(),     &a, 8);
    std::memcpy(bytes.data() + 8, &b, 8);

    // RFC 4122 v4: set version (4) and variant (10xx)
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(36);
    std::size_t pos = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[pos++] = '-';
        out[pos++] = hex[(bytes[i] >> 4) & 0x0F];
        out[pos++] = hex[bytes[i] & 0x0F];
    }
    return out;
}

} // namespace utils
} // namespace idtx