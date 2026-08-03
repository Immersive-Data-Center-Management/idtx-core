#include "JwtAuthHandler.h"

#include <shared_mutex>

#include <nlohmann/json.hpp>

#include "idtx/utils/Logger.h"
#include "utils/HttpClient.h"

using namespace idtx::middleware;
using json = nlohmann::json;

struct KeyEntry {
    //RSA public key PEM derived from JWK (n,e)
    std::string pem;
    std::string kid;
    std::string kty;
    std::string use;
    std::string alg;
};

class idtx::middleware::JwksCache {
    IDTX_LOG_CATEGORY("JwksCache")
    
public:
    /**
     * @brief Parses a JWKS (json Web Key Set) from the given json string and updates the
     * internal cache with all valid RSA signature keys. The method extracts keys with
     * "kty" = "RSA" and "use" = "sig".
     * It requires a non‑empty "kid" and converts the RSA components ("n" and "e") into a PEM encoded public key.
     * Invalid or unsupported keys are silently skipped.
     * 
     * @param jwks_json A UTF-8 string containing a JWKS document. Expected shape:
     * @code{.json}
     * {
     *   "keys": [
     *     {
     *       "kty": "RSA",
     *       "use": "sig",
     *       "kid": "example-key-id",
     *       "alg": "RS256",
     *       "n": "base64url-modulus",
     *       "e": "AQAB"
     *     }
     *   ]
     * }
     * @endcode
     */
    void loadJwksJson(const std::string& jwks_json)
    {
        std::unique_lock<std::shared_mutex> lk(mutex_);

        std::unordered_map<std::string, KeyEntry> keyEntries;
        auto publicKeys = json::parse(jwks_json);
        if (!publicKeys.contains("keys") || !publicKeys["keys"].is_array())
        {
            CROW_LOG_ERROR << "Invalid jwks: missing 'keys' array";
        }

        for (const auto& publicKey : publicKeys["keys"])
        {
            const auto kty = publicKey.value("kty", "");
            const auto use = publicKey.value("use", "sig");
            const auto kid = publicKey.value("kid", "");
            const auto alg = publicKey.value("alg", "");
            if (kid.empty() || use != "sig") continue;

            if (kty == "RSA")
            {
                if (!publicKey.contains("n") || !publicKey.contains("e"))
                {
                    continue;
                }

                const auto n = publicKey.at("n").get<std::string>();
                const auto e = publicKey.at("e").get<std::string>();
                auto pem = jwt::helper::create_public_key_from_rsa_components(n, e);
                auto keyEntry = KeyEntry{std::move(pem), kid, kty, use, alg};

                keyEntries.emplace(kid, keyEntry);

                IDTX_LOG(IDTX_INFO, "Loaded jwk: {}", kid);
            }
        }

        kid_to_keyEntry = std::move(keyEntries);
    }

    /**
     * @brief Retrieves a cached key entry by its key ID (kid).
     *
     * Searches for the given kid in the key cache. 
     * If a matching entry exists, the method returns the associated KeyEntry otherwise it returns std::nullopt.
     * 
     * @param kid The key ID used to look up a cached KeyEntry.
     * @return std::optional<KeyEntry> The matching key entry if found, or
     *         std::nullopt if no entry with the given kid exists.
     */
    std::optional<KeyEntry> getKeyEntry(const std::string& kid) const
    {
        std::shared_lock<std::shared_mutex>  lk(mutex_);
        auto keyEntry = kid_to_keyEntry.find(kid);
        if (keyEntry == kid_to_keyEntry.end())
        {
            return std::nullopt;
        }
    
        IDTX_LOG(IDTX_INFO, "Retrieving kid {}", kid);
        return keyEntry->second;
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, KeyEntry> kid_to_keyEntry;
};

void JwtAuthHandler::configure(const OidcConfiguration& oidcConfig, std::unordered_set<std::string> allowedAlgs,
    std::chrono::seconds leeway)
{
    well_known_url = oidcConfig.wellKnownUrl;
    audiences_ = oidcConfig.audiences;
    allowed_algs = std::move(allowedAlgs);
    leeway_ = leeway;
    is_enabled = oidcConfig.enabled;

    if (!is_enabled)
    {
        return;
    }
    
    cache_ = std::make_unique<JwksCache>();
        
    try
    {
        //Retrieve issuer and jwks_uri
        auto response = http::HttpClient::get(well_known_url);
        if (!response.ok)
        {
            IDTX_LOG(IDTX_ERROR, "Failed to GET wellknown: {}", response.err);
            return;
        }
        auto j = json::parse(response.body);
        issuer_ = j.at("issuer").get<std::string>();
        jwks_uri = j.at("jwks_uri").get<std::string>();

        IDTX_LOG(IDTX_INFO, "JWTAuth: issuer: {}", issuer_);
        IDTX_LOG(IDTX_INFO, "JWTAuth: jwks: {}", jwks_uri);

        refreshJwks();
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "jwt FAILED: {}", e.what());
    }
}

void JwtAuthHandler::before_handle(crow::request& req, crow::response& res, context& ctx)
{
    // Let CORS preflight pass
    if (req.method == crow::HTTPMethod::Options || !is_enabled) return;
    
    // let the health and login endpoints pass without authentication
    if (req.url == "/api/v1/health" ||
        req.url == "/api/v1/auth/login") return;
    
    auto result = authenticate(req);
    if (!result.success) {
        ctx.authenticated = false;
        ctx.message = result.message;
        ctx.sub = result.sub;
        res.code = 401;
        res.set_header("WWW-Authenticate", "Bearer");
        res.end(result.message);
        return;
    }
    ctx.authenticated = true;
    ctx.message = result.message;
    ctx.sub = result.sub;
}

void JwtAuthHandler::after_handle(crow::request& req, crow::response& res, context& ctx)
{
}

JwtAuthHandler::JwtAuthHandler() = default;
JwtAuthHandler::~JwtAuthHandler() = default;

std::optional<std::string> JwtAuthHandler::getBearerToken(const crow::request& req) const
{
    const std::string& auth = req.get_header_value("Authorization");

    if (!auth.starts_with("Bearer "))
    {
        return std::nullopt;
    }

    return auth.substr(7);
}

JwtAuthHandler::AuthResult JwtAuthHandler::authenticate(const crow::request& req)
{
    std::optional<std::string> tokenOpt = getBearerToken(req);

    if (!tokenOpt.has_value())
    {
        return {false, "Unauthorized: No Bearer token"};
    }

    return authenticate(tokenOpt.value());
}

JwtAuthHandler::AuthResult JwtAuthHandler::authenticate(const std::string& token)
{
    std::string sub;

    if (token.empty())
    {
        return {false, "Missing token", sub};
    }
    if (issuer_.empty() || jwks_uri.empty())
    {
        return {false, "JwtAuth not initialized", sub};
    }

    std::optional<decltype(jwt::decode(token))> decoded;
    try
    {
        decoded.emplace(jwt::decode(token));
    }
    catch (const std::exception& e)
    {
        return {false, std::string("Invalid token format: ") + e.what(), sub};
    }

    const auto& d = *decoded;

    //Header: kid & alg (alg must be allowed)
    const std::string kid = d.has_header_claim("kid")
                                ? d.get_header_claim("kid").as_string()
                                : "";
    const std::string alg = d.has_header_claim("alg")
                                ? d.get_header_claim("alg").as_string()
                                : "";
    sub = d.get_payload_claim("sub").as_string();

    if (kid.empty())
    {
        return {false, "Token missing 'kid'", sub};
    }
    if (alg.empty())
    {
        return {false, "Token missing 'alg'", sub};
    }
    if (!allowed_algs.empty() && !allowed_algs.count(alg))
    {
        return {false, "Disallowed algorithm: " + alg, sub};
    }

    //Attempt verification using cached key (retry once on unknown kid -> refresh jwks)
    std::string err;
    if (!tryVerify(d, alg, kid, err))
    {
        //Unknown kid or verification error attempt one refresh, then retry only on unknown kid case
        bool was_unknown_kid = (err == "Unknown kid");
        if (was_unknown_kid && refreshJwks() && tryVerify(d, alg, kid, err))
        {
            return {true, "OK", sub};
        }
        return {false, err, sub};
    }
    return {true, "OK", sub};
}

bool JwtAuthHandler::refreshJwks()
{
    if (jwks_uri.empty())
    {
        last_error_ = "jwks_uri not set";
        return false;
    }

    auto response = http::HttpClient::get(jwks_uri);
    if (!response.ok)
    {
        last_error_ = "Failed to GET jwks: HTTP " + std::to_string(response.code) + " " + response.err;
        return false;
    }

    try
    {
        cache_->loadJwksJson(response.body);
        return true;
    }
    catch (const std::exception& e)
    {
        last_error_ = std::string("Failed to load JWKS: ") + e.what();
        return false;
    }
}

template <typename Traits>
bool JwtAuthHandler::tryVerify(const jwt::decoded_jwt<Traits>& decoded, const std::string& alg,
    const std::string& kid, std::string& out_error)
{
    //Retrievs cached KeyEntry
    auto keyOpt = cache_ ? cache_->getKeyEntry(kid) : std::nullopt;
    if (!keyOpt)
    {
        out_error = "Unknown key id kid";
        return false;
    }
    const auto& key = *keyOpt;

    //Enforce signing usage and RSA type 
    if (key.use != "sig")
    {
        out_error = "jwk not for signing";
        return false;
    }
    if (key.kty != "RSA")
    {
        out_error = "Unsupported key type";
        return false;
    }

    //Creating verifier
    auto verifier = jwt::verify();
    verifier.leeway(static_cast<size_t>(leeway_.count()));
    if (!issuer_.empty())
    {
        verifier.with_issuer(issuer_);
    }
    for (const auto& aud : audiences_)
    {
        verifier.with_audience(aud);
    }

    //Allow the required algorithm (selected from jwt header and cross check with jwk alg if present)
    if (!key.alg.empty() && key.alg != alg)
    {
        out_error = "jwk alg mismatch";
        return false;
    }

    if (alg == "RS256") verifier.allow_algorithm(jwt::algorithm::rs256(key.pem, "", "", ""));
    else if (alg == "RS384") verifier.allow_algorithm(jwt::algorithm::rs384(key.pem, "", "", ""));
    else if (alg == "RS512") verifier.allow_algorithm(jwt::algorithm::rs512(key.pem, "", "", ""));
    else if (alg == "PS256") verifier.allow_algorithm(jwt::algorithm::ps256(key.pem, "", "", ""));
    else if (alg == "PS384") verifier.allow_algorithm(jwt::algorithm::ps384(key.pem, "", "", ""));
    else if (alg == "PS512") verifier.allow_algorithm(jwt::algorithm::ps512(key.pem, "", "", ""));
    else
    {
        out_error = "Unsupported algorithm";
        return false;
    }

    try
    {
        //Verifying :
        //signature, iss, aud and exp (with leeway)
        verifier.verify(decoded);
        return true;
    }
    catch (const std::exception& e)
    {
        out_error = std::string("Token verification failed: ") + e.what();
        return false;
    }
}
