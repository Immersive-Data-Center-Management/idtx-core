#include "thumbnails/PlaceholderThumbnailGenerator.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include <pxr/pxr.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>

#include <idtx/utils/Logger.h>

#include "thumbnails/MiniPng.h"

namespace idtx
{
namespace thumbnails
{

namespace {

/// Deterministic 32-bit FNV-1a hash. Used only to derive a visually stable
/// tint per file — never for security.
std::uint32_t Fnv1aHash(const std::string& s) noexcept
{
    std::uint32_t h = 0x811C9DC5u;
    for (char c : s)
    {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x01000193u;
    }
    return h;
}

/// Convert a hash into an (R,G,B) tint biased toward the mid-brightness range
/// so foreground text painted on top stays legible.
void HashToTint(std::uint32_t h,
                std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) noexcept
{
    r = static_cast<std::uint8_t>(80 + ((h >>  0) & 0x7F));
    g = static_cast<std::uint8_t>(80 + ((h >>  8) & 0x7F));
    b = static_cast<std::uint8_t>(80 + ((h >> 16) & 0x7F));
}

/// 3x5 bitmap font: bit 2 (MSB of the low 3 bits) = leftmost column, so the
/// binary literals below can be read as the actual visual shape of each
/// glyph. Only ASCII letters, digits and a few punctuation chars are
/// supported; anything else renders as a solid rectangle so the thumbnail
/// never has blank spots.
struct Glyph { std::uint8_t rows[5]; };

const Glyph& GlyphFor(char ch) noexcept
{
    static const Glyph kSolid { {0b111, 0b111, 0b111, 0b111, 0b111} };
    static const Glyph kSpace { {0b000, 0b000, 0b000, 0b000, 0b000} };
    static const Glyph kDot   { {0b000, 0b000, 0b000, 0b000, 0b010} };
    static const Glyph kDash  { {0b000, 0b000, 0b111, 0b000, 0b000} };
    static const Glyph kUnder { {0b000, 0b000, 0b000, 0b000, 0b111} };

    static const Glyph k0 { {0b111, 0b101, 0b101, 0b101, 0b111} };
    static const Glyph k1 { {0b010, 0b110, 0b010, 0b010, 0b111} };
    static const Glyph k2 { {0b111, 0b001, 0b111, 0b100, 0b111} };
    static const Glyph k3 { {0b111, 0b001, 0b111, 0b001, 0b111} };
    static const Glyph k4 { {0b101, 0b101, 0b111, 0b001, 0b001} };
    static const Glyph k5 { {0b111, 0b100, 0b111, 0b001, 0b111} };
    static const Glyph k6 { {0b111, 0b100, 0b111, 0b101, 0b111} };
    static const Glyph k7 { {0b111, 0b001, 0b010, 0b010, 0b010} };
    static const Glyph k8 { {0b111, 0b101, 0b111, 0b101, 0b111} };
    static const Glyph k9 { {0b111, 0b101, 0b111, 0b001, 0b111} };

    static const Glyph kA { {0b010, 0b101, 0b111, 0b101, 0b101} };
    static const Glyph kB { {0b110, 0b101, 0b110, 0b101, 0b110} };
    static const Glyph kC { {0b111, 0b100, 0b100, 0b100, 0b111} };
    static const Glyph kD { {0b110, 0b101, 0b101, 0b101, 0b110} };
    static const Glyph kE { {0b111, 0b100, 0b110, 0b100, 0b111} };
    static const Glyph kF { {0b111, 0b100, 0b110, 0b100, 0b100} };
    static const Glyph kG { {0b111, 0b100, 0b101, 0b101, 0b111} };
    static const Glyph kH { {0b101, 0b101, 0b111, 0b101, 0b101} };
    static const Glyph kI { {0b111, 0b010, 0b010, 0b010, 0b111} };
    static const Glyph kJ { {0b111, 0b001, 0b001, 0b101, 0b111} };
    static const Glyph kK { {0b101, 0b110, 0b100, 0b110, 0b101} };
    static const Glyph kL { {0b100, 0b100, 0b100, 0b100, 0b111} };
    static const Glyph kM { {0b101, 0b111, 0b111, 0b101, 0b101} };
    static const Glyph kN { {0b101, 0b111, 0b111, 0b111, 0b101} };
    static const Glyph kO { {0b111, 0b101, 0b101, 0b101, 0b111} };
    static const Glyph kP { {0b111, 0b101, 0b111, 0b100, 0b100} };
    static const Glyph kQ { {0b111, 0b101, 0b101, 0b111, 0b011} };
    static const Glyph kR { {0b111, 0b101, 0b110, 0b101, 0b101} };
    static const Glyph kS { {0b111, 0b100, 0b111, 0b001, 0b111} };
    static const Glyph kT { {0b111, 0b010, 0b010, 0b010, 0b010} };
    static const Glyph kU { {0b101, 0b101, 0b101, 0b101, 0b111} };
    static const Glyph kV { {0b101, 0b101, 0b101, 0b101, 0b010} };
    static const Glyph kW { {0b101, 0b101, 0b111, 0b111, 0b101} };
    static const Glyph kX { {0b101, 0b101, 0b010, 0b101, 0b101} };
    static const Glyph kY { {0b101, 0b101, 0b010, 0b010, 0b010} };
    static const Glyph kZ { {0b111, 0b001, 0b010, 0b100, 0b111} };

    switch (ch)
    {
        case ' ': return kSpace;
        case '.': return kDot;
        case '-': return kDash;
        case '_': return kUnder;
        case '0': return k0; case '1': return k1; case '2': return k2;
        case '3': return k3; case '4': return k4; case '5': return k5;
        case '6': return k6; case '7': return k7; case '8': return k8;
        case '9': return k9;
        case 'a': case 'A': return kA;
        case 'b': case 'B': return kB;
        case 'c': case 'C': return kC;
        case 'd': case 'D': return kD;
        case 'e': case 'E': return kE;
        case 'f': case 'F': return kF;
        case 'g': case 'G': return kG;
        case 'h': case 'H': return kH;
        case 'i': case 'I': return kI;
        case 'j': case 'J': return kJ;
        case 'k': case 'K': return kK;
        case 'l': case 'L': return kL;
        case 'm': case 'M': return kM;
        case 'n': case 'N': return kN;
        case 'o': case 'O': return kO;
        case 'p': case 'P': return kP;
        case 'q': case 'Q': return kQ;
        case 'r': case 'R': return kR;
        case 's': case 'S': return kS;
        case 't': case 'T': return kT;
        case 'u': case 'U': return kU;
        case 'v': case 'V': return kV;
        case 'w': case 'W': return kW;
        case 'x': case 'X': return kX;
        case 'y': case 'Y': return kY;
        case 'z': case 'Z': return kZ;
        default:  return kSolid;
    }
}

/// Draw a solid rectangle in the RGB buffer.
void FillRect(std::vector<std::uint8_t>& rgb,
              std::uint32_t w, std::uint32_t h,
              std::uint32_t x0, std::uint32_t y0,
              std::uint32_t rw, std::uint32_t rh,
              std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const std::uint32_t x1 = std::min(w, x0 + rw);
    const std::uint32_t y1 = std::min(h, y0 + rh);
    for (std::uint32_t y = y0; y < y1; ++y)
    {
        std::uint8_t* row = rgb.data() + (static_cast<std::size_t>(y) * w + x0) * 3u;
        for (std::uint32_t x = x0; x < x1; ++x)
        {
            row[0] = r; row[1] = g; row[2] = b;
            row += 3;
        }
    }
}

/// Render a single character at (x0, y0) with per-cell scale `scale`.
void DrawChar(std::vector<std::uint8_t>& rgb,
              std::uint32_t w, std::uint32_t h,
              std::uint32_t x0, std::uint32_t y0, std::uint32_t scale,
              char ch,
              std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const Glyph& glyph = GlyphFor(ch);
    for (std::uint32_t row = 0; row < 5; ++row)
    {
        for (std::uint32_t col = 0; col < 3; ++col)
        {
            // Bit 2 is the leftmost column (see Glyph declaration above), so
            // shift by (2 - col) instead of col to avoid a horizontal mirror.
            if (glyph.rows[row] & (1u << (2u - col)))
            {
                FillRect(rgb, w, h,
                         x0 + col * scale,
                         y0 + row * scale,
                         scale, scale, r, g, b);
            }
        }
    }
}

/// Draw a horizontally centered string.
void DrawStringCentered(std::vector<std::uint8_t>& rgb,
                        std::uint32_t w, std::uint32_t h,
                        std::uint32_t y0, std::uint32_t scale,
                        const std::string& text,
                        std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    if (text.empty()) return;
    const std::uint32_t char_w  = 3u * scale;
    const std::uint32_t spacing = scale;
    const std::uint32_t total_w =
          static_cast<std::uint32_t>(text.size()) * char_w
        + static_cast<std::uint32_t>(text.size() - 1) * spacing;
    if (total_w >= w) return;
    std::uint32_t x = (w - total_w) / 2u;
    for (char ch : text)
    {
        DrawChar(rgb, w, h, x, y0, scale, ch, r, g, b);
        x += char_w + spacing;
    }
}

/// Fill the buffer with a diagonal gradient between two colours.
void PaintGradient(std::vector<std::uint8_t>& rgb,
                   std::uint32_t w, std::uint32_t h,
                   std::uint8_t tr, std::uint8_t tg, std::uint8_t tb)
{
    // Anchor the dark corner at ~15% of the tint; that keeps the file visually
    // distinct without ever going fully black.
    const int dr = tr / 6;
    const int dg = tg / 6;
    const int db = tb / 6;
    const int max_axis = static_cast<int>(w + h);
    for (std::uint32_t y = 0; y < h; ++y)
    {
        for (std::uint32_t x = 0; x < w; ++x)
        {
            const int t = static_cast<int>(x + y);
            const int r = dr + ((tr - dr) * t) / max_axis;
            const int g = dg + ((tg - dg) * t) / max_axis;
            const int b = db + ((tb - db) * t) / max_axis;
            std::uint8_t* px = rgb.data() + (static_cast<std::size_t>(y) * w + x) * 3u;
            px[0] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
            px[1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
            px[2] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
        }
    }
}

/// Extract a short, single-line description of the USD file so the thumbnail
/// carries a hint of *what* is inside. Never throws: any SDF/USD failure is
/// swallowed and we fall back to the extension.
std::string DescribeUsdFile(const std::filesystem::path& usd_file) noexcept
{
    try
    {
        pxr::SdfLayerRefPtr layer =
            pxr::SdfLayer::FindOrOpen(usd_file.string());
        if (!layer) return "USD";

        const pxr::TfToken default_prim = layer->GetDefaultPrim();
        if (!default_prim.IsEmpty())
        {
            return default_prim.GetString();
        }

        const auto& root_names = layer->GetRootPrims();
        if (!root_names.empty())
        {
            return root_names.front()->GetName();
        }
    }
    catch (...) { /* fall through */ }
    return "USD";
}

/// Trim/upper-case a candidate label so the tiny bitmap font renders it well.
std::string PrepareLabel(std::string s, std::size_t max_chars)
{
    // The bitmap font only knows a-z, 0-9 and a few punctuation marks; strip
    // anything else so unknown characters don't render as solid blocks.
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_' || c == ' ';
        if (ok) out.push_back(c);
    }
    if (out.size() > max_chars) out.resize(max_chars);
    return out;
}

} // namespace

bool PlaceholderThumbnailGenerator::Generate(const std::filesystem::path& usd_file,
                                             const std::filesystem::path& out_path,
                                             std::string& out_error)
{
    try
    {
        const std::uint32_t w = m_size_;
        const std::uint32_t h = m_size_;
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3u, 0);

        // Derive tint from the filename so different files look different.
        const std::string basename = usd_file.filename().string();
        std::uint8_t tr = 0, tg = 0, tb = 0;
        HashToTint(Fnv1aHash(basename), tr, tg, tb);
        PaintGradient(rgb, w, h, tr, tg, tb);

        // Framing border.
        const std::uint32_t border = std::max<std::uint32_t>(2u, w / 64u);
        FillRect(rgb, w, h, 0, 0, w, border, 20, 20, 20);
        FillRect(rgb, w, h, 0, h - border, w, border, 20, 20, 20);
        FillRect(rgb, w, h, 0, 0, border, h, 20, 20, 20);
        FillRect(rgb, w, h, w - border, 0, border, h, 20, 20, 20);

        // Text: two centred lines — the (stem of the) filename, then a
        // short label extracted from the USD layer if available.
        //
        // Scale so both lines fit horizontally regardless of image size.
        const std::string stem  = PrepareLabel(usd_file.stem().string(),  16);
        const std::string label = PrepareLabel(DescribeUsdFile(usd_file), 16);

        auto max_scale = [&](const std::string& s) -> std::uint32_t {
            if (s.empty()) return 0;
            // total width for N chars at scale k: N*3k + (N-1)*k = (4N-1)*k
            const std::uint32_t denom =
                static_cast<std::uint32_t>(4u * s.size() - 1u);
            const std::uint32_t avail = (w * 4u) / 5u; // 80% of the width
            return std::max<std::uint32_t>(1u, avail / denom);
        };

        const std::uint32_t s1 = max_scale(stem);
        const std::uint32_t s2 = max_scale(label);

        // Vertical layout: two lines centred around the middle.
        const std::uint32_t line1_h = 5u * s1;
        const std::uint32_t line2_h = 5u * s2;
        const std::uint32_t gap     = std::max<std::uint32_t>(s1, s2);
        const std::uint32_t y1      = (h - line1_h - line2_h - gap) / 2u;
        const std::uint32_t y2      = y1 + line1_h + gap;

        DrawStringCentered(rgb, w, h, y1, s1, stem,  255, 255, 255);
        DrawStringCentered(rgb, w, h, y2, s2, label, 220, 220, 220);

        std::string png_err;
        if (!MiniPng::Write(out_path, w, h, rgb, png_err))
        {
            out_error = "PNG write failed: " + png_err;
            return false;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
    }
    catch (...)
    {
        out_error = "unknown error while generating thumbnail";
        return false;
    }
}

} // namespace thumbnails
} // namespace idtx
