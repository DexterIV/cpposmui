#include "oauth.hpp"
#include "http.hpp"
#include "sha256.hpp"

#include <boost/asio.hpp>          // must precede <windows.h> (winsock2)
#include <nlohmann/json.hpp>

#include <windows.h>
#include <shellapi.h>

#include <cctype>
#include <chrono>
#include <optional>
#include <random>
#include <string>
#include <thread>

namespace net {

namespace {

std::string url_encode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back((char)c);
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 0xf]); }
    }
    return out;
}

std::string random_verifier() {
    static const char* set =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 63);
    std::string v;
    for (int i = 0; i < 64; ++i) v.push_back(set[dist(gen)]);
    return v;
}

// Block (with timeout) on the loopback port until the browser delivers ?code=…
std::optional<std::string> wait_for_code(int port, std::string& err) {
    using boost::asio::ip::tcp;
    try {
        boost::asio::io_context io;
        tcp::acceptor acc(io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"),
                                            (unsigned short)port));
        std::optional<tcp::socket> sock;
        acc.async_accept([&](const boost::system::error_code& ec, tcp::socket s) {
            if (!ec) sock.emplace(std::move(s));
        });
        io.run_for(std::chrono::seconds(180));
        if (!sock) { err = "Login timed out (no redirect received)."; return std::nullopt; }

        boost::asio::streambuf buf;
        boost::system::error_code ec;
        boost::asio::read_until(*sock, buf, "\r\n", ec);
        std::istream is(&buf);
        std::string line;
        std::getline(is, line); // "GET /?code=XXXX&... HTTP/1.1"

        std::string html =
            "<html><body style='font-family:sans-serif'>"
            "<h3>cpposmui login complete</h3>"
            "<p>You can close this tab and return to the app.</p></body></html>";
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                           std::to_string(html.size()) + "\r\nConnection: close\r\n\r\n" + html;
        boost::asio::write(*sock, boost::asio::buffer(resp), ec);
        sock->close(ec);

        auto cpos = line.find("code=");
        if (cpos == std::string::npos) {
            err = "Authorization was denied or returned no code.";
            return std::nullopt;
        }
        cpos += 5;
        auto end = line.find_first_of("& ", cpos);
        return line.substr(cpos, end == std::string::npos ? std::string::npos : end - cpos);
    } catch (const std::exception& e) {
        err = std::string("Loopback listener failed: ") + e.what();
        return std::nullopt;
    }
}

} // anonymous namespace

void oauth2_login_async(const OAuthConfig& cfg,
                        std::function<void(std::string, std::string)> cb) {
    std::thread([cfg, cb = std::move(cb)]() mutable {
        if (cfg.client_id.empty()) { cb("", "Set a Client ID first."); return; }

        std::string verifier  = random_verifier();
        auto digest           = detail::sha256(verifier);
        std::string challenge = detail::base64url(digest.data(), digest.size());
        std::string redirect  = oauth_redirect_uri(cfg.port);

        std::string auth_url =
            cfg.web_base + "/oauth2/authorize?response_type=code"
            "&client_id="    + url_encode(cfg.client_id) +
            "&redirect_uri=" + url_encode(redirect) +
            "&scope="        + url_encode(cfg.scope) +
            "&code_challenge=" + challenge +
            "&code_challenge_method=S256";

        // Open the system browser to the authorization page.
        ShellExecuteA(nullptr, "open", auth_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

        std::string err;
        auto code = wait_for_code(cfg.port, err);
        if (!code) { cb("", err); return; }

        // Exchange the authorization code for an access token.
        // Include client_secret if the app was registered as Confidential on OSM.
        std::string body =
            "grant_type=authorization_code"
            "&code="          + url_encode(*code) +
            "&redirect_uri="  + url_encode(redirect) +
            "&client_id="     + url_encode(cfg.client_id) +
            "&code_verifier=" + url_encode(verifier);
        if (!cfg.client_secret.empty())
            body += "&client_secret=" + url_encode(cfg.client_secret);

        auto resp = net::request("POST", cfg.web_base + "/oauth2/token", body,
                                 {{"Content-Type", "application/x-www-form-urlencoded"}});
        if (!resp) { cb("", "Network error exchanging token."); return; }
        if (resp->status_code != 200) {
            // Parse OSM's error JSON if present so the user sees a useful message.
            std::string detail = resp->body;
            try {
                auto j = nlohmann::json::parse(resp->body);
                std::string e = j.value("error", "");
                std::string d = j.value("error_description", "");
                if (!e.empty()) detail = e + (d.empty() ? "" : ": " + d);
            } catch (...) {}
            cb("", std::format("Token exchange failed (HTTP {}): {}\n"
                               "Tip: if your OSM app is Confidential, fill in the Client Secret.",
                               resp->status_code, detail));
            return;
        }
        try {
            auto j = nlohmann::json::parse(resp->body);
            std::string token = j.value("access_token", std::string{});
            if (token.empty()) { cb("", "No access_token in response: " + resp->body); return; }
            cb(token, "Logged in to OSM successfully.");
        } catch (...) {
            cb("", "Could not parse token response: " + resp->body);
        }
    }).detach();
}

} // namespace net
