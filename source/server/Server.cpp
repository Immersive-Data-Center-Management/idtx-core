#include "Server.h"

using namespace idtx::core;

Server::Server(ApplicationContext& applicationContext)
    : m_appContext_(applicationContext)
    , m_routeRegistry_(m_crowApp_, applicationContext)
{
}

void Server::RegisterRoutes()
{
    m_routeRegistry_.RegisterAllRoutes();
}

void Server::Run()
{
    auto server_Port = EnvironmentUtils::get_env(c_serverPortVar_.c_str());
    uint16_t port = atoi(server_Port.value_or("8080").c_str());

    // Bound how long a connection may stay idle before Crow closes it. This is
    // a Slowloris mitigation: a flood of clients that open sockets and then
    // send bytes very slowly cannot pin server threads indefinitely. The value
    // is overridable via SERVER_TIMEOUT_SECONDS.
    std::uint64_t timeoutSeconds = EnvironmentUtils::get_env_u64("SERVER_TIMEOUT_SECONDS", 5);
    
    m_crowApp_
        .port(port)
        .timeout(static_cast<uint8_t>(timeoutSeconds))
        .multithreaded()
        .run();
}
