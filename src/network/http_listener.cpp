#include "http_listener.h"

#include <nlohmann/json.hpp>

#include "server/commands-common/common.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using asio::awaitable;
using asio::detached;
using asio::redirect_error;
using asio::use_awaitable;

using json = nlohmann::json;
using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;
using Handler = std::function<Response(const Request&)>;

inline Response makeJsonResponse(const Request& req, const json& data, http::status status = http::status::ok) {
  Response res{status, req.version()};
  res.set(http::field::server, "FreeKill-Asio");
  res.set(http::field::content_type, "application/json");
  res.keep_alive(req.keep_alive());
  res.body() = data.dump();
  res.prepare_payload();
  return res;
}

inline Response makeErrorResponse(const Request& req, http::status status, const std::string& message) {
  json error_json = {
      {"error", true},
      {"message", message}};
  return makeJsonResponse(req, error_json, status);
}

class HttpServer {
 public:
  void registerHandler(std::string path, Handler handler) {
    registerHandler(http::verb::unknown, std::move(path), std::move(handler));
  }
  void registerHandler(http::verb method, std::string path, Handler handler) {
    routes_[{method, path}] = std::move(handler);
  }
  void registerDefaultHandler(Handler handler) {
    defaultHandler = std::move(handler);
  }

  Response handleRequest(const Request& req) const {
    // Check headers
    auto getHeader = [&](std::string_view header_name) -> std::optional<std::string> {
      if (auto it = req.find(header_name); it != req.end()) {
        return std::string(it->value());
      }
      return std::nullopt;
    };

    auto checkHeader = [&](std::string_view header_name, std::string_view expected_value) -> bool {
      if (auto header_value = getHeader(header_name); header_value.has_value()) {
        return header_value.value() == expected_value;
      }
      return false;
    };

    if (checkHeader("Host", std::getenv("Host")) == false) {
      return makeErrorResponse(req, http::status::bad_request, "Invalid Host header");
    }
    if (checkHeader("Auth", std::getenv("Auth")) == false) {
      return makeErrorResponse(req, http::status::bad_request, "Invalid X-Requested-With header");
    }

    // Extract path and select handler
    std::string_view target = req.target();
    std::string path = [&] {
      if (auto pos = target.find('?'); pos == std::string_view::npos) {
        return std::string(target);
      } else {
        return std::string(target.substr(0, pos));
      }
    }();

    auto selectRoute = [&](http::verb method, const std::string& path) -> Handler {
      if (auto it = routes_.find({http::verb::unknown, path}); it != routes_.end()) {
        return it->second;
      }
      if (auto it = routes_.find({method, path}); it != routes_.end()) {
        return it->second;
      }
      return defaultHandler;
    };

    auto safeHandleRequest = [&](Handler handler, const Request& req) -> Response {
      try {
        return handler(req);
      } catch (const std::exception& e) {
        return makeErrorResponse(req, http::status::internal_server_error, e.what());
      }
    };

    // Handle the request
    auto handler = selectRoute(req.method(), path);
    return safeHandleRequest(handler, req);
  }

 private:
  // Specific route handlers
  std::map<std::pair<http::verb, std::string>, Handler> routes_;

  // Default response for unknown routes
  Handler defaultHandler = [](const Request& req) -> Response {
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.body() = "<b>Hello, world!</b>";
    res.prepare_payload();
    return res;
  };
};

HttpListener::HttpListener(tcp::endpoint end) : io_ctx{}, endpoint{std::move(end)} {
  spdlog::info("http API is ready to listen on {}", end.port());
}

HttpListener::~HttpListener() {
  io_ctx.stop();
  m_thread.join();
}

void HttpListener::start() {
  m_thread = std::thread(&HttpListener::run, this);
}

void HttpListener::run() {
  pthread_setname_np(pthread_self(), "HttpListener");
  asio::co_spawn(io_ctx, listener(), asio::detached);
  io_ctx.run();
}

static awaitable<void> session(beast::tcp_stream stream);

awaitable<void> HttpListener::listener() {
  auto acceptor = tcp::acceptor{io_ctx, endpoint};

  for (;;) {
    boost::system::error_code ec;
    auto socket = co_await acceptor.async_accept(redirect_error(use_awaitable, ec));
    if (ec) {
      spdlog::warn("Http accept error: {}", ec.message());
      continue;
    }

    auto stream = beast::tcp_stream{std::move(socket)};
    asio::co_spawn(io_ctx, session(std::move(stream)), detached);
  }
}

auto CreateHttpServer() -> HttpServer {
  HttpServer server;
  server.registerHandler("/list_players", [](const Request& req) {
    auto listPlayersResult = commandListPlayers();
    json playersJson = json::array();
    for (const auto& [id, player] : listPlayersResult.players) {
      playersJson.push_back({{"id", player->getId()},
                             {"screenName", player->getScreenName()},
                             {"avatar", player->getAvatar()},
                             {"state", player->getStateString()}});
    }
    json responseJson = {{"players", playersJson}};
    return makeJsonResponse(req, responseJson);
  });
  return server;
}

static awaitable<void> session(beast::tcp_stream stream) {
  beast::flat_buffer buffer;

  HttpServer server = CreateHttpServer();

  for (;;) {
    stream.expires_after(std::chrono::seconds(30));

    http::request<http::string_body> req;
    boost::system::error_code ec;
    co_await http::async_read(stream, buffer, req, redirect_error(use_awaitable, ec));
    if (ec) {
      break;
    }

    // Handle the request based on the path
    Response response = server.handleRequest(req);

    // Send the response to the client
    co_await beast::async_write(stream, http::message_generator{std::move(response)}, use_awaitable);

    // if (!req.keep_alive()) {
    if (true) {
      break;
    }
  }

  stream.socket().shutdown(tcp::socket::shutdown_send);
}
