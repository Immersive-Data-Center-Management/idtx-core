/**
 * @file UsdFileLocator.h
 * @brief Centralized helper to validate and resolve USD file paths under the
 *        server's `uploads/` root.
 *
 * This consolidates the path/file/USD-extension checks previously found in
 * FileServingController so that both file-serving and session creation use the
 * same rules:
 *   - reject directory traversal
 *   - allow only a restricted character set
 *   - canonicalize the resolved path under `uploads/`
 *   - require a USD extension (.usd, .usda, .usdc, .usdz)
 *
 * The helper is header-only and stateless aside from the configurable
 * uploads root, which defaults to "uploads" (matching the existing
 * controller behaviour).
 */
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <system_error>

#include <idtx/utils/Logger.h>

namespace idtx
{
namespace utils
{

class UsdFileLocator
{
    IDTX_LOG_CATEGORY("UsdFileLocator")

public:
    /**
     * @brief Result of a path/file validation.
     */
    enum class Status
    {
        Ok,
        InvalidEncoding,
        InvalidPath,
        NotFound,
        NotAUsdFile,
        OutsideRoot,
        PermissionDenied
    };

    explicit UsdFileLocator(std::string uploads_root = "uploads")
        : m_root_(std::move(uploads_root))
    {}

    const std::string& GetRoot() const noexcept { return m_root_; }

    /**
     * @brief Validate purely syntactic aspects of a (possibly URL-encoded) path.
     *        Performs percent decoding and checks the character set / no
     *        directory traversal.
     */
    Status ValidatePath(const std::string& filepath, std::string& out_decoded) const
    {
        if (!PercentDecode(filepath, out_decoded))
        {
            IDTX_LOG(IDTX_ERROR, "Unable to percent-decode path: {}", filepath);
            return Status::InvalidEncoding;
        }
        if (out_decoded.find("..") != std::string::npos)
        {
            IDTX_LOG(IDTX_ERROR, "Potential directory traversal: {}", out_decoded);
            return Status::InvalidPath;
        }
        static const std::regex valid_path_regex("^[ a-zA-Z0-9._/-]+$");
        if (!std::regex_match(out_decoded, valid_path_regex))
        {
            IDTX_LOG(IDTX_ERROR, "Path does not match allowed character set: {}", out_decoded);
            return Status::InvalidPath;
        }
        return Status::Ok;
    }

    /**
     * @brief Validate that a file exists, is a USD file, and lies under the
     *        configured uploads root.
     * @param filepath  Possibly URL-encoded relative path, as received over HTTP.
     * @param out_resolved Filled in on success with the canonicalized path
     *                     ("uploads/<decoded>"). Note this is the lexically
     *                     joined path, not the canonical absolute path; callers
     *                     can use it directly with filesystem APIs and USD.
     */
    Status Resolve(const std::string& filepath, std::filesystem::path& out_resolved) const
    {
        std::string decoded;
        Status s = ValidatePath(filepath, decoded);
        if (s != Status::Ok) return s;

        std::filesystem::path candidate = std::filesystem::path(m_root_) / decoded;

        std::error_code ec;
        std::filesystem::path canonical_root = std::filesystem::canonical(m_root_, ec);
        if (ec)
        {
            IDTX_LOG(IDTX_ERROR, "Uploads root '{}' is not accessible: {}",
                     m_root_, ec.message());
            return Status::NotFound;
        }

        std::filesystem::path canonical_file = std::filesystem::canonical(candidate, ec);
        if (ec) return Status::NotFound;

        // Ensure the resolved path is within the uploads directory.
        auto mismatch = std::mismatch(canonical_root.begin(), canonical_root.end(),
                                      canonical_file.begin(), canonical_file.end());
        if (mismatch.first != canonical_root.end()) return Status::OutsideRoot;

        if (!std::filesystem::exists(candidate)) return Status::NotFound;

        if (!IsUsdFilename(candidate.filename().string())) return Status::NotAUsdFile;

        out_resolved = candidate;
        return Status::Ok;
    }

    /**
     * @brief Convenience: pure boolean check.
     */
    bool IsValid(const std::string& filepath) const
    {
        std::filesystem::path resolved;
        return Resolve(filepath, resolved) == Status::Ok;
    }

    /**
     * @brief Validate a client-supplied upload target (directory + filename)
     *        and compose the absolute path it must be written to.
     *
     * The check is intentionally stricter than @c Resolve() because at upload
     * time the target file does not yet exist and @c std::filesystem::canonical
     * would fail. Instead we:
     *   1. Percent-decode both inputs, rejecting encoded slashes.
     *   2. Reject any occurrence of ".." (both raw and decoded).
     *   3. Reject absolute paths, backslashes and any characters outside the
     *      allowed whitelist.
     *   4. Require the filename to be a bare basename (no separators) and to
     *      carry a USD extension.
     *   5. Lexically join `<uploads_root> / <decoded_dir> / <decoded_filename>`
     *      and, after mkdir, canonicalise the parent directory to make sure
     *      no symlink allowed us to escape the uploads root.
     *
     * @param raw_relative_dir  Directory (relative, possibly URL-encoded, may be empty).
     * @param raw_filename      File name (URL-encoded allowed, must end with a USD extension).
     * @param out_full_path     On success: absolute path where the file should be written.
     * @param out_relative_path On success: normalised path relative to the uploads root.
     */
    Status ValidateUploadTarget(const std::string& raw_relative_dir,
                                const std::string& raw_filename,
                                std::filesystem::path& out_full_path,
                                std::filesystem::path& out_relative_path) const
    {
        // ---- Filename ------------------------------------------------------
        std::string decoded_filename;
        if (!PercentDecode(raw_filename, decoded_filename,
                           /*decode_plus_as_space=*/false,
                           /*reject_encoded_slash=*/true))
        {
            IDTX_LOG(IDTX_ERROR, "Unable to percent-decode filename: {}", raw_filename);
            return Status::InvalidEncoding;
        }
        if (decoded_filename.empty()
            || decoded_filename.find("..") != std::string::npos
            || raw_filename.find("..") != std::string::npos
            || decoded_filename.find('/') != std::string::npos
            || decoded_filename.find('\\') != std::string::npos)
        {
            IDTX_LOG(IDTX_ERROR, "Invalid upload filename: {}", raw_filename);
            return Status::InvalidPath;
        }
        static const std::regex valid_name_regex("^[ a-zA-Z0-9._-]+$");
        if (!std::regex_match(decoded_filename, valid_name_regex))
        {
            IDTX_LOG(IDTX_ERROR, "Filename outside allowed character set: {}", decoded_filename);
            return Status::InvalidPath;
        }
        if (!IsUsdFilename(decoded_filename))
        {
            return Status::NotAUsdFile;
        }

        // ---- Directory -----------------------------------------------------
        std::string decoded_dir;
        if (!PercentDecode(raw_relative_dir, decoded_dir,
                           /*decode_plus_as_space=*/false,
                           /*reject_encoded_slash=*/false))
        {
            IDTX_LOG(IDTX_ERROR, "Unable to percent-decode directory: {}", raw_relative_dir);
            return Status::InvalidEncoding;
        }
        // Normalise leading/trailing separators.
        while (!decoded_dir.empty() && (decoded_dir.front() == '/' || decoded_dir.front() == '\\'))
            decoded_dir.erase(decoded_dir.begin());
        while (!decoded_dir.empty() && (decoded_dir.back() == '/' || decoded_dir.back() == '\\'))
            decoded_dir.pop_back();

        if (decoded_dir.find("..") != std::string::npos
            || raw_relative_dir.find("..") != std::string::npos
            || decoded_dir.find('\\') != std::string::npos)
        {
            IDTX_LOG(IDTX_ERROR, "Potential directory traversal in upload dir: {}", raw_relative_dir);
            return Status::InvalidPath;
        }
        if (!decoded_dir.empty())
        {
            static const std::regex valid_dir_regex("^[ a-zA-Z0-9._/-]+$");
            if (!std::regex_match(decoded_dir, valid_dir_regex))
            {
                IDTX_LOG(IDTX_ERROR, "Upload directory outside allowed character set: {}", decoded_dir);
                return Status::InvalidPath;
            }
        }

        // ---- Compose & re-anchor under the uploads root --------------------
        std::filesystem::path root(m_root_);
        std::filesystem::path relative = decoded_dir.empty()
            ? std::filesystem::path(decoded_filename)
            : (std::filesystem::path(decoded_dir) / decoded_filename);

        std::filesystem::path candidate = (root / relative).lexically_normal();

        // Belt-and-braces: the normalised path must still start with the
        // (normalised) root and must not contain any parent-dir component.
        std::filesystem::path normalised_root = root.lexically_normal();
        auto mismatch_it = std::mismatch(normalised_root.begin(), normalised_root.end(),
                                         candidate.begin(), candidate.end());
        if (mismatch_it.first != normalised_root.end())
        {
            IDTX_LOG(IDTX_ERROR, "Composed upload path escapes root: {}", candidate.string());
            return Status::OutsideRoot;
        }
        for (const auto& part : relative)
        {
            if (part == "..") return Status::OutsideRoot;
        }

        out_full_path     = candidate;
        out_relative_path = relative;
        return Status::Ok;
    }

    /**
     * @brief Ensure the configured uploads root exists and is writable.
     *
     * Intended to be called once at startup so that later per-request calls to
     * @c EnsureDirectoryInsideRoot never fail merely because the root itself is
     * missing (e.g. a freshly-mounted, empty volume in a container). The
     * underlying @c std::error_code is surfaced in the log so permission vs.
     * missing-path failures are immediately diagnosable.
     *
     * @param ec  Populated with the failing operation's error code, if any.
     * @return @c Status::Ok when the root exists and is writable; otherwise
     *         @c PermissionDenied or @c NotFound with @p ec describing why.
     */
    Status EnsureRootExists(std::error_code& ec) const
    {
        ec.clear();
        std::filesystem::create_directories(m_root_, ec);
        if (ec)
        {
            const Status s = ClassifyFilesystemError(ec);
            IDTX_LOG(IDTX_ERROR, "Failed to create uploads root '{}': {} ({})",
                     m_root_, ec.message(), StatusToString(s));
            return s;
        }

        // Probe writability by creating (and removing) a temporary marker.
        const std::filesystem::path probe =
            std::filesystem::path(m_root_) / ".idtx-write-test";
        {
            std::error_code probe_ec;
            std::filesystem::remove(probe, probe_ec); // best-effort pre-clean
        }
        std::ofstream test(probe, std::ios::out | std::ios::trunc);
        if (!test)
        {
            ec = std::make_error_code(std::errc::permission_denied);
            IDTX_LOG(IDTX_ERROR,
                     "Uploads root '{}' exists but is not writable by this process.",
                     m_root_);
            return Status::PermissionDenied;
        }
        test.close();
        std::error_code rmec;
        std::filesystem::remove(probe, rmec);

        IDTX_LOG(IDTX_INFO, "Uploads root ready at '{}'.", m_root_);
        return Status::Ok;
    }

    /**
     * @brief Create a directory and re-verify it stays inside the uploads root.
     *        Used both for the upload target directory and for its @c thumbs/
     *        subdirectory. Follows the same "canonicalise the parent" pattern
     *        that @c ValidateUploadTarget applies for files.
     */
    Status EnsureDirectoryInsideRoot(const std::filesystem::path& dir,
                                     std::error_code& ec) const
    {
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            const Status s = ClassifyFilesystemError(ec);
            IDTX_LOG(IDTX_ERROR, "Failed to create directory '{}': {} ({})",
                     dir.string(), ec.message(), StatusToString(s));
            return s;
        }

        std::filesystem::path canonical_root = std::filesystem::canonical(m_root_, ec);
        if (ec)
        {
            const Status s = ClassifyFilesystemError(ec);
            IDTX_LOG(IDTX_ERROR, "Uploads root '{}' is not accessible: {} ({})",
                     m_root_, ec.message(), StatusToString(s));
            return s;
        }

        std::filesystem::path canonical_dir = std::filesystem::canonical(dir, ec);
        if (ec)
        {
            const Status s = ClassifyFilesystemError(ec);
            IDTX_LOG(IDTX_ERROR, "Directory '{}' is not accessible: {} ({})",
                     dir.string(), ec.message(), StatusToString(s));
            return s;
        }

        auto mismatch_it = std::mismatch(canonical_root.begin(), canonical_root.end(),
                                         canonical_dir.begin(), canonical_dir.end());
        if (mismatch_it.first != canonical_root.end())
        {
            IDTX_LOG(IDTX_ERROR, "Directory escapes root after canonicalisation: {}",
                     canonical_dir.string());
            return Status::OutsideRoot;
        }
        return Status::Ok;
    }

    /**
     * @brief Filename extension check.
     */
    static bool IsUsdFilename(const std::string& filename) noexcept
    {
        return filename.ends_with(".usd")
            || filename.ends_with(".usda")
            || filename.ends_with(".usdc")
            || filename.ends_with(".usdz");
    }

    /**
     * @brief Percent-decode for URL paths. Returns false on malformed input.
     *        Mirrors the behaviour of the helper that previously lived inside
     *        FileServingController.cpp.
     */
    static bool PercentDecode(const std::string& in,
                              std::string& out,
                              bool decode_plus_as_space = false,
                              bool reject_encoded_slash = false)
    {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            return -1;
        };

        out.clear();
        out.reserve(in.size());
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            char c = in[i];
            if (c == '%')
            {
                if (i + 2 >= in.size()) return false;
                int hi = hex(in[i + 1]);
                int lo = hex(in[i + 2]);
                if (hi < 0 || lo < 0) return false;
                unsigned char byte = static_cast<unsigned char>((hi << 4) | lo);
                if (reject_encoded_slash && byte == '/') return false;
                out.push_back(static_cast<char>(byte));
                i += 2;
            }
            else if (c == '+' && decode_plus_as_space)
            {
                out.push_back(' ');
            }
            else
            {
                out.push_back(c);
            }
        }
        return true;
    }

    static const char* StatusToString(Status s) noexcept
    {
        switch (s)
        {
            case Status::Ok:               return "Ok";
            case Status::InvalidEncoding:  return "InvalidEncoding";
            case Status::InvalidPath:      return "InvalidPath";
            case Status::NotFound:         return "NotFound";
            case Status::NotAUsdFile:      return "NotAUsdFile";
            case Status::OutsideRoot:      return "OutsideRoot";
            case Status::PermissionDenied: return "PermissionDenied";
        }
        return "Unknown";
    }

private:
    /**
     * @brief Map a filesystem @c std::error_code onto the locator's @c Status.
     *        Permission-related errors get their own status so callers can log
     *        (and, if they choose, respond) differently from a genuinely
     *        missing path.
     */
    static Status ClassifyFilesystemError(const std::error_code& ec) noexcept
    {
        if (ec == std::errc::permission_denied
            || ec == std::errc::operation_not_permitted
            || ec == std::errc::read_only_file_system)
        {
            return Status::PermissionDenied;
        }
        return Status::NotFound;
    }

    std::string m_root_;
};

} // namespace utils
} // namespace idtx