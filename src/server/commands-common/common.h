
#ifndef _COMMON_H
#define _COMMON_H

#include "server/server.h"
#include "server/user/serverplayer.h"
#include "server/user/user_manager.h"

struct ListPlayersResponse {
  const std::unordered_map<int, std::shared_ptr<ServerPlayer>>& players;
};

inline ListPlayersResponse commandListPlayers() {
  auto& user_manager = Server::instance().user_manager();
  auto& players = user_manager.getPlayers();
  return {players};
}

#endif  // _COMMON_H