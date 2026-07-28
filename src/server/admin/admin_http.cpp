// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/admin/admin_http.h"
#include "server/admin/admin_service.h"
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

AdminHttpServer::AdminHttpServer(tcp::endpoint endpoint) :
  io_ctx_{}, endpoint_{std::move(endpoint)}
{
  registerRoutes();
  spdlog::info("Admin HTTP API listening on port {}", endpoint_.port());
}

AdminHttpServer::~AdminHttpServer() {
  io_ctx_.stop();
  thread_.join();
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
  routes_["GET /api"] = &AdminHttpServer::handleApiHelp;
  routes_["GET /api/lsroom"] = &AdminHttpServer::handleLsRoom;
}

std::string AdminHttpServer::queryParam(const std::string &target, const std::string &key) {
  auto pos = target.find('?');
  if (pos == std::string::npos) return {};

  auto query = target.substr(pos + 1);
  auto prefix = key + "=";
  size_t start = 0;
  while (start < query.size()) {
    auto end = query.find('&', start);
    if (end == std::string::npos) end = query.size();
    auto part = query.substr(start, end - start);
    if (part.starts_with(prefix)) {
      return part.substr(prefix.size());
    }
    start = end + 1;
  }
  return {};
}

AdminHttpServer::HttpResponse AdminHttpServer::handleApiHelp(const http::request<http::string_body> &) {
  return {http::status::ok, "text/html; charset=utf-8", {API_HELP_HTML, sizeof(API_HELP_HTML) - 1}};
}

AdminHttpServer::HttpResponse AdminHttpServer::handleLsRoom(const http::request<http::string_body> &req) {
  auto idStr = queryParam(std::string(req.target()), "id");
  int roomId = -1;
  if (!idStr.empty()) {
    roomId = std::stoi(idStr);
  }
  auto result = AdminService::lsRoomInfo(roomId);
  auto status = result.ok() ? http::status::ok : http::status::not_found;
  return {status, "application/json", result.toHttpResponse().dump()};
}

awaitable<void> AdminHttpServer::listener() {
  auto acceptor = tcp::acceptor{io_ctx_, endpoint_};

  for (;;) {
    boost::system::error_code ec;
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

    http::request<http::string_body> req;
    co_await http::async_read(stream, buffer, req, redirect_error(use_awaitable, ec));
    if (ec) break;

    http::response<http::string_body> res;
    res.version(req.version());
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::access_control_allow_origin, "*");

    auto target = std::string(req.target());
    auto queryPos = target.find('?');
    auto path = queryPos == std::string::npos ? target : target.substr(0, queryPos);
    auto key = std::string(req.method_string()) + " " + path;

    auto it = routes_.find(key);
    if (it != routes_.end()) {
      try {
        auto response = it->second(req);
        res.result(response.status);
        res.set(http::field::content_type, response.contentType);
        res.body() = std::move(response.body);
      } catch (const std::exception &e) {
        spdlog::warn("Admin HTTP handler error: {}", e.what());
        res.result(http::status::internal_server_error);
        res.set(http::field::content_type, "application/json");
        res.body() = R"({"success":false,"error":"Internal server error"})";
      }
    } else {
      res.result(http::status::not_found);
      res.set(http::field::content_type, "application/json");
      res.body() = R"({"success":false,"error":"Not found"})";
    }

    res.prepare_payload();
    co_await beast::async_write(stream, http::message_generator{std::move(res)}, use_awaitable);
    break;
  }

  stream.socket().shutdown(tcp::socket::shutdown_send, ec);
}
