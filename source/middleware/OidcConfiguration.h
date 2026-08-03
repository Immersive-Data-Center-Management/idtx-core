#pragma once

#include <string>
#include <vector>

namespace idtx
{
namespace middleware
{
    struct OidcConfiguration
    {
        /**
        * @brief Creates the config retrieving the wellknownurl and audiences from the environment variables. 
        *
        * @param wellknownVar   The environment variable for the OIDC wellknow url.
        * @param audiencesVar  The environment variable for the audiences separated by a comma.
        */
        static OidcConfiguration create(const std::string& wellknownVar, const std::string& audiencesVar);
        
        std::string wellKnownUrl;
        std::vector<std::string> audiences;
        bool enabled;
    };
}
}

