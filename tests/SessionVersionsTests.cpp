// tests/SessionVersionsTests.cpp
//
// Unit tests of the stale-update rules in SessionVersions. No server or stage
// is involved: the tests record changes and sent versions directly.

#include "thirdparty/doctest/doctest.h"

#include <string>

#include "session/SessionVersions.h"

using idtx::session::SessionVersions;
using idtx::session::UpdateBaseStatus;

namespace
{

const std::string kPrim  = "/Root/Cube";
const std::string kOther = "/Root/Other";

constexpr idtx::session::ConnectionId kA = 1;
constexpr idtx::session::ConnectionId kB = 2;

} // namespace

TEST_CASE("versions: base 0 is current in a session without changes")
{
    SessionVersions versions;
    CHECK(versions.CheckUpdate(kA, kPrim, 0) == UpdateBaseStatus::Current);
}

TEST_CASE("versions: a base above everything sent to the client is invalid")
{
    SessionVersions versions;
    CHECK(versions.CheckUpdate(kA, kPrim, 1) == UpdateBaseStatus::InvalidBase);

    versions.RecordSent(kA, 3);
    versions.RecordSent(kB, 5);
    CHECK(versions.CheckUpdate(kA, kPrim, 3) == UpdateBaseStatus::Current);
    CHECK(versions.CheckUpdate(kA, kPrim, 4) == UpdateBaseStatus::InvalidBase);
    CHECK(versions.CheckUpdate(kB, kPrim, 5) == UpdateBaseStatus::Current);

    // The highest version counts, not the last one sent.
    versions.RecordSent(kA, 2);
    CHECK(versions.CheckUpdate(kA, kPrim, 3) == UpdateBaseStatus::Current);
}

TEST_CASE("versions: a change by someone else makes older bases stale")
{
    SessionVersions versions;
    versions.RecordChange(kPrim, 1, kB);
    versions.RecordSent(kA, 1);

    CHECK(versions.CheckUpdate(kA, kPrim, 0) == UpdateBaseStatus::Stale);
    CHECK(versions.CheckUpdate(kA, kPrim, 1) == UpdateBaseStatus::Current);

    // Other prims are not affected.
    CHECK(versions.CheckUpdate(kA, kOther, 0) == UpdateBaseStatus::Current);
}

TEST_CASE("versions: the client's own changes never make its update stale")
{
    SessionVersions versions;
    versions.RecordChange(kPrim, 1, kA);
    versions.RecordChange(kPrim, 2, kA);
    versions.RecordChange(kPrim, 3, kA);
    versions.RecordSent(kA, 3);

    CHECK(versions.CheckUpdate(kA, kPrim, 0) == UpdateBaseStatus::Current);
}

TEST_CASE("versions: the latest foreign change before the client's run counts")
{
    SessionVersions versions;
    versions.RecordChange(kPrim, 1, kA);
    versions.RecordChange(kPrim, 2, kB);
    versions.RecordChange(kPrim, 3, kA);
    versions.RecordChange(kPrim, 4, kA);
    versions.RecordSent(kA, 4);
    versions.RecordSent(kB, 4);

    // A must have seen B's change 2; its own run 3..4 does not matter.
    CHECK(versions.CheckUpdate(kA, kPrim, 1) == UpdateBaseStatus::Stale);
    CHECK(versions.CheckUpdate(kA, kPrim, 2) == UpdateBaseStatus::Current);

    // For B the latest foreign change is A's 4.
    CHECK(versions.CheckUpdate(kB, kPrim, 3) == UpdateBaseStatus::Stale);
    CHECK(versions.CheckUpdate(kB, kPrim, 4) == UpdateBaseStatus::Current);
}

TEST_CASE("versions: a server-side change is foreign to every client")
{
    SessionVersions versions;
    versions.RecordChange(kPrim, 1, kA);
    versions.RecordChange(kPrim, 2, 0);
    versions.RecordSent(kA, 2);

    CHECK(versions.CheckUpdate(kA, kPrim, 1) == UpdateBaseStatus::Stale);
    CHECK(versions.CheckUpdate(kA, kPrim, 2) == UpdateBaseStatus::Current);

    // A's next run starts after the reload.
    versions.RecordChange(kPrim, 3, kA);
    versions.RecordSent(kA, 3);
    CHECK(versions.CheckUpdate(kA, kPrim, 1) == UpdateBaseStatus::Stale);
    CHECK(versions.CheckUpdate(kA, kPrim, 2) == UpdateBaseStatus::Current);
}

TEST_CASE("versions: a correction makes older bases of that client and prim stale")
{
    SessionVersions versions;
    versions.RecordChange(kPrim, 1, kA);
    versions.RecordCorrection(kA, kPrim, 2);

    // The correction counts as sent.
    CHECK(versions.CheckUpdate(kA, kPrim, 2) == UpdateBaseStatus::Current);
    CHECK(versions.CheckUpdate(kA, kPrim, 1) == UpdateBaseStatus::Stale);
    CHECK(versions.CheckUpdate(kA, kOther, 1) == UpdateBaseStatus::Current);

    // Other clients are not affected by A's correction.
    versions.RecordSent(kB, 1);
    CHECK(versions.CheckUpdate(kB, kPrim, 1) == UpdateBaseStatus::Current);
}
