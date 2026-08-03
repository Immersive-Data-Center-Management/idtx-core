// tests/main.cpp — doctest entry point for the idtx-core test binary.
//
// Doctest is header-only and supplies its own main() via
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN. We keep this .cpp deliberately tiny so
// that the framework's implementation lives in a single translation unit.
//
// Before running any tests we also install the process-wide logger so that
// server code paths (which unconditionally IDTX_LOG(...)) don't crash when
// their SPI is invoked.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "thirdparty/doctest/doctest.h"

#include "utils/IDTXCoreLogger.h"

namespace {

struct TestBootstrap
{
    idtx::utils::IDTXCoreLogger logger;
    TestBootstrap()
    {
        idtx::utils::Log::set_logger(&logger);
    }
};

// One process-wide bootstrap; constructed before doctest::Context runs the
// first TEST_CASE.
static TestBootstrap g_bootstrap;

} // namespace