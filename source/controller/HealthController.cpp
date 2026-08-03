#include "HealthController.h"

#include <chrono>
#include <crow/json.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

static std::string get_current_timestamp()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);

    std::tm utc;
#if defined(_WIN32)
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif

    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

crow::response HealthController::GetHealth(const crow::request& req)
{
    // Basic status
    json response = {
        {"status", "ok"},
        {"timestamp", get_current_timestamp()}
    };
    
    return crow::response(200, response.dump());
}
