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
  static AdminResult error(std::string msg);

  bool ok() const;
  const std::string &errorMsg() const;
  const nlohmann::json &data() const;

  nlohmann::json toHttpResponse() const;

private:
  bool ok_;
  std::string error_;
  nlohmann::json data_;

  AdminResult(bool ok, std::string err, nlohmann::json d);
};

class AdminService {
public:
  static AdminResult lsRoomInfo(int roomId = -1);
};
