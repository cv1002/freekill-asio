// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "c-wrapper.h"

struct git_repository;

class PackMan {

public:
  static PackMan &instance();
  static void destroy();
  PackMan(PackMan &) = delete;
  PackMan(PackMan &&) = delete;
  ~PackMan();

  std::vector<std::string> &getDisabledPacks();
  const std::string &summary() const;
  void refreshSummary();
  /*
  // server用不到loadSummary，但还是先留着
  void loadSummary(const QString &, bool useThread = false);
  */
  int downloadNewPack(const char *url);
  void enablePack(const char *pack);
  void disablePack(const char *pack);
  int updatePack(const char *pack, const char *hash);
  int upgradePack(const char *pack);
  void removePack(const char *pack);
  Sqlite3::QueryResult listPackages();

  void forceCheckoutMaster(const char *pack);

  // 从数据库读取所有包。将repo的实际HEAD写入到db
  // 适用于自己手动git pull包后使用
  void syncCommitHashToDatabase();

private:
  PackMan();

  std::unique_ptr<Sqlite3> db;
  std::vector<std::string> disabled_packs;

  std::string m_summary;

  struct GitRepo;

  int open(const char *name, GitRepo &repo);
  int clone(const char *url, GitRepo &repo);
  int pull(git_repository *repo);
  int checkout(git_repository *repo, const char *hash);
  int checkout_branch(git_repository *repo, const char *branch);
  int status(git_repository *repo); // return 1 if the workdir is modified
  std::string head(git_repository *repo); // get commit hash of HEAD
  std::string generate_changelog(git_repository *repo, const char *commit_range);
};
