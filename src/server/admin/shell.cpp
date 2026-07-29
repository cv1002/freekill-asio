// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/admin/shell.h"
#include "server/admin/admin_service.h"
#include "core/packman.h"
// #include "server/rpc-lua/rpc-lua.h"
#include "server/server.h"
#include "server/user/serverplayer.h"
#include "server/user/user_manager.h"
#include "server/room/room_manager.h"
#include "server/room/room.h"
#include "server/room/lobby.h"
#include "server/rpc-lua/rpc-lua.h"
#include "server/gamelogic/roomthread.h"
#include "core/util.h"
#include "core/c-wrapper.h"

#include <nlohmann/json.hpp>

#include <readline/history.h>
#include <readline/readline.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstdio>
#include <cstring>
#include <pthread.h>

namespace asio = boost::asio;

static constexpr const char *prompt = "fk-asio> ";

// Shell 线程调写方法也派发到主 io_context 等待，避免跨线程碰游戏状态
static AdminResult runOnMain(std::function<AdminResult()> fn) {
  return asio::dispatch(Server::instance().context(),
                        asio::use_future(std::move(fn))).get();
}

void Shell::helpCommand(StringList &) {
  spdlog::info("Frequently used commands:");
#define HELP_MSG(a, b)                                                         \
  spdlog::info((a), Color((b), fkShell::Cyan));

  spdlog::info("===== General commands =====");
  HELP_MSG("{}: Display this help message.", "help");
  HELP_MSG("{}: Shut down the server.", "quit");
  HELP_MSG("{}: Crash the server. Useful when encounter dead loop.", "crash");
  HELP_MSG("{}: View status of server.", "stat/gc");
  HELP_MSG("{}: Reload server config file.", "reloadconf/r");

  spdlog::info("");
  spdlog::info("===== Inspect commands =====");
  HELP_MSG("{}: List all online players.", "lsplayer");
  HELP_MSG("{}: List all running rooms, or show player of room by an <id>.", "lsroom");
  HELP_MSG("{}: Broadcast message.", "msg/m");
  HELP_MSG("{}: Broadcast message to a room.", "msgroom/mr");
  HELP_MSG("{}: Kick a player by his <name>.", "kick");
  HELP_MSG("{}: Kick all players in a room, then abandon it.", "killroom");
  HELP_MSG("{}: Delete dead players in the lobby.", "checklobby");

  spdlog::info("");
  spdlog::info("===== Account commands =====");
  HELP_MSG("{}: Ban 1 or more accounts, IP, UUID by their <name>.", "ban");
  HELP_MSG("{}: Unban 1 or more accounts by their <name>.", "unban");
  HELP_MSG(
      "{}: Ban 1 or more IP address. "
      "At least 1 <name> required.",
      "banip");
  HELP_MSG(
      "{}: Unban 1 or more IP address. "
      "At least 1 <name> required.",
      "unbanip");
  HELP_MSG(
      "{}: Ban 1 or more UUID. "
      "At least 1 <name> required.",
      "banuuid");
  HELP_MSG(
      "{}: Unban 1 or more UUID. "
      "At least 1 <name> required.",
      "unbanuuid");
  HELP_MSG("{}: Ban an accounts by his <name> and <duration> (??m/??h/??d/??mo).", "tempban");
  HELP_MSG("{}: Ban a player's chat by his <name> and <duration> (??m/??h/??d/??mo).", "tempmute");
  HELP_MSG("{}: Unban 1 or more players' chat by their <name>.", "unmute");
  HELP_MSG("{}: Add or remove names from whitelist.", "whitelist");
  HELP_MSG("{}: reset <name>'s password to 1234.", "resetpassword/rp");

  spdlog::info("");
  spdlog::info("===== Package commands =====");
  HELP_MSG("{}: Install a new package from <url>.", "install");
  HELP_MSG("{}: Remove a package.", "remove");
  HELP_MSG("{}: List all packages.", "pkgs");
  HELP_MSG("{}: Get packages hash from file system and write to database.", "syncpkgs");
  HELP_MSG("{}: Enable a package.", "enable");
  HELP_MSG("{}: Disable a package.", "disable");
  HELP_MSG("{}: Upgrade a package. Leave empty to upgrade all.", "upgrade/u");
  spdlog::info("For more commands, check the documentation.");

#undef HELP_MSG
}

Shell::~Shell() {
  rl_clear_history();
  m_thread.join();
}

void Shell::start() {
  m_thread = std::thread(&Shell::run, this);
}

void Shell::lspCommand(StringList &) {
  auto result = AdminService::lsPlayers();
  if (!result.ok()) {
    spdlog::info(result.errorMsg());
    return;
  }

  auto &players = result.data()["players"];
  if (players.empty()) {
    spdlog::info("No online player.");
    return;
  }
  spdlog::info("Current {} online player(s) are:", players.size());
  for (auto &player : players) {
    spdlog::info("{} {{id:{}, connId:{}, state:{}}}",
                 player["screenName"].get<std::string>(),
                 player["id"].get<int>(),
                 player["connId"].get<int>(),
                 player["state"].get<std::string>());
  }
}

void Shell::lsrCommand(StringList &list) {
  int roomId = -1;
  if (!list.empty() && !list[0].empty()) {
    roomId = std::atoi(list[0].c_str());
  }

  auto result = AdminService::lsRoomInfo(roomId);

  if (!result.ok()) {
    spdlog::info(result.errorMsg());
    return;
  }

  auto &data = result.data();

  if (data.is_array()) {
    if (data.empty()) {
      spdlog::info("No running room.");
      return;
    }
    spdlog::info("Current {} running rooms are:", data.size());
    for (auto &r : data) {
      auto password = r["password"];
      auto pwStr = password.is_null() ? "<nil>" : password.get<std::string>();
      spdlog::info("{}, {} {{mode:{}, running={}, pw:{}}}",
        r["id"].get<int>(), r["name"].get<std::string>(),
        r["mode"].get<std::string>(), r["started"].get<bool>(), pwStr);
    }
  } else if (data.contains("lobby")) {
    spdlog::info("You are viewing lobby, players in lobby are:");
    for (auto &p : data["players"]) {
      spdlog::info("{} {{id:{}, connId:{}, state:{}}}",
        p["screenName"].get<std::string>(), p["id"].get<int>(),
        p["connId"].get<int>(), p["state"].get<std::string>());
    }
  } else if (data.contains("room")) {
    auto &r = data["room"];
    auto password = r["password"];
    auto pwStr = password.is_null() ? "<nil>" : password.get<std::string>();
    spdlog::info("{}, {} {{mode:{}, running={}, pw:{}}}",
      r["id"].get<int>(), r["name"].get<std::string>(),
      r["mode"].get<std::string>(), r["started"].get<bool>(), pwStr);
    spdlog::info("Players in this room:");
    for (auto &p : data["players"]) {
      spdlog::info("{} {{id:{}, connId:{}, state:{}}}",
        p["screenName"].get<std::string>(), p["id"].get<int>(),
        p["connId"].get<int>(), p["state"].get<std::string>());
    }
  }
}

void Shell::installCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'install' command need a URL to install.");
    return;
  }
  auto result = AdminService::installPackage(list[0]);
  if (!result.ok()) spdlog::warn(result.errorMsg());
}

void Shell::removeCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'remove' command need a package name to remove.");
    return;
  }
  auto result = AdminService::removePackage(list[0]);
  if (!result.ok()) spdlog::warn(result.errorMsg());
}

void Shell::upgradeCommand(StringList &list) {
  auto result = AdminService::upgradePackage(list.empty() ? "" : list[0]);
  if (!result.ok()) spdlog::warn(result.errorMsg());
}

void Shell::enableCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'enable' command need a package name to enable.");
    return;
  }
  auto result = AdminService::enablePackage(list[0]);
  if (!result.ok()) spdlog::warn(result.errorMsg());
}

void Shell::disableCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'disable' command need a package name to disable.");
    return;
  }
  auto result = AdminService::disablePackage(list[0]);
  if (!result.ok()) spdlog::warn(result.errorMsg());
}

void Shell::lspkgCommand(StringList &) {
  auto result = AdminService::listPackages();
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
    return;
  }
  spdlog::info("Name\tVersion\t\tEnabled");
  spdlog::info("------------------------------");
  for (auto &a : result.data()["packages"]) {
    auto hash = a["hash"].get<std::string>();
    spdlog::info("{}\t{}\t{}", a["name"].get<std::string>(),
                 hash.substr(0, std::min<size_t>(8, hash.size())),
                 a["enabled"].get<bool>());
  }
}

void Shell::syncpkgCommand(StringList &) {
  auto result = AdminService::syncPackages();
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
    return;
  }
  spdlog::info("Done.");
}

void Shell::kickCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'kick' command needs a player name.");
    return;
  }

  auto playerName = list[0];
  auto result = runOnMain([&] { return AdminService::kickPlayer(playerName); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::msgCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'msg' command needs message body.");
    return;
  }

  std::string msg;
  for (auto &s : list) {
    msg += s;
    msg += ' ';
  }
  runOnMain([&] { return AdminService::broadcast(msg); });
}

void Shell::msgRoomCommand(StringList &list) {
  if (list.size() < 2) {
    spdlog::warn("The 'msgroom' command needs <roomId> and message body.");
    return;
  }

  auto roomId = atoi(list[0].c_str());
  std::string msg;
  for (size_t i = 1; i < list.size(); i++) {
    msg += list[i];
    msg += ' ';
  }
  auto result = runOnMain([&] { return AdminService::broadcastRoom(roomId, msg); });
  if (!result.ok()) {
    spdlog::info(result.errorMsg());
  }
}

void Shell::banCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'ban' command needs at least 1 <name>.");
    return;
  }

  auto result = runOnMain([&] { return AdminService::banAccounts(list); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::unbanCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'unban' command needs at least 1 <name>.");
    return;
  }

  auto result = runOnMain([&] { return AdminService::unbanAccounts(list); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::banipCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'banip' command needs at least 1 <name>.");
    return;
  }

  auto result = runOnMain([&] { return AdminService::banIpsByNames(list); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::unbanipCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'unbanip' command needs at least 1 <name>.");
    return;
  }

  auto result = runOnMain([&] { return AdminService::unbanIps(list); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::banUuidCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'banuuid' command needs at least 1 <name>.");
    return;
  }

  auto result = runOnMain([&] { return AdminService::banUuidsByNames(list); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::unbanUuidCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'unbanuuid' command needs at least 1 <name>.");
    return;
  }

  auto result = runOnMain([&] { return AdminService::unbanUuids(list); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::tempbanCommand(StringList &list) {
  if (list.size() != 2) {
    spdlog::warn("usage: tempban <name> <duration>");
    return;
  }

  auto result = runOnMain([&] { return AdminService::tempBan(list[0], list[1]); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::tempmuteCommand(StringList &list) {
  if (list.size() != 2) {
    spdlog::warn("usage: tempmute <name> <duration>");
    return;
  }

  auto result = runOnMain([&] { return AdminService::tempMute(list[0], list[1]); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::unmuteCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'unmute' command needs at least 1 <name>.");
    return;
  }

  auto result = runOnMain([&] { return AdminService::unmute(list); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::whitelistCommand(StringList &list) {
  if (list.size() < 2) {
    spdlog::warn("usage: whitelist add/rm <names>...");
    return;
  }

  auto op = list[0];
  StringList names(list.begin() + 1, list.end());
  auto result = runOnMain([&] { return AdminService::whitelist(op, names); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::reloadConfCommand(StringList &) {
  auto result = runOnMain([] { return AdminService::reloadConfig(); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::resetPasswordCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'resetpassword' command needs at least 1 <name>.");
    return;
  }

  auto result = runOnMain([&] { return AdminService::resetPassword(list); });
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
  }
}

void Shell::statCommand(StringList &) {
  auto result = AdminService::serverStat();
  if (!result.ok()) {
    spdlog::warn(result.errorMsg());
    return;
  }

  auto &data = result.data();
  spdlog::info("uptime: {}", data["uptime"].get<std::string>());
  spdlog::info("Player(s) logged in: {}", data["playerCount"].get<int>());

  for (auto &thr : data["threads"]) {
    spdlog::info("RoomThread {} | {} | {} room(s) {}",
                 thr["id"].get<int>(),
                 thr["connection"].get<std::string>(),
                 thr["roomCount"].get<int>(),
                 thr["outdated"].get<bool>() ? "| Outdated" : "");
  }

  spdlog::info("Database memory usage: {:.2f} MiB",
               data["databaseMemoryMiB"].get<double>());
}

void Shell::killRoomCommand(StringList &list) {
  if (list.empty() || list[0].empty()) {
    spdlog::warn("Need room id to do this.");
    return;
  }
  int id = atoi(list[0].c_str());
  auto result = runOnMain([id] { return AdminService::killRoom(id); });
  if (!result.ok()) spdlog::info(result.errorMsg());
}

void Shell::checkLobbyCommand(StringList &) {
  auto result = runOnMain([] { return AdminService::checkLobby(); });
  if (!result.ok()) spdlog::warn(result.errorMsg());
}

static void sigintHandler(int) {
  rl_reset_line_state();
  rl_replace_line("", 0);
  rl_crlf();
  puts("(To exit, press Ctrl+D or type quit)");
  rl_forced_update_display();
}
static char **fk_completion(const char *text, int start, int end);
static char *null_completion(const char *, int) { return NULL; }

Shell::Shell() {
  // Setup readline here

  rl_catch_signals = 1;
  rl_catch_sigwinch = 1;
  rl_set_signals();
  signal(SIGINT, sigintHandler);

  rl_attempted_completion_function = fk_completion;
  rl_completion_entry_function = null_completion;

  static const std::unordered_map<std::string_view, void (Shell::*)(StringList &)> handlers = {
    {"help", &Shell::helpCommand},
    {"?", &Shell::helpCommand},
    {"lsplayer", &Shell::lspCommand},
    {"lsroom", &Shell::lsrCommand},
    {"install", &Shell::installCommand},
    {"remove", &Shell::removeCommand},
    {"upgrade", &Shell::upgradeCommand},
    {"u", &Shell::upgradeCommand},
    {"pkgs", &Shell::lspkgCommand},
    {"syncpkgs", &Shell::syncpkgCommand},
    {"enable", &Shell::enableCommand},
    {"disable", &Shell::disableCommand},
    {"kick", &Shell::kickCommand},
    {"msg", &Shell::msgCommand},
    {"m", &Shell::msgCommand},
    {"msgroom", &Shell::msgRoomCommand},
    {"mr", &Shell::msgRoomCommand},
    {"ban", &Shell::banCommand},
    {"unban", &Shell::unbanCommand},
    {"banip", &Shell::banipCommand},
    {"unbanip", &Shell::unbanipCommand},
    {"banuuid", &Shell::banUuidCommand},
    {"unbanuuid", &Shell::unbanUuidCommand},
    {"tempban", &Shell::tempbanCommand},
    {"tempmute", &Shell::tempmuteCommand},
    {"unmute", &Shell::unmuteCommand},
    {"whitelist", &Shell::whitelistCommand},
    {"reloadconf", &Shell::reloadConfCommand},
    {"r", &Shell::reloadConfCommand},
    {"resetpassword", &Shell::resetPasswordCommand},
    {"rp", &Shell::resetPasswordCommand},
    {"stat", &Shell::statCommand},
    {"gc", &Shell::statCommand},
    {"killroom", &Shell::killRoomCommand},
    {"checklobby", &Shell::checkLobbyCommand},
    // special command
    {"quit", &Shell::helpCommand},
    {"crash", &Shell::helpCommand},
  };
  handler_map = handlers;
}

void Shell::handleLine(char *bytes) {
  if (!bytes || !strncmp(bytes, "quit", 4)) {
    spdlog::info("Server is shutting down.");
    Server::instance().stop();
    done = true;
    free(bytes);
    return;
  }

  spdlog::info("Running command: '{}'", bytes);

  if (!strncmp(bytes, "crash", 5)) {
    spdlog::error("Crashing."); // should dump core
    free(bytes);
    std::exit(1);
    return;
  }

  add_history(bytes);

  auto command = std::string { bytes };
  std::istringstream iss(command);
  std::vector<std::string> command_list;

  for (std::string token; iss >> token;) {
    command_list.push_back(token);
  }
  if (command_list.size() == 0) return;

  auto it = handler_map.find(command_list[0]);
  if (it == handler_map.end()) {
    auto bytes = command_list[0];
    spdlog::warn("Unknown command '{}'. Type 'help' for hints.", bytes);
  } else {
    command_list.erase(command_list.begin());
    (this->*it->second)(command_list);
  }

  free(bytes);
}

void Shell::redisplay() {
  rl_clear_visible_line();
  rl_forced_update_display();
}

void Shell::moveCursorToStart() {
  winsize sz;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &sz);
  int lines = (rl_end + strlen(prompt) - 1) / sz.ws_col;
  printf("\e[%d;%dH", sz.ws_row - lines, 0);
}

void Shell::clearLine() {
  rl_clear_visible_line();
}

bool Shell::lineDone() const {
  return (bool)rl_done;
}

/*
// 最简单的语法高亮，若命令可执行就涂绿，否则涂红
QString Shell::syntaxHighlight(char *bytes) {
  QString ret(bytes);
  auto command = ret.split(' ').first();
  auto func = handler_map[command];
  auto colored_command = command;
  if (!func) {
    colored_command = Color(command, fkShell::Red, fkShell::Bold);
  } else {
    colored_command = Color(command, fkShell::Green);
  }
  ret.replace(0, command.length(), colored_command);
  return ret;
}
*/

char *Shell::generateCommand(const char *text, int state) {
  static size_t list_index, len;
  static std::vector<std::string_view> keys;
  static std::once_flag flag;
  std::call_once(flag, [&] {
    for (const auto &[k, _] : handler_map) {
      keys.push_back(k);
    }
  });
  const char *name;

  if (state == 0) {
    list_index = 0;
    len = strlen(text);
  }

  while (list_index < keys.size()) {
    name = keys[list_index].data();
    ++list_index;
    if (strncmp(name, text, len) == 0) {
      return strdup(name);
    }
  }

  return NULL;
}

static char *command_generator(const char *text, int state) {
  return Server::instance().shell().generateCommand(text, state);
}

static char *repo_generator(const char *text, int state) {
  static constexpr const char *recommend_repos[] = {
    "https://gitee.com/Qsgs-Fans/standard_ex",
    "https://gitee.com/Qsgs-Fans/shzl",
    "https://gitee.com/Qsgs-Fans/sp",
    "https://gitee.com/Qsgs-Fans/yj",
    "https://gitee.com/Qsgs-Fans/ol",
    "https://gitee.com/Qsgs-Fans/mougong",
    "https://gitee.com/Qsgs-Fans/mobile",
    "https://gitee.com/Qsgs-Fans/tenyear",
    "https://gitee.com/Qsgs-Fans/overseas",
    "https://gitee.com/Qsgs-Fans/jsrg",
    "https://gitee.com/Qsgs-Fans/qsgs",
    "https://gitee.com/Qsgs-Fans/mini",
    "https://gitee.com/Qsgs-Fans/gamemode",
    "https://gitee.com/Qsgs-Fans/utility",
    "https://gitee.com/Qsgs-Fans/freekill-core",
    "https://gitee.com/Qsgs-Fans/offline",
    "https://gitee.com/Qsgs-Fans/hegemony",
    "https://gitee.com/Qsgs-Fans/lunar",
  };
  static size_t list_index, len;
  const char *name;

  if (state == 0) {
    list_index = 0;
    len = strlen(text);
  }

  while (list_index < std::size(recommend_repos)) {
    name = recommend_repos[list_index];
    ++list_index;
    if (strncmp(name, text, len) == 0) {
      return strdup(name);
    }
  }

  return NULL;
}

static char *package_generator(const char *text, int state) {
  static Sqlite3::QueryResult arr;
  static size_t list_index, len;
  const char *name;

  if (state == 0) {
    arr = PackMan::instance().listPackages();
    list_index = 0;
    len = strlen(text);
  }

  while (list_index < arr.size()) {
    name = arr[list_index].at("name").c_str();
    ++list_index;
    if (strncmp(name, text, len) == 0) {
      return strdup(name);
    }
  }

  return NULL;
}

static char *online_user_generator(const char *text, int state) {
  static std::vector<std::string> arr;
  static size_t list_index, len;
  const char *name;

  if (state == 0) {
    arr.clear();
    for (const auto &[_, p] : Server::instance().user_manager().getPlayers()) {
      arr.push_back(p->getScreenName());
    }
    list_index = 0;
    len = strlen(text);
  }

  while (list_index < arr.size()) {
    name = arr[list_index].c_str();
    ++list_index;
    if (strncmp(name, text, len) == 0) {
      return strdup(name);
    }
  }

  return NULL;
};


static char *user_generator(const char *text, int state) {
  // TODO: userinfo表需要一个cache机制
  static Sqlite3::QueryResult arr;
  static size_t list_index, len;
  const char *name;

  if (state == 0) {
    arr = Server::instance().database().select("SELECT name FROM userinfo;");
    list_index = 0;
    len = strlen(text);
  }

  while (list_index < arr.size()) {
    name = arr[list_index]["name"].c_str();
    ++list_index;
    if (strncmp(name, text, len) == 0) {
      return strdup(name);
    }
  }

  return NULL;
};

static char *banned_user_generator(const char *text, int state) {
  // TODO: userinfo表需要一个cache机制
  static Sqlite3::QueryResult arr;
  static size_t list_index, len;
  const char *name;
  auto &db = Server::instance().database();

  if (state == 0) {
    arr = db.select("SELECT name FROM userinfo WHERE banned = 1;");
    list_index = 0;
    len = strlen(text);
  }

  while (list_index < arr.size()) {
    name = arr[list_index]["name"].c_str();
    ++list_index;
    if (strncmp(name, text, len) == 0) {
      return strdup(name);
    }
  }

  return NULL;
};

static char **fk_completion(const char* text, int start, int end) {
  char **matches = NULL;
  if (start == 0) {
    matches = rl_completion_matches(text, command_generator);
  } else {
    auto str = std::string { rl_line_buffer };
    std::istringstream iss(str);
    std::vector<std::string> command_list;

    for (std::string token; iss >> token;) {
      command_list.push_back(token);
    }

    if (command_list.size() > 2) return NULL;
    auto command = command_list[0];
    if (command == "install") {
      matches = rl_completion_matches(text, repo_generator);
    } else if (command == "remove" || command == "upgrade" || command == "u"
        || command == "enable" || command == "disable") {
      matches = rl_completion_matches(text, package_generator);
    } else if (command.starts_with("ban") || command == "tempban"
        || command == "resetpassword" || command == "rp") {
      matches = rl_completion_matches(text, user_generator);
    } else if (command.starts_with("unban")) {
      matches = rl_completion_matches(text, banned_user_generator);
    } else if (command.starts_with("kick")) {
      matches = rl_completion_matches(text, online_user_generator);
    }
  }

  return matches;
}

void Shell::run() {
  pthread_setname_np(pthread_self(), "Shell");

  printf("\rfreekill-asio, Copyright (C) 2025, GNU GPL'd, by Notify et al.\n");
  printf("This program comes with ABSOLUTELY NO WARRANTY.\n");
  printf(
      "This is free software, and you are welcome to redistribute it under\n");
  printf("certain conditions; For more information visit "
         "http://www.gnu.org/licenses.\n\n");

  printf("[freekill-asio v%s] Welcome to CLI. Enter 'help' for usage hints.\n", FK_VERSION);

  while (true) {
    char *bytes = readline(prompt);
    handleLine(bytes);
    if (done) break;
  }
}
