// tests/UsdFileLocatorTests.cpp
//
// Checks UsdFileLocator::ToUploadsRelative, which gives every file under the
// uploads root one canonical name, whatever spelling a request or a reference
// used. No server is involved; the files live in a temporary directory.

#include "thirdparty/doctest/doctest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "utils/UsdFileLocator.h"

using idtx::utils::UsdFileLocator;

namespace
{

namespace fs = std::filesystem;

/// A temporary uploads root with a few USD files, removed again afterwards.
class TempUploads
{
public:
    TempUploads()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_base_ = fs::temp_directory_path() / ("idtx-locator-" + std::to_string(stamp));
        m_root_ = m_base_ / "uploads";
        fs::create_directories(m_root_ / "dir");
        Touch(m_root_ / "a.usda");
        Touch(m_root_ / "dir" / "b.usda");
        Touch(m_root_ / "my asset.usda");
        Touch(m_base_ / "outside.usda");
    }

    ~TempUploads()
    {
        std::error_code ec;
        fs::remove_all(m_base_, ec);
    }

    TempUploads(const TempUploads&)            = delete;
    TempUploads& operator=(const TempUploads&) = delete;

    const fs::path& base() const noexcept { return m_base_; }
    const fs::path& root() const noexcept { return m_root_; }

private:
    static void Touch(const fs::path& path)
    {
        std::ofstream(path) << "#usda 1.0\n";
    }

    fs::path m_base_;
    fs::path m_root_;
};

/// ToUploadsRelative() of @p path, or "<status>" if it fails.
std::string Canonical(const UsdFileLocator& locator, const fs::path& path)
{
    std::string relative;
    const auto status = locator.ToUploadsRelative(path, relative);
    if (status != UsdFileLocator::Status::Ok)
        return std::string("<") + UsdFileLocator::StatusToString(status) + ">";
    return relative;
}

/// Resolve() a request string, then ToUploadsRelative() the result.
std::string CanonicalOfRequest(const UsdFileLocator& locator, const std::string& request)
{
    fs::path resolved;
    const auto status = locator.Resolve(request, resolved);
    if (status != UsdFileLocator::Status::Ok)
        return std::string("<") + UsdFileLocator::StatusToString(status) + ">";
    return Canonical(locator, resolved);
}

} // namespace

TEST_CASE("locator: one file has one canonical name, however it is spelled")
{
    TempUploads uploads;
    const UsdFileLocator locator(uploads.root().string());

    CHECK(Canonical(locator, uploads.root() / "a.usda") == "a.usda");
    CHECK(Canonical(locator, uploads.root() / "." / "a.usda") == "a.usda");
    CHECK(Canonical(locator, uploads.root() / "dir" / ".." / "a.usda") == "a.usda");
    CHECK(Canonical(locator, fs::path(uploads.root().string() + "//dir/./b.usda")) == "dir/b.usda");

    // Request strings as clients send them: percent-encoded, with "./" or
    // repeated separators.
    CHECK(CanonicalOfRequest(locator, "a.usda") == "a.usda");
    CHECK(CanonicalOfRequest(locator, "./a.usda") == "a.usda");
    CHECK(CanonicalOfRequest(locator, "dir//b.usda") == "dir/b.usda");
    CHECK(CanonicalOfRequest(locator, "dir/./b.usda") == "dir/b.usda");
    CHECK(CanonicalOfRequest(locator, "my%20asset.usda") == "my asset.usda");
    CHECK(CanonicalOfRequest(locator, "my asset.usda") == "my asset.usda");
}

TEST_CASE("locator: a non-canonical uploads root still yields canonical names")
{
    TempUploads uploads;
    const UsdFileLocator locator(uploads.base().string() + "/./uploads/");

    CHECK(Canonical(locator, uploads.root() / "a.usda") == "a.usda");
    CHECK(CanonicalOfRequest(locator, "./dir/b.usda") == "dir/b.usda");
}

TEST_CASE("locator: paths outside the uploads root have no canonical name")
{
    TempUploads uploads;
    const UsdFileLocator locator(uploads.root().string());

    CHECK(Canonical(locator, uploads.base() / "outside.usda") == "<OutsideRoot>");
    CHECK(Canonical(locator, uploads.root() / ".." / "outside.usda") == "<OutsideRoot>");
    CHECK(Canonical(locator, uploads.root()) == "<InvalidPath>");
}
