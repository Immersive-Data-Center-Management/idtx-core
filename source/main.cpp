/**
 * @file main.cpp
 * @brief Entrypoint into the IDTX-Core Backend Server
 * 
 */

#include "app/Application.h"
#include "utils/IDTXCoreLogger.h"

// static instance of the specialized logger to be used for the lifetime of the app
static idtx::utils::IDTXCoreLogger g_logger;

int main() {
    // setup the logger
    idtx::utils::Log::set_logger(&g_logger);
    
    // Load .env file if present — only fills in variables not already set in the environment
    EnvironmentUtils::load_dotenv();
    
    try {
        idtx::core::Application application;
        if (!application.Initialize())
        {
            IDTX_LOGF(IDTX_ERROR, "Unable to initialize the IDTX Core Application");
            return 1;
        }
        application.Run();
    } catch (const std::exception& e) {
        IDTX_LOGF(IDTX_ERROR, "Application error: {}", e.what());
        return 1;
    }
    
    return 0;
}