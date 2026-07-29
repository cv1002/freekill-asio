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

#include <chrono>

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

// ─── 干预 ────────────────────────────────────────────────

AdminResult AdminService::kickPlayer(const std::string &name) {
  const auto &players = Server::instance().user_manager().getPlayers();
  for (const auto &[_, p] : players) {
    if (p->getScreenName() == name) {
      p->emitKicked();
      return AdminResult::success({{"kicked", name}});
    }
  }
  return AdminResult::error("NOT_FOUND",
      fmt::format("Can't find any online player named {}.", name), 404);
}

AdminResult AdminService::broadcast(const std::string &message) {
  if (message.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'msg' command needs message body.");
  }
  Server::instance().broadcast("ServerMessage", message);
  return AdminResult::success({});
}

AdminResult AdminService::broadcastRoom(int roomId, const std::string &message) {
  if (message.empty()) {
    return AdminResult::error("BAD_REQUEST",
        "The 'msgroom' command needs <roomId> and message body.");
  }
  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    return AdminResult::error("NOT_FOUND", "No such room.", 404);
  }
  room->doBroadcastNotify(room->getPlayers(), "ServerMessage", message);
  return AdminResult::success({});
}

AdminResult AdminService::killRoom(int roomId) {
  auto &um = Server::instance().user_manager();
  auto &rm = Server::instance().room_manager();
  auto room = rm.findRoom(roomId).lock();
  if (!room) {
    return AdminResult::error("NOT_FOUND", "No such room.", 404);
  }

  spdlog::info("Killing room {}", roomId);
  for (auto pConnId : room->getPlayers()) {
    auto player = um.findPlayerByConnId(pConnId).lock();
    if (player && player->getId() > 0)
      player->emitKicked();
  }
  room->checkAbandoned(Room::NoHuman);
  return AdminResult::success({{"killed", roomId}});
}

AdminResult AdminService::checkLobby() {
  auto lobby = Server::instance().room_manager().lobby().lock();
  lobby->checkAbandoned();
  return AdminResult::success({});
}

// ─── 账号（逻辑照搬原 Shell 实现）─────────────────────────

// 返回是否找到并处理了该用户
static bool banAccountImpl(Sqlite3 &db, const std::string_view &name, bool banned) {
  if (!Sqlite3::checkString(name))
    return false;
  static constexpr const char *sql_find =
    "SELECT id FROM userinfo WHERE name='{}';";
  auto result = db.select(fmt::format(sql_find, name));
  if (result.empty())
    return false;
  auto obj = result[0];
  int id = atoi(obj["id"].c_str());
  db.exec(fmt::format("UPDATE userinfo SET banned={} WHERE id={};",
                  banned ? 1 : 0, id));

  if (banned) {
    auto p = Server::instance().user_manager().findPlayer(id).lock();
    if (p) {
      p->emitKicked();
    }
    spdlog::info("Banned {}.", name);
  } else {
    spdlog::info("Unbanned {}.", name);
  }
  return true;
}

AdminResult AdminService::banAccounts(const std::vector<std::string> &names) {
  if (names.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'ban' command needs at least 1 <name>.");
  }
  auto &db = Server::instance().database();
  auto done = json::array();
  for (auto &name : names) {
    if (banAccountImpl(db, name, true)) done.push_back(name);
  }
  return AdminResult::success({{"banned", done}});
}

AdminResult AdminService::unbanAccounts(const std::vector<std::string> &names) {
  if (names.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'unban' command needs at least 1 <name>.");
  }
  auto &db = Server::instance().database();
  auto done = json::array();
  for (auto &name : names) {
    if (banAccountImpl(db, name, false)) done.push_back(name);
  }
  // 保持 Shell 语义：unban 连带解除 UUID 封禁
  auto result = unbanUuids(names);
  if (!result.ok()) return result;
  return AdminResult::success({{"unbanned", done}});
}

static bool banIPByNameImpl(Sqlite3 &db, const std::string_view &name, bool banned) {
  if (!Sqlite3::checkString(name))
    return false;

  static constexpr const char *sql_find =
    "SELECT id, lastLoginIp FROM userinfo WHERE name='{}';";
  auto result = db.select(fmt::format(sql_find, name));
  if (result.empty())
    return false;
  auto obj = result[0];
  int id = atoi(obj["id"].c_str());
  auto addr = obj["lastLoginIp"];

  if (banned) {
    db.exec(fmt::format("INSERT INTO banip VALUES('{}');", addr));

    auto p = Server::instance().user_manager().findPlayer(id).lock();
    if (p) {
      p->emitKicked();
    }
    spdlog::info("Banned IP {}.", addr);
  } else {
    db.exec(fmt::format("DELETE FROM banip WHERE ip='{}';", addr));
    spdlog::info("Unbanned IP {}.", addr);
  }
  return true;
}

AdminResult AdminService::banIpsByNames(const std::vector<std::string> &names) {
  if (names.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'banip' command needs at least 1 <name>.");
  }
  auto &db = Server::instance().database();
  auto done = json::array();
  for (auto &name : names) {
    if (banIPByNameImpl(db, name, true)) done.push_back(name);
  }
  return AdminResult::success({{"bannedIps", done}});
}

AdminResult AdminService::unbanIps(const std::vector<std::string> &names) {
  if (names.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'unbanip' command needs at least 1 <name>.");
  }
  auto &db = Server::instance().database();
  auto done = json::array();
  for (auto &name : names) {
    if (banIPByNameImpl(db, name, false)) done.push_back(name);
  }
  return AdminResult::success({{"unbannedIps", done}});
}

static bool banUuidByNameImpl(Sqlite3 &db, const std::string_view &name, bool banned) {
  if (!Sqlite3::checkString(name))
    return false;
  static constexpr const char *sql_find =
    "SELECT id FROM userinfo WHERE name='{}';";
  auto result = db.select(fmt::format(sql_find, name));
  if (result.empty())
    return false;
  auto obj = result[0];
  int id = atoi(obj["id"].c_str());

  auto result2 = db.select(fmt::format("SELECT * FROM uuidinfo WHERE id={};", id));
  if (result2.empty())
    return false;

  auto uuid = result2[0]["uuid"];

  if (banned) {
    db.exec(fmt::format("INSERT INTO banuuid VALUES('{}');", uuid));

    auto p = Server::instance().user_manager().findPlayer(id).lock();
    if (p) {
      p->emitKicked();
    }
    spdlog::info("Banned UUID {}.", uuid);
  } else {
    db.exec(fmt::format("DELETE FROM banuuid WHERE uuid='{}';", uuid));
    spdlog::info("Unbanned UUID {}.", uuid);
  }
  return true;
}

AdminResult AdminService::banUuidsByNames(const std::vector<std::string> &names) {
  if (names.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'banuuid' command needs at least 1 <name>.");
  }
  auto &db = Server::instance().database();
  auto done = json::array();
  for (auto &name : names) {
    if (banUuidByNameImpl(db, name, true)) done.push_back(name);
  }
  return AdminResult::success({{"bannedUuids", done}});
}

AdminResult AdminService::unbanUuids(const std::vector<std::string> &names) {
  if (names.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'unbanuuid' command needs at least 1 <name>.");
  }
  auto &db = Server::instance().database();
  auto done = json::array();
  for (auto &name : names) {
    if (banUuidByNameImpl(db, name, false)) done.push_back(name);
  }
  return AdminResult::success({{"unbannedUuids", done}});
}

// ─── 时长解析（原 shell.cpp 实现下沉）─────────────────────

static constexpr const char *kInvalidDuration = "Invalid duration value. "
  "Possible choices: ??m (minute), ??h (hour), ??d (day) and ??mo (month, 30 days).";

static std::optional<std::chrono::seconds> parseDuration(const std::string &durationStr) {
  size_t pos;
  long value;
  try {
    value = std::stol(durationStr, &pos);
  } catch (const std::exception &) {
    return std::nullopt;
  }

  if (value < 0) return std::nullopt;

  using namespace std::chrono;
  std::string unit = durationStr.substr(pos);

  if (unit == "m") return value * 60s;
  if (unit == "h") return value * 3600s;
  if (unit == "d") return value * 86400s;
  if (unit == "mo") return value * 2592000s;
  return std::nullopt;
}

AdminResult AdminService::tempBan(const std::string &name, const std::string &durationStr) {
  auto duration = parseDuration(durationStr);
  if (!duration) {
    return AdminResult::error("BAD_REQUEST", kInvalidDuration);
  }

  using namespace std::chrono;
  auto end_tp = system_clock::now() + *duration;
  auto expireTimestamp = duration_cast<seconds>(end_tp.time_since_epoch()).count();

  if (!Sqlite3::checkString(name)) {
    return AdminResult::error("BAD_REQUEST", "Invalid name.");
  }

  auto &db = Server::instance().database();
  static constexpr const char *sql_find =
    "SELECT id FROM userinfo WHERE name='{}';";
  auto result = db.select(fmt::format(sql_find, name));
  if (result.empty()) {
    return AdminResult::error("NOT_FOUND", fmt::format("No such user {}.", name), 404);
  }

  auto obj = result[0];
  int id = atoi(obj["id"].c_str());
  db.exec(fmt::format("UPDATE userinfo SET banned=1 WHERE id={};", id));
  db.exec(fmt::format(
    "REPLACE INTO tempban (uid, expireAt) VALUES ({}, {});", id, expireTimestamp));

  auto p = Server::instance().user_manager().findPlayer(id).lock();
  if (p) {
    p->emitKicked();
  }

  std::time_t now_time_t = system_clock::to_time_t(end_tp);
  std::tm local_tm = *std::localtime(&now_time_t);
  spdlog::info("Banned {} until {:04}-{:02}-{:02} {:02}:{:02}:{:02}.", name.c_str(),
               local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
               local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
  return AdminResult::success({{"banned", name}, {"expireAt", expireTimestamp}});
}

AdminResult AdminService::tempMute(const std::string &name, const std::string &durationStr) {
  auto duration = parseDuration(durationStr);
  if (!duration) {
    return AdminResult::error("BAD_REQUEST", kInvalidDuration);
  }
  int mute_type = 1; // 1为完全禁言

  using namespace std::chrono;
  auto end_tp = system_clock::now() + *duration;
  auto expireTimestamp = duration_cast<seconds>(end_tp.time_since_epoch()).count();

  if (!Sqlite3::checkString(name)) {
    return AdminResult::error("BAD_REQUEST", "Invalid name.");
  }

  auto &db = Server::instance().database();
  static constexpr const char *sql_find =
    "SELECT id FROM userinfo WHERE name='{}';";
  auto result = db.select(fmt::format(sql_find, name));
  if (result.empty()) {
    return AdminResult::error("NOT_FOUND", fmt::format("No such user {}.", name), 404);
  }

  auto obj = result[0];
  int id = atoi(obj["id"].c_str());
  db.exec(fmt::format(
    "REPLACE INTO tempmute (uid, expireAt, type) VALUES ({}, {}, {});", id, expireTimestamp, mute_type));

  std::time_t now_time_t = system_clock::to_time_t(end_tp);
  std::tm local_tm = *std::localtime(&now_time_t);
  spdlog::info("Muted {} until {:04}-{:02}-{:02} {:02}:{:02}:{:02}.", name.c_str(),
              local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
              local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
  return AdminResult::success({{"muted", name}, {"expireAt", expireTimestamp}});
}

AdminResult AdminService::unmute(const std::vector<std::string> &names) {
  if (names.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'unmute' command needs at least 1 <name>.");
  }
  auto &db = Server::instance().database();
  auto done = json::array();

  for (auto &name : names) {
    if (!Sqlite3::checkString(name))
      continue;

    static constexpr const char *sql_find =
      "SELECT id FROM userinfo WHERE name='{}';";
    auto result = db.select(fmt::format(sql_find, name));
    if (result.empty()) {
      spdlog::info("Player {} not found.", name.c_str());
      continue;
    }

    auto obj = result[0];
    int id = atoi(obj["id"].c_str());
    db.exec(fmt::format("DELETE FROM tempmute WHERE uid={};", id));
    spdlog::info("Unmuted player {}.", name.c_str());
    done.push_back(name);
  }
  return AdminResult::success({{"unmuted", done}});
}

AdminResult AdminService::whitelist(const std::string &action,
                                    const std::vector<std::string> &names) {
  if (names.empty() || (action != "add" && action != "rm")) {
    return AdminResult::error("BAD_REQUEST", "usage: whitelist add/rm <names>...");
  }

  auto &server = Server::instance();
  auto &db = server.database();
  auto done = json::array();

  server.beginTransaction();
  if (action == "add") {
    for (auto &name : names) {
      if (!Sqlite3::checkString(name))
        continue;
      db.exec(fmt::format("INSERT INTO whitelist VALUES ('{}');", name));
      done.push_back(name);
    }
  } else {
    for (auto &name : names) {
      if (!Sqlite3::checkString(name))
        continue;
      db.exec(fmt::format("DELETE FROM whitelist WHERE name='{}';", name));
      done.push_back(name);
    }
  }
  server.endTransaction();

  return AdminResult::success({{"action", action}, {"names", done}});
}

AdminResult AdminService::resetPassword(const std::vector<std::string> &names) {
  if (names.empty()) {
    return AdminResult::error("BAD_REQUEST",
        "The 'resetpassword' command needs at least 1 <name>.");
  }

  auto &db = Server::instance().database();
  auto done = json::array();
  std::string missing;
  for (auto &name : names) {
    auto result = db.select(fmt::format(
        "SELECT id FROM userinfo WHERE name='{}';", name));
    if (result.empty()) {
      if (!missing.empty()) missing += ", ";
      missing += name;
      continue;
    }
    // 重置为1234
    db.exec(fmt::format("UPDATE userinfo SET password="
          "'dbdc2ad3d9625407f55674a00b58904242545bfafedac67485ac398508403ade',"
          "salt='00000000' WHERE name='{}';", name));
    done.push_back(name);
  }

  if (!missing.empty()) {
    return AdminResult::error("NOT_FOUND",
        fmt::format("No such user: {}.", missing), 404);
  }
  return AdminResult::success({{"reset", done}});
}

// ─── 配置 ────────────────────────────────────────────────

AdminResult AdminService::reloadConfig() {
  Server::instance().reloadConfig();
  spdlog::info("Reloaded server config file.");
  return AdminResult::success({});
}

// ─── 包管理 ──────────────────────────────────────────────

static json packageSideEffect() {
  return {
    {"md5Refreshed", true},
    {"sideEffect", "Lobby players must re-login with new package MD5."},
  };
}

AdminResult AdminService::installPackage(const std::string &url) {
  if (url.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'install' command need a URL to install.");
  }
  PackMan::instance().downloadNewPack(url.c_str());
  Server::instance().refreshMd5();
  auto data = packageSideEffect();
  data["url"] = url;
  return AdminResult::success(std::move(data));
}

AdminResult AdminService::removePackage(const std::string &name) {
  if (name.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'remove' command need a package name to remove.");
  }
  PackMan::instance().removePack(name.c_str());
  Server::instance().refreshMd5();
  auto data = packageSideEffect();
  data["name"] = name;
  return AdminResult::success(std::move(data));
}

AdminResult AdminService::enablePackage(const std::string &name) {
  if (name.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'enable' command need a package name to enable.");
  }
  PackMan::instance().enablePack(name.c_str());
  Server::instance().refreshMd5();
  auto data = packageSideEffect();
  data["name"] = name;
  return AdminResult::success(std::move(data));
}

AdminResult AdminService::disablePackage(const std::string &name) {
  if (name.empty()) {
    return AdminResult::error("BAD_REQUEST", "The 'disable' command need a package name to disable.");
  }
  PackMan::instance().disablePack(name.c_str());
  Server::instance().refreshMd5();
  auto data = packageSideEffect();
  data["name"] = name;
  return AdminResult::success(std::move(data));
}

AdminResult AdminService::upgradePackage(const std::string &name) {
  if (name.empty()) {
    auto arr = PackMan::instance().listPackages();
    for (auto &a : arr) {
      PackMan::instance().upgradePack(a["name"].c_str());
    }
  } else {
    PackMan::instance().upgradePack(name.c_str());
  }
  Server::instance().refreshMd5();
  auto data = packageSideEffect();
  if (!name.empty()) data["name"] = name;
  else data["upgradedAll"] = true;
  return AdminResult::success(std::move(data));
}

AdminResult AdminService::syncPackages() {
  PackMan::instance().syncCommitHashToDatabase();
  Server::instance().refreshMd5();
  auto data = packageSideEffect();
  data["synced"] = true;
  return AdminResult::success(std::move(data));
}
