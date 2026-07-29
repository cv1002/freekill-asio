// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/admin/admin_service.h"
#include "server/server.h"
#include "server/room/room_manager.h"
#include "server/room/room.h"
#include "server/room/lobby.h"
#include "server/user/user_manager.h"
#include "server/user/serverplayer.h"
#include "server/gamelogic/roomthread.h"
#include "server/rpc-lua/rpc-lua.h"
#include "core/packman.h"
#include "core/c-wrapper.h"

using json = nlohmann::json;

// ─── PlayerInfo ────────────────────────────────────────────

json PlayerInfo::toJson() const {
  return {
    {"id", id},
    {"connId", connId},
    {"screenName", screenName},
    {"state", state},
    {"avatar", avatar},
  };
}

// ─── RoomSummary ───────────────────────────────────────────

json RoomSummary::toJson() const {
  return {
    {"id", id},
    {"name", name},
    {"mode", mode},
    {"started", started},
    {"password", password.has_value() ? json(password.value()) : json()},
    {"playerCount", playerCount},
  };
}

// ─── RoomDetail ────────────────────────────────────────────

json RoomDetail::toJson() const {
  auto arr = json::array();
  for (auto &p : players) arr.push_back(p.toJson());
  return {
    {"room", room.toJson()},
    {"players", arr},
  };
}

// ─── LobbyInfo ─────────────────────────────────────────────

json LobbyInfo::toJson() const {
  auto arr = json::array();
  for (auto &p : players) arr.push_back(p.toJson());
  return {{"lobby", true}, {"players", arr}};
}

// ─── AdminResult ───────────────────────────────────────────

AdminResult AdminResult::success(json data) {
  return {true, {}, {}, std::move(data), 200};
}

AdminResult AdminResult::error(std::string code, std::string msg, int httpStatus) {
  return {false, std::move(code), std::move(msg), {}, httpStatus};
}

AdminResult::AdminResult(bool ok, std::string code, std::string err, json d, int status)
  : ok_(ok), error_code_(std::move(code)), error_(std::move(err)),
    data_(std::move(d)), http_status_(status) {}

bool AdminResult::ok() const { return ok_; }
const std::string &AdminResult::errorMsg() const { return error_; }
const std::string &AdminResult::errorCode() const { return error_code_; }
const nlohmann::json &AdminResult::data() const { return data_; }
int AdminResult::httpStatus() const { return http_status_; }

json AdminResult::toHttpResponse() const {
  if (ok_) return {{"success", true}, {"data", data_}};
  return {
    {"success", false},
    {"error", {
      {"code", error_code_},
      {"message", error_},
    }},
  };
}

// ─── 辅助函数 ──────────────────────────────────────────────

static PlayerInfo playerToInfo(ServerPlayer &p) {
  return {
    p.getId(),
    p.getConnId(),
    p.getScreenName(),
    std::string(p.getStateString()),
    p.getAvatar(),
  };
}

static RoomSummary roomToSummary(Room &room, bool exposePassword) {
  auto pw = room.getPassword();
  std::optional<std::string> password;
  if (!pw.empty() && exposePassword) {
    password = pw;
  } else if (!pw.empty()) {
    password = "******";  // HTTP 默认脱敏
  }
  return {
    room.getId(),
    room.getName(),
    std::string(room.getGameMode()),
    room.isStarted(),
    password,
    static_cast<int>(room.getPlayers().size()),
  };
}

static std::string formatMsDuration(int64_t time) {
  std::string ret;
  ret.reserve(32);

  auto ms = time % 1000;
  time /= 1000;
  auto sec = time % 60;
  ret = fmt::format("{}.{} seconds", sec, ms) + ret;
  time /= 60;
  if (time == 0) return ret;

  auto min = time % 60;
  ret = fmt::format("{} minutes, ", min) + ret;
  time /= 60;
  if (time == 0) return ret;

  auto hour = time % 24;
  ret = fmt::format("{} hours, ", hour) + ret;
  time /= 24;
  if (time == 0) return ret;

  ret = fmt::format("{} days, ", time) + ret;
  return ret;
}

// ─── AdminService ──────────────────────────────────────────

AdminResult AdminService::lsPlayers() {
  auto &players = Server::instance().user_manager().getPlayers();
  auto arr = json::array();
  for (auto &[_, player] : players) {
    arr.push_back(playerToInfo(*player).toJson());
  }
  return AdminResult::success({{"players", arr}});
}

AdminResult AdminService::lsRoomInfo(int roomId, bool exposePassword) {
  auto &user_manager = Server::instance().user_manager();
  auto &room_manager = Server::instance().room_manager();

  if (roomId > 0) {
    auto room = room_manager.findRoom(roomId).lock();
    if (!room) {
      return AdminResult::error("NOT_FOUND", "No such room.", 404);
    }

    RoomDetail detail;
    detail.room = roomToSummary(*room, exposePassword);
    for (auto pid : room->getPlayers()) {
      auto p = user_manager.findPlayerByConnId(pid).lock();
      if (p) detail.players.push_back(playerToInfo(*p));
    }
    return AdminResult::success(detail.toJson());
  }

  if (roomId == 0) {
    auto lobby = room_manager.lobby().lock();
    LobbyInfo info;
    for (auto &[pid, _] : lobby->getPlayers()) {
      auto p = user_manager.findPlayerByConnId(pid).lock();
      if (p) info.players.push_back(playerToInfo(*p));
    }
    return AdminResult::success(info.toJson());
  }

  const auto &rooms = room_manager.getRooms();
  auto result = json::array();
  for (auto &[id, room] : rooms) {
    result.push_back(roomToSummary(*room, exposePassword).toJson());
  }
  return AdminResult::success(result);
}

AdminResult AdminService::listPackages() {
  auto rows = PackMan::instance().listPackages();
  auto arr = json::array();
  for (auto &row : rows) {
    auto enabledStr = row.contains("enabled") ? row.at("enabled") : "0";
    bool enabled = enabledStr == "1" || enabledStr == "true" || enabledStr == "TRUE";
    arr.push_back({
      {"name", row.at("name")},
      {"url", row.contains("url") ? row.at("url") : ""},
      {"hash", row.at("hash")},
      {"enabled", enabled},
    });
  }
  return AdminResult::success({{"packages", arr}});
}

AdminResult AdminService::serverStat() {
  auto &server = Server::instance();
  auto uptime_ms = server.getUptime();

  json threads = json::array();
  auto &thread_map = server.getThreads();
  std::vector<int> to_remove;

  for (auto &[id, thr] : thread_map) {
    auto roomsCount = thr->getRefCount();
    auto &L = thr->getLua();
    auto outdated = thr->isOutdated();
    if (roomsCount == 0 && outdated) {
      to_remove.push_back(id);
      continue;
    }
    threads.push_back({
      {"id", id},
      {"connection", L.getConnectionInfo()},
      {"roomCount", roomsCount},
      {"outdated", outdated},
    });
  }
  for (auto id : to_remove) {
    server.removeThread(id);
  }

  json data = {
    {"uptimeMs", uptime_ms},
    {"uptime", formatMsDuration(uptime_ms)},
    {"playerCount", static_cast<int>(server.user_manager().getPlayers().size())},
    {"threads", threads},
    {"databaseMemoryMiB",
      static_cast<double>(server.database().getMemUsage()) / 1048576.0},
  };
  return AdminResult::success(std::move(data));
}
