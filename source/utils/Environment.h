#pragma once

#include <fstream>
#include <string>
#include <optional>
#include <charconv>
#include <cstdint>

class EnvironmentUtils {
public:
    /**
     * @brief Loads a .env file and injects its entries into the process environment.
     *        Existing environment variables are NOT overwritten.
     *
     * @param path  Path to the .env file. Defaults to ".env" in the working directory.
     */
    static void load_dotenv(const std::string& path = ".env")
    {
        std::ifstream file(path);
        if (!file.is_open()) return;   // no .env file — that's fine

        std::string line;
        while (std::getline(file, line))
        {
            // Strip trailing \r (Windows line endings)
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // Skip blanks and comments
            if (line.empty() || line.front() == '#')
                continue;

            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;   // no '=' → skip

            std::string key   = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            // Only set if not already defined in the environment
#if defined(_WIN32)
            if (!std::getenv(key.c_str()))
                _putenv_s(key.c_str(), value.c_str());
#else
            ::setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
#endif
        }
    }
    
    /**
     * @brief Gets the environment variable with the given name.
     * 
     * @param name  The stringId of the environment variable.
     * @return std::string The value retrieved from the environment variable.
     */
    inline static  std::optional<std::string> get_env(const char* name)
    {
        if (const char* v = std::getenv(name))
        {
            if (*v) return std::string(v);
        }
        
        return std::nullopt;
    }

    /**
     * @brief Read an unsigned integer environment variable with a fallback.
     *
     * Invalid or absent values fall back to @p fallback.
     */
    inline static std::uint64_t get_env_u64(const char* name, std::uint64_t fallback)
    {
        if (auto v = EnvironmentUtils::get_env(name))
        {
            std::uint64_t out{};
            const auto* first = v->data();
            const auto* last  = v->data() + v->size();
            if (auto [p, ec] = std::from_chars(first, last, out);
                ec == std::errc() && p == last)
            {
                return out;
            }
        }
        return fallback;
    }
};