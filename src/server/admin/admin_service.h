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
  // ─── 只读 ───
  static AdminResult lsPlayers();
  // exposePassword 默认 true（Shell 本机可见明文）；HTTP 传入 adminHttp.exposeRoomPassword
  static AdminResult lsRoomInfo(int roomId = -1, bool exposePassword = true);
  static AdminResult listPackages();
  static AdminResult serverStat();

  // ─── 干预 ───
  // 写方法必须在主 io_context 上调用（HTTP 经 runOnMain，Shell 经同名 helper）
  static AdminResult kickPlayer(const std::string &name);
  static AdminResult broadcast(const std::string &message);
  static AdminResult broadcastRoom(int roomId, const std::string &message);
  static AdminResult killRoom(int roomId);
  static AdminResult checkLobby();

  // ─── 账号 ───
  static AdminResult banAccounts(const std::vector<std::string> &names);
  // 与 Shell 语义一致：unban 连带解除对应 UUID 封禁
  static AdminResult unbanAccounts(const std::vector<std::string> &names);
  static AdminResult banIpsByNames(const std::vector<std::string> &names);
  static AdminResult unbanIps(const std::vector<std::string> &names);
  static AdminResult banUuidsByNames(const std::vector<std::string> &names);
  static AdminResult unbanUuids(const std::vector<std::string> &names);
  // durationStr 与 Shell 一致：??m / ??h / ??d / ??mo
  static AdminResult tempBan(const std::string &name, const std::string &durationStr);
  static AdminResult tempMute(const std::string &name, const std::string &durationStr);
  static AdminResult unmute(const std::vector<std::string> &names);
  // action 仅支持 "add" / "rm"
  static AdminResult whitelist(const std::string &action, const std::vector<std::string> &names);
  static AdminResult resetPassword(const std::vector<std::string> &names);

  // ─── 配置 ───
  static AdminResult reloadConfig();

  // ─── 包管理（会 refreshMd5，大厅玩家需重登）───
  static AdminResult installPackage(const std::string &url);
  static AdminResult removePackage(const std::string &name);
  static AdminResult enablePackage(const std::string &name);
  static AdminResult disablePackage(const std::string &name);
  // name 为空则升级全部
  static AdminResult upgradePackage(const std::string &name = {});
  static AdminResult syncPackages();
};
