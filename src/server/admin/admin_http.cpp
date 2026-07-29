// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/admin/admin_http.h"
#include "server/server.h"

#include <nlohmann/json.hpp>

constexpr char API_HELP_HTML[] = {
  #embed "api_help.html"
  , 0
};

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using asio::awaitable;
using asio::detached;
using asio::use_awaitable;
using asio::redirect_error;

using json = nlohmann::json;

static constexpr std::size_t kBodyLimit = 64 * 1024;  // 请求体上限 64KiB

AdminHttpServer::AdminHttpServer(tcp::endpoint endpoint, AdminHttpConfig config) :
  io_ctx_{}, endpoint_{std::move(endpoint)}, config_{std::move(config)}
{
  registerRoutes();
}

AdminHttpServer::~AdminHttpServer() {
  io_ctx_.stop();
  if (thread_.joinable()) thread_.join();
}

void AdminHttpServer::start() {
  thread_ = std::thread(&AdminHttpServer::run, this);
}

void AdminHttpServer::run() {
  pthread_setname_np(pthread_self(), "AdminHttp");
  asio::co_spawn(io_ctx_, listener(), asio::detached);
  io_ctx_.run();
}

void AdminHttpServer::registerRoutes() {
  // 只读
  routes_["GET /api"] = [this](const Request &req) { return handleApiHelp(req); };
  routes_["GET /api/players"] = [this](const Request &req) { return handleLsPlayers(req); };
  routes_["GET /api/rooms"] = [this](const Request &req) { return handleLsRooms(req); };
  routes_["GET /api/server/stat"] = [this](const Request &req) { return handleServerStat(req); };

  // 干预
  routes_["POST /api/players/kick"] = [this](const Request &req) { return handleKick(req); };
  routes_["POST /api/broadcast"] = [this](const Request &req) { return handleBroadcast(req); };
  routes_["POST /api/lobby/check"] = [this](const Request &req) { return handleCheckLobby(req); };

  // 账号
  routes_["POST /api/accounts/ban"] = [this](const Request &req) { return handleBan(req); };
  routes_["POST /api/accounts/unban"] = [this](const Request &req) { return handleUnban(req); };
  routes_["POST /api/accounts/ban-ip"] = [this](const Request &req) { return handleBanIp(req); };
  routes_["POST /api/accounts/unban-ip"] = [this](const Request &req) { return handleUnbanIp(req); };
  routes_["POST /api/accounts/ban-uuid"] = [this](const Request &req) { return handleBanUuid(req); };
  routes_["POST /api/accounts/unban-uuid"] = [this](const Request &req) { return handleUnbanUuid(req); };
  routes_["POST /api/accounts/temp-ban"] = [this](const Request &req) { return handleTempBan(req); };
  routes_["POST /api/accounts/temp-mute"] = [this](const Request &req) { return handleTempMute(req); };
  routes_["POST /api/accounts/unmute"] = [this](const Request &req) { return handleUnmute(req); };
  routes_["POST /api/accounts/whitelist"] = [this](const Request &req) { return handleWhitelist(req); };
  routes_["POST /api/accounts/reset-password"] = [this](const Request &req) { return handleResetPassword(req); };

  // 配置
  routes_["POST /api/server/reload-config"] = [this](const Request &req) { return handleReloadConfig(req); };
}

std::string AdminHttpServer::requestPath(const Request &req) {
  auto target = std::string(req.target());
  auto queryPos = target.find('?');
  return queryPos == std::string::npos ? target : target.substr(0, queryPos);
}

bool AdminHttpServer::parseRoomId(std::string_view text, int &out) {
  if (text.empty()) return false;
  // 拒绝符号/空白等，要求整段都是十进制数字
  for (unsigned char ch : text) {
    if (!std::isdigit(ch)) return false;
  }
  try {
    size_t idx = 0;
    auto value = std::stoll(std::string(text), &idx, 10);
    if (idx != text.size() || value < 0 || value > std::numeric_limits<int>::max()) {
      return false;
    }
    out = static_cast<int>(value);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool AdminHttpServer::parseRoomAction(std::string_view path, int &roomId,
                                      std::string_view &action) {
  static constexpr std::string_view kPrefix = "/api/rooms/";
  if (!path.starts_with(kPrefix)) return false;
  auto rest = path.substr(kPrefix.size());
  auto slash = rest.find('/');
  if (slash == std::string_view::npos || slash == 0) return false;
  if (!parseRoomId(rest.substr(0, slash), roomId)) return false;
  action = rest.substr(slash + 1);
  return action == "broadcast" || action == "kill";
}

bool AdminHttpServer::checkAuth(const Request &req) const {
  auto it = req.find(http::field::authorization);
  if (it == req.end()) return false;
  auto value = it->value();
  static constexpr std::string_view kPrefix = "Bearer ";
  if (!value.starts_with(kPrefix)) return false;
  return std::string_view(value).substr(kPrefix.size()) == config_.token;
}

void AdminHttpServer::applyCors(const Request &req,
                                http::response<http::string_body> &res) const {
  if (config_.corsOrigins.empty()) return;
  auto origin_it = req.find(http::field::origin);
  if (origin_it == req.end()) return;
  auto origin = std::string(origin_it->value());
  for (auto &allowed : config_.corsOrigins) {
    if (allowed == origin || allowed == "*") {
      res.set(http::field::access_control_allow_origin, origin);
      res.set(http::field::access_control_allow_headers, "Authorization, Content-Type");
      res.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
      return;
    }
  }
}

AdminHttpServer::HttpResponse AdminHttpServer::runOnMain(std::function<AdminResult()> fn) const {
  auto &ctx = Server::instance().context();
  auto fut = asio::dispatch(ctx, asio::use_future(std::move(fn)));
  return fromAdminResult(fut.get());
}

AdminHttpServer::HttpResponse AdminHttpServer::fromAdminResult(const AdminResult &result) {
  return {
    static_cast<http::status>(result.httpStatus()),
    "application/json",
    result.toHttpResponse().dump(),
  };
}

AdminHttpServer::HttpResponse AdminHttpServer::jsonError(http::status status,
                                                         std::string_view code,
                                                         std::string_view message) {
  json body = {
    {"success", false},
    {"error", {
      {"code", code},
      {"message", message},
    }},
  };
  return {status, "application/json", body.dump()};
}

bool AdminHttpServer::parseJsonBody(const Request &req, json &out, HttpResponse &err) {
  if (req.body().empty()) {
    out = json::object();
    return true;
  }
  try {
    out = json::parse(req.body());
    if (!out.is_object()) {
      err = jsonError(http::status::bad_request, "BAD_REQUEST", "JSON body must be an object");
      return false;
    }
    return true;
  } catch (const std::exception &) {
    err = jsonError(http::status::bad_request, "BAD_REQUEST", "Invalid JSON body");
    return false;
  }
}

bool AdminHttpServer::requireString(const json &body, const char *key,
                                    std::string &out, HttpResponse &err) {
  if (!body.contains(key) || !body[key].is_string()) {
    err = jsonError(http::status::bad_request, "BAD_REQUEST",
                    fmt::format("Missing or invalid string field '{}'", key));
    return false;
  }
  out = body[key].get<std::string>();
  return true;
}

bool AdminHttpServer::requireStringArray(const json &body, const char *key,
                                         std::vector<std::string> &out, HttpResponse &err) {
  if (!body.contains(key) || !body[key].is_array()) {
    err = jsonError(http::status::bad_request, "BAD_REQUEST",
                    fmt::format("Missing or invalid array field '{}'", key));
    return false;
  }
  out.clear();
  for (auto &v : body[key]) {
    if (!v.is_string()) {
      err = jsonError(http::status::bad_request, "BAD_REQUEST",
                      fmt::format("Field '{}' must be an array of strings", key));
      return false;
    }
    out.push_back(v.get<std::string>());
  }
  return true;
}

AdminHttpServer::HttpResponse AdminHttpServer::handleApiHelp(const Request &) {
  return {http::status::ok, "text/html; charset=utf-8", {API_HELP_HTML, sizeof(API_HELP_HTML) - 1}};
}

AdminHttpServer::HttpResponse AdminHttpServer::handleLsPlayers(const Request &) {
  return runOnMain([] { return AdminService::lsPlayers(); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleLsRooms(const Request &) {
  bool expose = config_.exposeRoomPassword;
  return runOnMain([expose] { return AdminService::lsRoomInfo(-1, expose); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleLsRoomById(const Request &, int roomId) {
  bool expose = config_.exposeRoomPassword;
  return runOnMain([roomId, expose] { return AdminService::lsRoomInfo(roomId, expose); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleServerStat(const Request &) {
  return runOnMain([] { return AdminService::serverStat(); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleKick(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::string name;
  if (!requireString(body, "name", name, err)) return err;
  return runOnMain([name = std::move(name)] { return AdminService::kickPlayer(name); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleBroadcast(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::string message;
  if (!requireString(body, "message", message, err)) return err;
  return runOnMain([message = std::move(message)] { return AdminService::broadcast(message); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleRoomBroadcast(const Request &req, int roomId) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::string message;
  if (!requireString(body, "message", message, err)) return err;
  return runOnMain([roomId, message = std::move(message)] {
    return AdminService::broadcastRoom(roomId, message);
  });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleKillRoom(const Request &, int roomId) {
  return runOnMain([roomId] { return AdminService::killRoom(roomId); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleCheckLobby(const Request &) {
  return runOnMain([] { return AdminService::checkLobby(); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleBan(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::vector<std::string> names;
  if (!requireStringArray(body, "names", names, err)) return err;
  return runOnMain([names = std::move(names)] { return AdminService::banAccounts(names); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleUnban(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::vector<std::string> names;
  if (!requireStringArray(body, "names", names, err)) return err;
  return runOnMain([names = std::move(names)] { return AdminService::unbanAccounts(names); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleBanIp(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::vector<std::string> names;
  if (!requireStringArray(body, "names", names, err)) return err;
  return runOnMain([names = std::move(names)] { return AdminService::banIpsByNames(names); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleUnbanIp(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::vector<std::string> names;
  if (!requireStringArray(body, "names", names, err)) return err;
  return runOnMain([names = std::move(names)] { return AdminService::unbanIps(names); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleBanUuid(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::vector<std::string> names;
  if (!requireStringArray(body, "names", names, err)) return err;
  return runOnMain([names = std::move(names)] { return AdminService::banUuidsByNames(names); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleUnbanUuid(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::vector<std::string> names;
  if (!requireStringArray(body, "names", names, err)) return err;
  return runOnMain([names = std::move(names)] { return AdminService::unbanUuids(names); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleTempBan(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::string name, duration;
  if (!requireString(body, "name", name, err)) return err;
  if (!requireString(body, "duration", duration, err)) return err;
  return runOnMain([name = std::move(name), duration = std::move(duration)] {
    return AdminService::tempBan(name, duration);
  });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleTempMute(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::string name, duration;
  if (!requireString(body, "name", name, err)) return err;
  if (!requireString(body, "duration", duration, err)) return err;
  return runOnMain([name = std::move(name), duration = std::move(duration)] {
    return AdminService::tempMute(name, duration);
  });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleUnmute(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::vector<std::string> names;
  if (!requireStringArray(body, "names", names, err)) return err;
  return runOnMain([names = std::move(names)] { return AdminService::unmute(names); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleWhitelist(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::string action;
  std::vector<std::string> names;
  if (!requireString(body, "action", action, err)) return err;
  if (!requireStringArray(body, "names", names, err)) return err;
  return runOnMain([action = std::move(action), names = std::move(names)] {
    return AdminService::whitelist(action, names);
  });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleResetPassword(const Request &req) {
  json body; HttpResponse err;
  if (!parseJsonBody(req, body, err)) return err;
  std::vector<std::string> names;
  if (!requireStringArray(body, "names", names, err)) return err;
  return runOnMain([names = std::move(names)] { return AdminService::resetPassword(names); });
}

AdminHttpServer::HttpResponse AdminHttpServer::handleReloadConfig(const Request &) {
  return runOnMain([] { return AdminService::reloadConfig(); });
}

AdminHttpServer::HttpResponse AdminHttpServer::dispatch(const Request &req) {
  auto path = requestPath(req);
  auto key = std::string(req.method_string()) + " " + path;

  auto it = routes_.find(key);
  if (it != routes_.end()) {
    return it->second(req);
  }

  // GET /api/rooms/{id}
  if (req.method() == http::verb::get && path.starts_with("/api/rooms/")) {
    auto idStr = path.substr(std::string_view("/api/rooms/").size());
    int roomId = 0;
    if (idStr.find('/') != std::string::npos || !parseRoomId(idStr, roomId)) {
      return jsonError(http::status::bad_request, "BAD_REQUEST", "Invalid room id");
    }
    return handleLsRoomById(req, roomId);
  }

  // POST /api/rooms/{id}/broadcast|kill
  if (req.method() == http::verb::post) {
    int roomId = 0;
    std::string_view action;
    if (parseRoomAction(path, roomId, action)) {
      if (action == "broadcast") return handleRoomBroadcast(req, roomId);
      if (action == "kill") return handleKillRoom(req, roomId);
    }
  }

  if (isKnownPath(req.method(), path)) {
    return jsonError(http::status::method_not_allowed,
                     "METHOD_NOT_ALLOWED", "Method not allowed");
  }
  return jsonError(http::status::not_found, "NOT_FOUND", "Not found");
}

awaitable<void> AdminHttpServer::listener() {
  boost::system::error_code ec;
  tcp::acceptor acceptor{io_ctx_};
  acceptor.open(endpoint_.protocol(), ec);
  if (ec) {
    spdlog::error("Admin HTTP open failed: {}", ec.message());
    co_return;
  }
  acceptor.set_option(asio::socket_base::reuse_address(true), ec);
  acceptor.bind(endpoint_, ec);
  if (ec) {
    spdlog::error("Admin HTTP bind {}:{} failed: {}",
                  endpoint_.address().to_string(), endpoint_.port(), ec.message());
    co_return;
  }
  acceptor.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    spdlog::error("Admin HTTP listen failed: {}", ec.message());
    co_return;
  }

  spdlog::info("Admin HTTP API listening on {}:{}",
               endpoint_.address().to_string(), endpoint_.port());

  for (;;) {
    auto socket = co_await acceptor.async_accept(redirect_error(use_awaitable, ec));
    if (ec) {
      spdlog::warn("Admin HTTP accept error: {}", ec.message());
      continue;
    }

    auto stream = beast::tcp_stream{std::move(socket)};
    asio::co_spawn(io_ctx_, session(std::move(stream)), detached);
  }
}

awaitable<void> AdminHttpServer::session(beast::tcp_stream stream) {
  beast::flat_buffer buffer;
  boost::system::error_code ec;

  for (;;) {
    stream.expires_after(std::chrono::seconds(30));

    http::request_parser<http::string_body> parser;
    parser.body_limit(kBodyLimit);
    co_await http::async_read(stream, buffer, parser, redirect_error(use_awaitable, ec));
    if (ec) {
      // body/header 超限不能静默断连，回明确的 413/431 再关
      if (ec == http::error::body_limit || ec == http::error::header_limit) {
        co_await respondLimitExceeded(stream, ec == http::error::body_limit);
      }
      break;
    }

    auto req = parser.release();

    http::response<http::string_body> res;
    res.version(req.version());
    res.set(http::field::server, "freekill-asio");
    res.keep_alive(req.keep_alive());
    applyCors(req, res);

    if (req.method() == http::verb::options) {
      res.result(http::status::no_content);
      res.prepare_payload();
      co_await beast::async_write(stream, http::message_generator{std::move(res)},
                                  redirect_error(use_awaitable, ec));
      if (ec || !req.keep_alive()) break;
      continue;
    }

    if (!checkAuth(req)) {
      auto err = jsonError(http::status::unauthorized, "UNAUTHORIZED",
                           "Missing or invalid Authorization: Bearer <token>");
      res.result(err.status);
      res.set(http::field::content_type, err.contentType);
      res.body() = std::move(err.body);
      res.prepare_payload();
      co_await beast::async_write(stream, http::message_generator{std::move(res)},
                                  redirect_error(use_awaitable, ec));
      if (ec || !req.keep_alive()) break;
      continue;
    }

    auto path = requestPath(req);

    HttpResponse response;
    try {
      response = dispatch(req);
    } catch (const std::exception &e) {
      spdlog::warn("Admin HTTP handler error: {}", e.what());
      response = jsonError(http::status::internal_server_error, "INTERNAL",
                           "Internal server error");
    }

    spdlog::info("[admin-http] {} {} -> {}",
                 std::string(req.method_string()), path,
                 static_cast<unsigned>(response.status));

    res.result(response.status);
    res.set(http::field::content_type, response.contentType);
    res.body() = std::move(response.body);
    res.prepare_payload();
    co_await beast::async_write(stream, http::message_generator{std::move(res)},
                                redirect_error(use_awaitable, ec));
    if (ec || !req.keep_alive()) break;
  }

  stream.socket().shutdown(tcp::socket::shutdown_send, ec);
}

bool AdminHttpServer::isKnownPath(http::verb method, std::string_view path) const {
  auto suffix = " " + std::string(path);
  for (const auto &[key, _] : routes_) {
    // 路由 key 形如 "GET /api/players"，用前导空格锚定避免前缀误配
    if (key.ends_with(suffix)) return true;
  }
  // 动态 GET /api/rooms/{id}
  if (path.starts_with("/api/rooms/")) {
    auto rest = path.substr(std::string_view("/api/rooms/").size());
    if (rest.find('/') == std::string_view::npos) {
      return method != http::verb::get;  // 仅 GET 合法 → 其他方法算 known → 405
    }
    int roomId = 0;
    std::string_view action;
    if (parseRoomAction(path, roomId, action)) {
      return method != http::verb::post;
    }
  }
  return false;
}

awaitable<void> AdminHttpServer::respondLimitExceeded(beast::tcp_stream &stream,
                                                      bool bodyTooLarge) {
  boost::system::error_code ec;

  auto err = bodyTooLarge
      ? jsonError(http::status::payload_too_large, "PAYLOAD_TOO_LARGE",
                  "Request body too large")
      : jsonError(http::status::request_header_fields_too_large,
                  "HEADER_TOO_LARGE", "Request header too large");
  http::response<http::string_body> res;
  res.result(err.status);
  res.version(11);
  res.set(http::field::server, "freekill-asio");
  res.set(http::field::content_type, err.contentType);
  res.keep_alive(false);  // Connection: close
  res.body() = std::move(err.body);
  res.prepare_payload();
  co_await beast::async_write(stream, http::message_generator{std::move(res)},
                              redirect_error(use_awaitable, ec));
  if (ec) co_return;

  // 响应发完后再排空客户端已发但 parser 未消费的数据：
  // 若接收缓冲里还有数据就关连接，内核会发 RST 吞掉上面的响应。
  // 客户端不会主动 EOF，所以用 watchdog 兜底关闭。
  auto executor = co_await asio::this_coro::executor;
  asio::steady_timer watchdog(executor);
  watchdog.expires_after(std::chrono::seconds(2));
  watchdog.async_wait([&stream](boost::system::error_code) {
    boost::system::error_code ignored;
    stream.socket().close(ignored);
  });
  char drainBuf[8192];
  std::size_t drained = 0;
  while (drained < 4 * 1024 * 1024) {  // 总量上限兜底
    auto n = co_await stream.socket().async_read_some(
        asio::buffer(drainBuf), redirect_error(use_awaitable, ec));
    if (ec || n == 0) break;  // EOF / watchdog 关闭 / 出错
    drained += n;
  }
  watchdog.cancel();
}
