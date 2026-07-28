// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <nlohmann/json.hpp>

class AdminHttpServer {
public:
  AdminHttpServer(boost::asio::ip::tcp::endpoint endpoint);
  ~AdminHttpServer();

  AdminHttpServer(AdminHttpServer &) = delete;
  AdminHttpServer(AdminHttpServer &&) = delete;

  void start();

private:
  boost::asio::io_context io_ctx_;
  boost::asio::ip::tcp::endpoint endpoint_;
  std::thread thread_;

  void run();
  boost::asio::awaitable<void> listener();
  boost::asio::awaitable<void> session(boost::beast::tcp_stream stream);

  struct HttpResponse {
    boost::beast::http::status status;
    std::string contentType;
    std::string body;
  };
  using Handler = std::function<HttpResponse(const boost::beast::http::request<boost::beast::http::string_body> &)>;
  std::unordered_map<std::string, Handler> routes_;

  void registerRoutes();
  static HttpResponse handleLsRoom(const boost::beast::http::request<boost::beast::http::string_body> &req);
  static HttpResponse handleApiHelp(const boost::beast::http::request<boost::beast::http::string_body> &req);
  static std::string queryParam(const std::string &target, const std::string &key);
};
