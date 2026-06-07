#pragma once
#include <string>
#include <functional>

namespace net {

struct OAuthConfig {
    std::string web_base;     // e.g. https://www.openstreetmap.org (authorize/token live here)
    std::string client_id;    // from the registered OAuth2 application
    std::string client_secret;// leave empty for public apps; required for confidential ones
    std::string scope{"write_api"};
    int         port{8910};   // loopback redirect port
};

// The fixed redirect URI the user must register for their OAuth2 app.
inline std::string oauth_redirect_uri(int port) {
    return "http://127.0.0.1:" + std::to_string(port) + "/";
}

// Run the OAuth2 Authorization-Code + PKCE flow on a worker thread:
// opens the system browser, listens on the loopback port for the redirect,
// then exchanges the code for an access token. The callback fires on that
// worker thread with (token, message); token is empty on failure.
void oauth2_login_async(const OAuthConfig& cfg,
                        std::function<void(std::string token, std::string message)> cb);

} // namespace net
