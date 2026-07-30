// SPDX-License-Identifier: GPL-3.0-or-later

#include <git2.h>
#include <git2/errors.h>
#include <algorithm>
#include <string>
#include "core/packman.h"
#include "core/c-wrapper.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

static std::unique_ptr<PackMan> pacman_instance = nullptr;

PackMan &PackMan::instance() {
  if (!pacman_instance) {
    pacman_instance = std::unique_ptr<PackMan>(new PackMan);

#ifdef FK_EMBEDDED
    // 静态编译版中，需要手动打包certs文件，过老的系统里面的证书不太可靠
    git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, NULL, "./certs");
#endif
  }

  return *pacman_instance;
}

void PackMan::destroy() {
  // spdlog::debug("[MEMORY] pacman_instance destructed");
  pacman_instance = nullptr;
}

PackMan::PackMan() {
  git_libgit2_init();
  db = std::make_unique<Sqlite3>("./packages/packages.db", "./packages/init.sql");

  for (auto &obj : db->select("SELECT name, enabled FROM packages;")) {
    auto pack = obj["name"];
    auto enabled = obj["enabled"] == "1";

    if (!enabled) {
      disabled_packs.push_back(pack);
    }
  }
}

PackMan::~PackMan() {
  git_libgit2_shutdown();
}

std::vector<std::string> &PackMan::getDisabledPacks() {
  return disabled_packs;
}

const std::string &PackMan::summary() const {
  return m_summary;
}

void PackMan::refreshSummary() {
  auto data = db->select("SELECT name, url, hash FROM packages WHERE enabled = 1;");

  json arr = json::array();
  for (const auto &mp : data) {
    arr.push_back({
      { "name", mp.at("name") },
      { "hash", mp.at("hash") },
      { "url", mp.at("url") },
    });
  }

  auto bin = json::to_cbor(arr);
  m_summary = std::string(bin.begin(), bin.end());
}

// RAII wrapper for libgit2 git_repository
struct PackMan::GitRepo {
  git_repository *repo = NULL;

  GitRepo() noexcept = default;
  GitRepo(GitRepo &) = delete;
  GitRepo(GitRepo &&) = delete;
  ~GitRepo() { if (repo) git_repository_free(repo); }
};


/*
void PackMan::loadSummary(const QString &jsonData, bool useThread) {
  // First, disable all packages
  for (auto e : db->select("SELECT name FROM packages;")) {
    disablePack(e["name"]);
  }

  // Then read conf from string
  auto doc = QJsonDocument::fromJson(jsonData.toUtf8());
  auto arr = doc.array();
  for (auto e : arr) {
    auto obj = e.toObject();
    auto name = obj["name"].toString();
    auto url = obj["url"].toString();
    int err = 0;

    if (db->select(
      QString("SELECT name FROM packages WHERE name='{}';").arg(name))
      .isEmpty()) {
      err = downloadNewPack(url);
      if (err != 0) {
        continue;
      }
    }

    enablePack(name);

    if (head(name) != obj["hash"].toString()) {
      err = updatePack(name, obj["hash"].toString());
      if (err != 0) {
        continue;
      }
    }

    db->exec(QString("UPDATE packages SET hash='{}' WHERE name='{}'")
             .arg(obj["hash"].toString())
             .arg(name));
  }
}
*/

int PackMan::downloadNewPack(const char *u) {
  static constexpr const char *sql_select = "SELECT name FROM packages \
    WHERE name = '{}';";
  static constexpr const char *sql_update = "INSERT INTO packages (name,url,hash,enabled) \
    VALUES ('{}','{}','{}',1);";

  GitRepo raii_repo;
  int err = clone(u, raii_repo);
  if (err < 0)
    return err;

  auto repo = raii_repo.repo;

  auto url = std::string { u };
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  auto fileName = url.substr(url.find_last_of('/') + 1);
  if (fileName.size() > 4 && fileName.substr(fileName.size() - 4) == ".git") {
    fileName = fileName.substr(0, fileName.size() - 4);
  }

  auto result = db->select(fmt::format(sql_select, fileName));
  if (result.empty()) {
    db->exec(fmt::format(sql_update, fileName, url, head(repo)));
  }

  return err;
}

void PackMan::enablePack(const char *pack) {
  db->exec(
      fmt::format("UPDATE packages SET enabled = 1 WHERE name = '{}';", pack));

  auto it = std::remove(disabled_packs.begin(), disabled_packs.end(), pack);
  if (it != disabled_packs.end()) {
    disabled_packs.erase(it, disabled_packs.end());
  }
}

void PackMan::disablePack(const char *pack) {
  if (strcmp(pack, "freekill-core") == 0) {
    spdlog::warn("Package 'freekill-core' cannot be disabled.");
    return;
  }

  db->exec(
    fmt::format("UPDATE packages SET enabled = 0 WHERE name = '{}';", pack));

  auto it = std::ranges::find(disabled_packs, pack);
  if (it == disabled_packs.end())
    disabled_packs.push_back(pack);
}

int PackMan::updatePack(const char *pack, const char *hash) {
  GitRepo raii_repo;
  int err = open(pack, raii_repo);
  if (err < 0)
    return err;
  auto repo = raii_repo.repo;

  // 先status 检查dirty 后面全是带--force的操作
  err = status(repo);
  if (err != 0)
    return err;
  err = pull(repo);
  if (err < 0)
    return err;
  err = checkout(repo, hash);
  if (err < 0)
    return err;
  return 0;
}

int PackMan::upgradePack(const char *pack) {
  GitRepo raii_repo;
  int err = open(pack, raii_repo);
  if (err < 0)
    return err;
  auto repo = raii_repo.repo;

  // 先status 检查dirty 后面全是带--force的操作
  err = status(repo);
  if (err != 0)
    return err;

  // 记录当前commit hash
  auto old_hash = head(repo);

  err = pull(repo);
  if (err < 0)
    return err;

  // 至此upgrade命令把包升到了FETCH_HEAD的commit
  // 我们稍微操作一下，让HEAD指向最新的master
  // 这样以后就能开新分支干活了
  err = checkout_branch(repo, "master");
  if (err < 0)
    return err;

  auto hash = head(repo);
  auto commit_range = fmt::format("{}..{}", old_hash, hash);
  auto changelog = generate_changelog(repo, commit_range.c_str());
  if (!changelog.empty()) {
    spdlog::info("New changes of package '{}':\n{}", pack, changelog);
  }

  db->exec(fmt::format("UPDATE packages SET hash = '{}' WHERE name = '{}';",
                  head(repo), pack));
  return 0;
}

void PackMan::removePack(const char *pack) {
  auto result = db->select(fmt::format("SELECT enabled FROM packages \
    WHERE name = '{}';", pack));
  if (result.empty())
    return;

  db->exec(fmt::format("DELETE FROM packages WHERE name = '{}';", pack));

  std::error_code ec;
  std::filesystem::remove_all(fmt::format("packages/{}", pack), ec);
  if (ec) {
    spdlog::error("Failed to remove directory: {}", ec.message());
  }
}

Sqlite3::QueryResult PackMan::listPackages() {
  return db->select("SELECT * FROM packages;");
}

void PackMan::forceCheckoutMaster(const char *pack) {
  GitRepo repo;
  int err = open(pack, repo);
  if (err < 0)
    return;
  checkout_branch(repo.repo, "master");
}

void PackMan::syncCommitHashToDatabase() {
  for (auto e : db->select("SELECT name FROM packages;")) {
    auto pack = e["name"];
    GitRepo repo;
    int err = open(pack.c_str(), repo);
    if (err < 0)
      continue;
    db->exec(fmt::format("UPDATE packages SET hash = '{}' WHERE name = '{}';",
             head(repo.repo), pack));
  }
}

#define GIT_FAIL                                                               \
  const git_error *e = git_error_last();                                       \
  spdlog::error("Error {}/{}: {}", err, e->klass, e->message)

#define GIT_CHK_CLEAN  \
  if (err < 0) {     \
    GIT_FAIL;          \
    goto clean;        \
  }

int PackMan::open(const char *name, GitRepo &repo) {
  git_repository *raw = NULL;
  auto path = fmt::format("packages/{}", name);
  int err = git_repository_open(&raw, path.c_str());
  if (err < 0) {
    GIT_FAIL;
    return err;
  }
  repo.repo = raw;
  return 0;
}

static int transfer_progress_cb(const git_indexer_progress *stats,
                                void *payload) {
  if (stats->received_objects == stats->total_objects) {
    printf("Resolving deltas %u/%u\r", stats->indexed_deltas,
           stats->total_deltas);
  } else if (stats->total_objects > 0) {
    printf("Received %u/%u objects (%u) in %zu bytes\r",
           stats->received_objects, stats->total_objects,
           stats->indexed_objects, stats->received_bytes);
  }

  return 0;
}

int PackMan::clone(const char *u, GitRepo &repo) {
  auto url = std::string { u };
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  auto fileName = url.substr(url.find_last_of('/') + 1);
  if (fileName.size() > 4 && fileName.substr(fileName.size() - 4) == ".git") {
    fileName = fileName.substr(0, fileName.size() - 4);
  }
  auto clonePath = std::filesystem::path("packages") / fileName;

  git_repository *raw = NULL;
  git_clone_options opt;
  git_clone_init_options(&opt, GIT_CLONE_OPTIONS_VERSION);
  opt.fetch_opts.proxy_opts.version = 1;
  opt.fetch_opts.callbacks.transfer_progress = transfer_progress_cb;
  int err = git_clone(&raw, url.c_str(), clonePath.string().c_str(), &opt);
  if (err < 0) {
    std::error_code ec;
    std::filesystem::remove_all(clonePath, ec);
    if (ec) {
      spdlog::error("Failed to remove directory: {}", ec.message());
    }
    GIT_FAIL;
  } else {
    repo.repo = raw;
    printf("\n");
  }

  return err;
}

// git fetch && git checkout FETCH_HEAD -f
int PackMan::pull(git_repository *repo) {
  int err;
  git_remote *remote = NULL;
  git_fetch_options opt;
  git_fetch_init_options(&opt, GIT_FETCH_OPTIONS_VERSION);
  opt.proxy_opts.version = 1;
  opt.callbacks.transfer_progress = transfer_progress_cb;

  git_checkout_options opt2 = GIT_CHECKOUT_OPTIONS_INIT;
  opt2.checkout_strategy = GIT_CHECKOUT_FORCE;

  // first git fetch origin
  err = git_remote_lookup(&remote, repo, "origin");
  GIT_CHK_CLEAN;

  err = git_remote_fetch(remote, NULL, &opt, NULL);
  GIT_CHK_CLEAN;

  // then git checkout FETCH_HEAD
  err = git_repository_set_head(repo, "FETCH_HEAD");
  GIT_CHK_CLEAN;

  err = git_checkout_head(repo, &opt2);
  GIT_CHK_CLEAN;

  printf("\n");

clean:
  git_remote_free(remote);
  return err;
}

int PackMan::checkout(git_repository *repo, const char *hash) {
  int err;
  git_oid oid = {0};
  git_checkout_options opt = GIT_CHECKOUT_OPTIONS_INIT;
  opt.checkout_strategy = GIT_CHECKOUT_FORCE;
  err = git_oid_fromstr(&oid, hash);
  GIT_CHK_CLEAN;
  err = git_repository_set_head_detached(repo, &oid);
  GIT_CHK_CLEAN;
  err = git_checkout_head(repo, &opt);
  GIT_CHK_CLEAN;

clean:
  return err;
}

// git checkout -B branch origin/branch --force
int PackMan::checkout_branch(git_repository *repo, const char *branch) {
  git_oid oid = {0};
  int err;
  git_object *obj = NULL;
  git_reference *branch_ref = NULL;
  git_reference *remote_ref = NULL;
  git_reference *new_branch_ref = NULL;
  git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
  checkout_opts.checkout_strategy = GIT_CHECKOUT_FORCE;

  std::string local_branch;
  std::string remote_branch;

  // 查找远程分支的引用 (refs/remotes/origin/branch)
  remote_branch = fmt::format("refs/remotes/origin/{}", branch);
  err = git_reference_lookup(&remote_ref, repo, remote_branch.c_str());
  GIT_CHK_CLEAN;

  // 获取远程分支指向的对象
  err = git_reference_peel(&obj, remote_ref, GIT_OBJECT_COMMIT);
  GIT_CHK_CLEAN;

  // 获取commit的OID
  git_oid_cpy(&oid, git_object_id(obj));

   // 查找本地分支的引用
  local_branch = fmt::format("refs/heads/{}", branch);
  err = git_reference_lookup(&branch_ref, repo, local_branch.c_str());
  if (err == 0) {
    // 分支存在，强制重置
    err = git_reference_set_target(&new_branch_ref, branch_ref, &oid, "reset: moving to remote branch");
    GIT_CHK_CLEAN;
  } else {
    // 分支不存在，创建新分支
    err = git_branch_create(&new_branch_ref, repo, branch,
        (git_commit*)obj, 0);
    GIT_CHK_CLEAN;
  }

  // 设HEAD到分支
  err = git_repository_set_head(repo, git_reference_name(new_branch_ref));
  GIT_CHK_CLEAN;

  // 强制检出到HEAD
  err = git_checkout_head(repo, &checkout_opts);
  GIT_CHK_CLEAN;

clean:
  git_reference_free(new_branch_ref);
  git_reference_free(branch_ref);
  git_reference_free(remote_ref);
  git_object_free(obj);

  return err;
}

int PackMan::status(git_repository *repo) {
  int err;
  git_status_list *status_list = NULL;
  size_t i, maxi;
  const git_status_entry *s;
  err = git_status_list_new(&status_list, repo, NULL);
  GIT_CHK_CLEAN;
  maxi = git_status_list_entrycount(status_list);
  for (i = 0; i < maxi; ++i) {
    // char *istatus = NULL;
    s = git_status_byindex(status_list, i);
    if (s->status != GIT_STATUS_CURRENT && s->status != GIT_STATUS_IGNORED) {
      git_status_list_free(status_list);
      spdlog::error("Workspace is dirty.");
      return 100;
    }
  }

clean:
  git_status_list_free(status_list);
  return err;
}

std::string PackMan::head(git_repository *repo) {
  int err;
  git_object *obj = NULL;
  const git_oid *oid;
  char buf[42] = {0};
  err = git_revparse_single(&obj, repo, "HEAD");
  GIT_CHK_CLEAN;

  oid = git_object_id(obj);
  git_oid_tostr(buf, 41, oid);
  git_object_free(obj);
  return std::string { buf };

clean:
  git_object_free(obj);
  return "0000000000000000000000000000000000000000";
}

struct ConvCommit {
  std::string raw_message_head;
  enum { Feat, Fix } type;
  bool breaking = false;
  std::string subtype;
  std::string message_title;
};

// 根据Conventional Commit规范，对于给定commit_range生成摘要
// 但是实际上是简化版方法，只检测feat: 和 fix:，以及他俩携带括号的版本和携带感叹号的版本
// 比如feat(foo)!: message title\n\n  LONG MESSAGE BODY
// 这单独一条显示为 - **破坏性!** (foo) message title
std::string PackMan::generate_changelog(git_repository *repo, const char *commit_range) {
  int err;
  git_revwalk *walk = NULL;
  git_oid oid;
  std::vector<ConvCommit> feats;
  std::vector<ConvCommit> fixes;
  std::string result;

  err = git_revwalk_new(&walk, repo);
  GIT_CHK_CLEAN;

  err = git_revwalk_push_range(walk, commit_range);
  GIT_CHK_CLEAN;

  while (!git_revwalk_next(&oid, walk)) {
    git_commit *commit = NULL;
    err = git_commit_lookup(&commit, repo, &oid);
    if (err < 0) continue;

    const char *msg = git_commit_message(commit);
    std::string msg_str(msg);
    auto first_line = msg_str.substr(0, msg_str.find('\n'));

    ConvCommit cc;
    cc.raw_message_head = first_line;

    size_t pos = 0;
    if (first_line.starts_with("feat")) {
      cc.type = ConvCommit::Feat;
      pos = 4;
    } else if (first_line.starts_with("fix")) {
      cc.type = ConvCommit::Fix;
      pos = 3;
    } else {
      git_commit_free(commit);
      continue;
    }

    if (pos < first_line.size() && first_line[pos] == '(') {
      size_t end = first_line.find(')', pos);
      if (end != std::string::npos) {
        cc.subtype = first_line.substr(pos + 1, end - pos - 1);
        pos = end + 1;
      }
    }

    if (pos < first_line.size() && first_line[pos] == '!') {
      cc.breaking = true;
      pos++;
    }

    if (pos < first_line.size() && first_line[pos] == ':') {
      pos++;
      if (pos < first_line.size() && first_line[pos] == ' ') {
        pos++;
      }
    }

    cc.message_title = first_line.substr(pos);

    if (cc.type == ConvCommit::Feat) {
      feats.push_back(cc);
    } else {
      fixes.push_back(cc);
    }

    git_commit_free(commit);
  }

  std::reverse(feats.begin(), feats.end());
  std::reverse(fixes.begin(), fixes.end());

  if (!feats.empty()) {
    result += "## Feature(s)\n\n";
    for (const auto &c : feats) {
      result += "- ";
      if (c.breaking) {
        result += "**BREAKING!** ";
      }
      if (!c.subtype.empty()) {
        result += "(" + c.subtype + ") ";
      }
      result += c.message_title + "\n";
    }
    result += "\n";
  }

  if (!fixes.empty()) {
    result += "## Fix(es)\n\n";
    for (const auto &c : fixes) {
      result += "- ";
      if (c.breaking) {
        result += "**BREAKING!** ";
      }
      if (!c.subtype.empty()) {
        result += "(" + c.subtype + ") ";
      }
      result += c.message_title + "\n";
    }
    result += "\n";
  }

clean:
  git_revwalk_free(walk);
  return result;
}

#undef GIT_FAIL
#undef GIT_CHK_CLEAN
