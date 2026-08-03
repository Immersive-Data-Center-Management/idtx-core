#include "OidcConfiguration.h"

#include <stdexcept>

#include "utils/Environment.h"

namespace 
{
    //Checks if a character is a white space
    bool is_space(unsigned char c)
    {
        return std::isspace(c) != 0; //ASCII whitespace
    }

    //Removes white spaces from the beginning and end of a string
    std::string_view trim_space(std::string_view env)
    {
        size_t start_index = 0, end_index = env.size();
        while (start_index < end_index && is_space(static_cast<unsigned char>(env[start_index]))) ++start_index;
        while (end_index > start_index && is_space(static_cast<unsigned char>(env[end_index - 1]))) --end_index;
        return env.substr(start_index, end_index - start_index);
    }


    //Splits a single comma separated string into a vector of strings
    std::vector<std::string> split_env(std::string_view env)
    {
        std::vector<std::string> split_result;
        size_t start_index = 0;
        while (start_index < env.size())
        {
            size_t comma_index = env.find(',', start_index);
            std::string_view token = env.substr(start_index, (comma_index == std::string_view::npos ? env.size() : comma_index) - start_index);
            std::string_view token_trimmed = trim_space(token);
            if (!token_trimmed.empty()) split_result.emplace_back(token_trimmed);
            start_index = (comma_index == std::string_view::npos) ? env.size() : comma_index + 1;
        }
        return split_result;
    }
}

idtx::middleware::OidcConfiguration idtx::middleware::OidcConfiguration::create(const std::string& wellknownVar,
                                                                                const std::string& audiencesVar)
{
    OidcConfiguration oidcData;
    oidcData.enabled = true;

    if (oidcData.enabled)
    {
        auto retrieved_wellKnownUrl = EnvironmentUtils::get_env(wellknownVar.c_str());
        if (!retrieved_wellKnownUrl)
        {
            throw std::runtime_error("Missing required environment variable: " + std::string(wellknownVar));
        }
        oidcData.wellKnownUrl = retrieved_wellKnownUrl.value();

        auto retrieved_audiencesVar = EnvironmentUtils::get_env(audiencesVar.c_str());
        if (!retrieved_audiencesVar)
        {
            throw std::runtime_error("Missing required environment variable: " + std::string(audiencesVar));
        }
        oidcData.audiences = split_env(retrieved_audiencesVar.value());

        if (oidcData.audiences.empty())
        {
            throw std::runtime_error(audiencesVar + " parsed to an empty list.");
        }
    }

    return oidcData;
}
