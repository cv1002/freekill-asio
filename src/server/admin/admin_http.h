// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "server/server.h"
#include "server/admin/admin_service.h"

#include <nlohmann/json.hpp>

class AdminHttpServer {
public:
  AdminHttpServer(boost::asio::ip::tcp::endpoint endpoint, AdminHttpConfig config);
  ~AdminHttpServer();

  AdminHttpServer(AdminHttpServer &) = delete;
  AdminHttpServer(AdminHttpServer &&) = delete;

  void start();

private:
  boost::asio::io_context io_ctx_;
  boost::asio::ip::tcp::endpoint endpoint_;
  AdminHttpConfig config_;
  std::thread thread_;

  void run();
  boost::asio::awaitable<void> listener();
  boost::asio::awaitable<void> session(boost::beast::tcp_stream stream);

  struct HttpResponse {
    boost::beast::http::status status;
    std::string contentType;
    std::string body;
  };

  using Request = boost::beast::http::request<boost::beast::http::string_body>;
  using Handler = std::function<HttpResponse(const Request &)>;
  std::unordered_map<std::string, Handler> routes_;

  void registerRoutes();
  bool checkAuth(const Request &req) const;  // Authorization: Bearer <token>
  void applyCors(const Request &req, boost::beast::http::response<boost::beast::http::string_body> &res) const;

  static std::string requestPath(const Request &req);
  // 解析 /api/rooms/{id}：必须是完整非负整数
  static bool parseRoomId(std::string_view text, int &out);
  // 解析动态房间写路径：/api/rooms/{id}/broadcast|kill
  static bool parseRoomAction(std::string_view path, int &roomId, std::string_view &action);
  // 路径是否匹配某条路由（含动态房间路径），用于区分 404 与 405
  bool isKnownPath(boost::beast::http::verb method, std::string_view path) const;
  // body/header 超限时回 413/431：先排空客户端已发数据，避免直接关连接 RST 吞掉响应
  boost::asio::awaitable<void> respondLimitExceeded(boost::beast::tcp_stream &stream,
                                                    bool bodyTooLarge);

  // 投递到主 io_context 执行业务，避免跨线程碰游戏状态
  HttpResponse runOnMain(std::function<AdminResult()> fn) const;

  static HttpResponse fromAdminResult(const AdminResult &result);
  static HttpResponse jsonError(boost::beast::http::status status,
                                std::string_view code, std::string_view message);

  // 解析 JSON body；失败时 err 已填好，返回 false
  static bool parseJsonBody(const Request &req, nlohmann::json &out, HttpResponse &err);
  static bool requireString(const nlohmann::json &body, const char *key,
                            std::string &out, HttpResponse &err);
  static bool requireStringArray(const nlohmann::json &body, const char *key,
                                 std::vector<std::string> &out, HttpResponse &err);

  HttpResponse dispatch(const Request &req);
  HttpResponse handleApiHelp(const Request &req);
  HttpResponse handleLsPlayers(const Request &req);
  HttpResponse handleLsRooms(const Request &req);
  HttpResponse handleLsRoomById(const Request &req, int roomId);
  HttpResponse handleServerStat(const Request &req);

  HttpResponse handleKick(const Request &req);
  HttpResponse handleBroadcast(const Request &req);
  HttpResponse handleRoomBroadcast(const Request &req, int roomId);
  HttpResponse handleKillRoom(const Request &req, int roomId);
  HttpResponse handleCheckLobby(const Request &req);

  HttpResponse handleBan(const Request &req);
  HttpResponse handleUnban(const Request &req);
  HttpResponse handleBanIp(const Request &req);
  HttpResponse handleUnbanIp(const Request &req);
  HttpResponse handleBanUuid(const Request &req);
  HttpResponse handleUnbanUuid(const Request &req);
  HttpResponse handleTempBan(const Request &req);
  HttpResponse handleTempMute(const Request &req);
  HttpResponse handleUnmute(const Request &req);
  HttpResponse handleWhitelist(const Request &req);
  HttpResponse handleResetPassword(const Request &req);

  HttpResponse handleReloadConfig(const Request &req);
};
