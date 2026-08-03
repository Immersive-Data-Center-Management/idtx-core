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

    m_crowApp_
        .port(port)
        .multithreaded()
        .run();
}
