/**
 * @file MiniPng.h
 * @brief Minimal PNG writer for the placeholder thumbnail generator.
 *
 * Writes an 8-bit RGB PNG using only the standard library. The compressed
 * image data uses zlib "stored" (uncompressed) deflate blocks so we do not
 * need to pull in a zlib/miniz dependency just to make small preview images.
 *
 * The output is a fully valid PNG accepted by every viewer we care about
 * (browsers, image libraries, DCC tools).
 *
 * Only the features needed for thumbnails are implemented:
 *   - IHDR: 8-bit, colour type 2 (RGB), no interlace.
 *   - IDAT: single zlib stream, one or more stored deflate blocks.
 *   - IEND.
 */
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace idtx
{
namespace thumbnails
{

class MiniPng
{
public:
    /**
     * @brief Write an 8-bit RGB PNG to @p path.
     *
     * @param path    Target file path. Overwritten if it exists.
     * @param width   Image width in pixels.
     * @param height  Image height in pixels.
     * @param rgb     Pixel buffer, size == @c width*height*3 (interleaved RGB).
     * @param out_err On failure, filled with a short error message.
     * @return true on success.
     */
    static bool Write(const std::filesystem::path& path,
                      std::uint32_t width, std::uint32_t height,
                      const std::vector<std::uint8_t>& rgb,
                      std::string& out_err)
    {
        if (width == 0 || height == 0)
        {
            out_err = "invalid image dimensions";
            return false;
        }
        if (rgb.size() != static_cast<std::size_t>(width) * height * 3u)
        {
            out_err = "pixel buffer size does not match dimensions";
            return false;
        }

        std::vector<std::uint8_t> out;
        out.reserve(64 + rgb.size() + height /*filter bytes*/ + 4096);

        // PNG signature.
        static constexpr std::uint8_t kSig[8] =
            { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        out.insert(out.end(), std::begin(kSig), std::end(kSig));

        // IHDR ---------------------------------------------------------------
        {
            std::vector<std::uint8_t> ihdr;
            ihdr.reserve(13);
            AppendBE32(ihdr, width);
            AppendBE32(ihdr, height);
            ihdr.push_back(8);      // bit depth
            ihdr.push_back(2);      // colour type: RGB
            ihdr.push_back(0);      // compression: deflate
            ihdr.push_back(0);      // filter: default
            ihdr.push_back(0);      // interlace: none
            WriteChunk(out, "IHDR", ihdr);
        }

        // IDAT ---------------------------------------------------------------
        // Filter byte 0 (None) prepended to each scanline.
        std::vector<std::uint8_t> raw;
        raw.reserve(static_cast<std::size_t>(height) * (1 + width * 3u));
        for (std::uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(0); // filter: None
            const std::uint8_t* row = rgb.data() + static_cast<std::size_t>(y) * width * 3u;
            raw.insert(raw.end(), row, row + width * 3u);
        }

        std::vector<std::uint8_t> zlib_stream = ZlibWrapStored(raw);
        WriteChunk(out, "IDAT", zlib_stream);

        // IEND ---------------------------------------------------------------
        WriteChunk(out, "IEND", {});

        // Persist ------------------------------------------------------------
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            out_err = "failed to open output file for writing";
            return false;
        }
        f.write(reinterpret_cast<const char*>(out.data()),
                static_cast<std::streamsize>(out.size()));
        if (!f)
        {
            out_err = "failed to write image bytes";
            return false;
        }
        return true;
    }

private:
    static void AppendBE32(std::vector<std::uint8_t>& v, std::uint32_t x)
    {
        v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
        v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
        v.push_back(static_cast<std::uint8_t>((x >>  8) & 0xFF));
        v.push_back(static_cast<std::uint8_t>((x >>  0) & 0xFF));
    }

    static void WriteChunk(std::vector<std::uint8_t>& out,
                           const char (&type)[5],
                           const std::vector<std::uint8_t>& data)
    {
        AppendBE32(out, static_cast<std::uint32_t>(data.size()));
        const std::size_t crc_start = out.size();
        out.push_back(static_cast<std::uint8_t>(type[0]));
        out.push_back(static_cast<std::uint8_t>(type[1]));
        out.push_back(static_cast<std::uint8_t>(type[2]));
        out.push_back(static_cast<std::uint8_t>(type[3]));
        out.insert(out.end(), data.begin(), data.end());
        const std::uint32_t crc = Crc32(out.data() + crc_start,
                                        out.size() - crc_start);
        AppendBE32(out, crc);
    }

    /// Wrap a raw byte stream in a zlib container using stored deflate blocks.
    /// This produces a valid RFC-1950/1951 stream with no actual compression,
    /// which is fine for small thumbnails and avoids a zlib dependency.
    static std::vector<std::uint8_t> ZlibWrapStored(const std::vector<std::uint8_t>& raw)
    {
        std::vector<std::uint8_t> z;
        z.reserve(raw.size() + 32);

        // zlib header: CMF=0x78 (deflate, 32K window), FLG chosen so the
        // 16-bit big-endian (CMF*256+FLG) is a multiple of 31. 0x78 0x01
        // satisfies that and advertises no preset dict, fastest level.
        z.push_back(0x78);
        z.push_back(0x01);

        // Split into 65535-byte stored blocks (max size for a single stored
        // deflate block). Each block: 1 byte header, 2 LEN LE, 2 NLEN LE,
        // LEN payload bytes.
        constexpr std::size_t kMax = 65535;
        std::size_t offset = 0;
        while (true)
        {
            const std::size_t remaining = raw.size() - offset;
            const std::size_t chunk     = remaining > kMax ? kMax : remaining;
            const bool last             = (offset + chunk == raw.size());

            z.push_back(last ? 0x01 : 0x00);
            const std::uint16_t len  = static_cast<std::uint16_t>(chunk);
            const std::uint16_t nlen = static_cast<std::uint16_t>(~len);
            z.push_back(static_cast<std::uint8_t>(len  & 0xFF));
            z.push_back(static_cast<std::uint8_t>((len  >> 8) & 0xFF));
            z.push_back(static_cast<std::uint8_t>(nlen & 0xFF));
            z.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xFF));
            if (chunk > 0)
                z.insert(z.end(), raw.begin() + offset,
                                  raw.begin() + offset + chunk);
            offset += chunk;
            if (last) break;
        }

        // Adler-32 of the *uncompressed* data, big-endian.
        const std::uint32_t adler = Adler32(raw.data(), raw.size());
        z.push_back(static_cast<std::uint8_t>((adler >> 24) & 0xFF));
        z.push_back(static_cast<std::uint8_t>((adler >> 16) & 0xFF));
        z.push_back(static_cast<std::uint8_t>((adler >>  8) & 0xFF));
        z.push_back(static_cast<std::uint8_t>((adler >>  0) & 0xFF));
        return z;
    }

    static std::uint32_t Adler32(const std::uint8_t* data, std::size_t len)
    {
        constexpr std::uint32_t kMod = 65521;
        std::uint32_t a = 1, b = 0;
        for (std::size_t i = 0; i < len; ++i)
        {
            a = (a + data[i]) % kMod;
            b = (b + a)       % kMod;
        }
        return (b << 16) | a;
    }

    static std::uint32_t Crc32(const std::uint8_t* data, std::size_t len)
    {
        static const auto table = []() {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t n = 0; n < 256; ++n)
            {
                std::uint32_t c = n;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                t[n] = c;
            }
            return t;
        }();

        std::uint32_t c = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < len; ++i)
            c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
};

} // namespace thumbnails
} // namespace idtx