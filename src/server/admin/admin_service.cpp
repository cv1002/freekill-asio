// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/admin/admin_service.h"
#include "server/server.h"
#include "server/room/room_manager.h"
#include "server/room/room.h"
#include "server/room/lobby.h"
#include "server/user/user_manager.h"
#include "server/user/serverplayer.h"

using json = nlohmann::json;

// ─── PlayerInfo ────────────────────────────────────────────

json PlayerInfo::toJson() const {
  return {
    {"id", id},
    {"connId", connId},
    {"screenName", screenName},
    {"state", state},
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

AdminResult AdminResult::success(json data) { return {true, {}, std::move(data)}; }
AdminResult AdminResult::error(std::string msg) { return {false, std::move(msg), {}}; }

AdminResult::AdminResult(bool ok, std::string err, json d)
  : ok_(ok), error_(std::move(err)), data_(std::move(d)) {}

bool AdminResult::ok() const { return ok_; }
const std::string &AdminResult::errorMsg() const { return error_; }
const nlohmann::json &AdminResult::data() const { return data_; }

json AdminResult::toHttpResponse() const {
  if (ok_) return {{"success", true}, {"data", data_}};
  return {{"success", false}, {"error", error_}};
}

// ─── AdminService ──────────────────────────────────────────

static PlayerInfo playerToInfo(ServerPlayer &p) {
  return {p.getId(), p.getConnId(), p.getScreenName(), std::string(p.getStateString())};
}

static RoomSummary roomToSummary(Room &room) {
  auto pw = room.getPassword();
  return {
    room.getId(),
    room.getName(),
    std::string(room.getGameMode()),
    room.isStarted(),
    pw.empty() ? std::nullopt : std::optional<std::string>(pw),
    static_cast<int>(room.getPlayers().size()),
  };
}

AdminResult AdminService::lsRoomInfo(int roomId) {
  auto &user_manager = Server::instance().user_manager();
  auto &room_manager = Server::instance().room_manager();

  if (roomId > 0) {
    auto room = room_manager.findRoom(roomId).lock();
    if (!room) {
      return AdminResult::error("No such room.");
    }

    RoomDetail detail;
    detail.room = roomToSummary(*room);
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
    result.push_back(roomToSummary(*room).toJson());
  }
  return AdminResult::success(result);
}
