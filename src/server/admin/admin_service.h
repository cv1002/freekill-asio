// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

struct PlayerInfo {
  int id;
  int connId;
  std::string screenName;
  std::string state;
  std::string avatar;

  nlohmann::json toJson() const;
};

struct RoomSummary {
  int id;
  std::string name;
  std::string mode;
  bool started;
  std::optional<std::string> password;
  int playerCount;

  nlohmann::json toJson() const;
};

struct RoomDetail {
  RoomSummary room;
  std::vector<PlayerInfo> players;

  nlohmann::json toJson() const;
};

struct LobbyInfo {
  std::vector<PlayerInfo> players;

  nlohmann::json toJson() const;
};

class AdminResult {
public:
  static AdminResult success(nlohmann::json data);
  static AdminResult error(std::string code, std::string msg, int httpStatus = 400);

  bool ok() const;
  const std::string &errorMsg() const;
  const std::string &errorCode() const;
  const nlohmann::json &data() const;
  int httpStatus() const;

  nlohmann::json toHttpResponse() const;

private:
  bool ok_;
  std::string error_code_;
  std::string error_;
  nlohmann::json data_;
  int http_status_;

  AdminResult(bool ok, std::string code, std::string err, nlohmann::json d, int status);
};

class AdminService {
public:
  static AdminResult lsPlayers();
  // exposePassword 默认 true（Shell 本机可见明文）；HTTP 传入 adminHttp.exposeRoomPassword
  static AdminResult lsRoomInfo(int roomId = -1, bool exposePassword = true);
  static AdminResult listPackages();
  static AdminResult serverStat();
};
