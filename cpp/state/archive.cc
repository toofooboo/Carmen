#include "state/archive.h"

#include <algorithm>
#include <queue>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "backend/common/sqlite/sqlite.h"
#include "common/hash.h"
#include "common/memory_usage.h"
#include "common/status_util.h"
#include "common/type.h"

namespace carmen {

using ::carmen::backend::Sqlite;
using ::carmen::backend::SqlRow;
using ::carmen::backend::SqlStatement;

namespace internal {

class Archive {
 public:
  // Opens an archive database stored in the given file.
  static absl::StatusOr<std::unique_ptr<Archive>> Open(
      std::filesystem::path file) {
    ASSIGN_OR_RETURN(auto db, Sqlite::Open(file));

    // TODO: check whether there is already some data in the proper format.

    // Create tables.
    RETURN_IF_ERROR(db.Run(kCreateBlockTable));
    RETURN_IF_ERROR(db.Run(kCreateAccountHashTable));
    RETURN_IF_ERROR(db.Run(kCreateStatusTable));
    RETURN_IF_ERROR(db.Run(kCreateBalanceTable));
    RETURN_IF_ERROR(db.Run(kCreateCodeTable));
    RETURN_IF_ERROR(db.Run(kCreateNonceTable));
    RETURN_IF_ERROR(db.Run(kCreateValueTable));

    // Prepare query statements.
    ASSIGN_OR_RETURN(auto add_block, db.Prepare(kAddBlockStmt));
    ASSIGN_OR_RETURN(auto get_block_height, db.Prepare(kGetBlockHeightStmt));

    ASSIGN_OR_RETURN(auto add_account_hash, db.Prepare(kAddAccountHashStmt));
    ASSIGN_OR_RETURN(auto get_account_hash, db.Prepare(kGetAccountHashStmt));

    ASSIGN_OR_RETURN(auto create_account, db.Prepare(kCreateAccountStmt));
    ASSIGN_OR_RETURN(auto delete_account, db.Prepare(kDeleteAccountStmt));
    ASSIGN_OR_RETURN(auto get_status, db.Prepare(kGetStatusStmt));

    ASSIGN_OR_RETURN(auto add_balance, db.Prepare(kAddBalanceStmt));
    ASSIGN_OR_RETURN(auto get_balance, db.Prepare(kGetBalanceStmt));

    ASSIGN_OR_RETURN(auto add_code, db.Prepare(kAddCodeStmt));
    ASSIGN_OR_RETURN(auto get_code, db.Prepare(kGetCodeStmt));

    ASSIGN_OR_RETURN(auto add_nonce, db.Prepare(kAddNonceStmt));
    ASSIGN_OR_RETURN(auto get_nonce, db.Prepare(kGetNonceStmt));

    ASSIGN_OR_RETURN(auto add_value, db.Prepare(kAddValueStmt));
    ASSIGN_OR_RETURN(auto get_value, db.Prepare(kGetValueStmt));

    auto wrap = [](SqlStatement stmt) -> std::unique_ptr<SqlStatement> {
      return std::make_unique<SqlStatement>(std::move(stmt));
    };

    return std::unique_ptr<Archive>(new Archive(
        std::move(db), wrap(std::move(add_block)),
        wrap(std::move(get_block_height)), wrap(std::move(add_account_hash)),
        wrap(std::move(get_account_hash)), wrap(std::move(create_account)),
        wrap(std::move(delete_account)), wrap(std::move(get_status)),
        wrap(std::move(add_balance)), wrap(std::move(get_balance)),
        wrap(std::move(add_code)), wrap(std::move(get_code)),
        wrap(std::move(add_nonce)), wrap(std::move(get_nonce)),
        wrap(std::move(add_value)), wrap(std::move(get_value))));
  }

  // Adds the block update for the given block.
  absl::Status Add(BlockId block, const Update& update) {
    // Check that new block is newer than anything before.
    ASSIGN_OR_RETURN(std::int64_t newestBlock, GetLastBlockHeight());
    if (newestBlock >= 0 && BlockId(newestBlock) >= block) {
      return absl::FailedPreconditionError(absl::StrFormat(
          "Unable to insert block %d, archive already contains block %d", block,
          newestBlock));
    }

    // Compute hashes of account updates.
    absl::flat_hash_map<Address, Hash> diff_hashes;
    for (const auto& [addr, diff] : AccountUpdate::From(update)) {
      diff_hashes[addr] = diff.GetHash();
    }

    // Fill in data in a single transaction.
    auto guard = absl::MutexLock(&mutation_lock_);
    if (!add_value_stmt_) return absl::FailedPreconditionError("DB Closed");
    RETURN_IF_ERROR(db_.Run("BEGIN TRANSACTION"));

    RETURN_IF_ERROR(add_block_stmt_->Reset());
    RETURN_IF_ERROR(add_block_stmt_->Bind(0, std::int64_t(block)));
    RETURN_IF_ERROR(add_block_stmt_->Run());

    for (auto& addr : update.GetDeletedAccounts()) {
      RETURN_IF_ERROR(delete_account_stmt_->Reset());
      RETURN_IF_ERROR(delete_account_stmt_->Bind(0, addr));
      RETURN_IF_ERROR(delete_account_stmt_->Bind(1, static_cast<int>(block)));
      RETURN_IF_ERROR(delete_account_stmt_->Bind(2, addr));
      RETURN_IF_ERROR(delete_account_stmt_->Run());
    }

    for (auto& addr : update.GetCreatedAccounts()) {
      RETURN_IF_ERROR(create_account_stmt_->Reset());
      RETURN_IF_ERROR(create_account_stmt_->Bind(0, addr));
      RETURN_IF_ERROR(create_account_stmt_->Bind(1, static_cast<int>(block)));
      RETURN_IF_ERROR(create_account_stmt_->Bind(2, addr));
      RETURN_IF_ERROR(create_account_stmt_->Run());
    }

    for (auto& [addr, balance] : update.GetBalances()) {
      RETURN_IF_ERROR(add_balance_stmt_->Reset());
      RETURN_IF_ERROR(add_balance_stmt_->Bind(0, addr));
      RETURN_IF_ERROR(add_balance_stmt_->Bind(1, static_cast<int>(block)));
      RETURN_IF_ERROR(add_balance_stmt_->Bind(2, balance));
      RETURN_IF_ERROR(add_balance_stmt_->Run());
    }

    for (auto& [addr, code] : update.GetCodes()) {
      RETURN_IF_ERROR(add_code_stmt_->Reset());
      RETURN_IF_ERROR(add_code_stmt_->Bind(0, addr));
      RETURN_IF_ERROR(add_code_stmt_->Bind(1, static_cast<int>(block)));
      RETURN_IF_ERROR(add_code_stmt_->Bind(2, code));
      RETURN_IF_ERROR(add_code_stmt_->Run());
    }

    for (auto& [addr, nonce] : update.GetNonces()) {
      RETURN_IF_ERROR(add_nonce_stmt_->Reset());
      RETURN_IF_ERROR(add_nonce_stmt_->Bind(0, addr));
      RETURN_IF_ERROR(add_nonce_stmt_->Bind(1, static_cast<int>(block)));
      RETURN_IF_ERROR(add_nonce_stmt_->Bind(2, nonce));
      RETURN_IF_ERROR(add_nonce_stmt_->Run());
    }

    for (auto& [addr, key, value] : update.GetStorage()) {
      RETURN_IF_ERROR(add_value_stmt_->Reset());
      RETURN_IF_ERROR(add_value_stmt_->Bind(0, addr));
      RETURN_IF_ERROR(add_value_stmt_->Bind(1, addr));
      RETURN_IF_ERROR(add_value_stmt_->Bind(2, static_cast<int>(block)));
      RETURN_IF_ERROR(add_value_stmt_->Bind(3, key));
      RETURN_IF_ERROR(add_value_stmt_->Bind(4, static_cast<int>(block)));
      RETURN_IF_ERROR(add_value_stmt_->Bind(5, value));
      RETURN_IF_ERROR(add_value_stmt_->Run());
    }

    for (auto& [addr, hash] : diff_hashes) {
      ASSIGN_OR_RETURN(auto last_hash, GetAccountHash(block, addr));
      RETURN_IF_ERROR(add_account_hash_stmt_->Reset());
      RETURN_IF_ERROR(add_account_hash_stmt_->Bind(0, addr));
      RETURN_IF_ERROR(add_account_hash_stmt_->Bind(1, static_cast<int>(block)));
      RETURN_IF_ERROR(
          add_account_hash_stmt_->Bind(2, GetSha256Hash(last_hash, hash)));
      RETURN_IF_ERROR(add_account_hash_stmt_->Run());
    }

    return db_.Run("END TRANSACTION");
  }

  // Gets the maximum block height insert so far, returns -1 if there is none.
  absl::StatusOr<std::int64_t> GetLastBlockHeight() {
    auto guard = absl::MutexLock(&get_block_height_lock_);
    if (!get_block_height_stmt_)
      return absl::FailedPreconditionError("DB Closed");
    RETURN_IF_ERROR(get_block_height_stmt_->Reset());
    std::int64_t result = -1;
    RETURN_IF_ERROR(get_block_height_stmt_->Run(
        [&](const SqlRow& row) { result = row.GetInt64(0); }));
    return result;
  }

  absl::StatusOr<bool> Exists(BlockId block, const Address& account) {
    auto guard = absl::MutexLock(&get_status_lock_);
    if (!get_status_stmt_) return absl::FailedPreconditionError("DB Closed");
    RETURN_IF_ERROR(get_status_stmt_->Reset());
    RETURN_IF_ERROR(get_status_stmt_->Bind(0, account));
    RETURN_IF_ERROR(get_status_stmt_->Bind(1, static_cast<int>(block)));

    // The query produces 0 or 1 results. If there is no result, returning false
    // is what is expected since this is the default account state.
    bool result = false;
    RETURN_IF_ERROR(get_status_stmt_->Run(
        [&](const SqlRow& row) { result = (row.GetInt(0) != 0); }));
    return result;
  }

  absl::StatusOr<Balance> GetBalance(BlockId block, const Address& account) {
    // TODO: once account states are tracked, make sure the account exists at
    // that block.
    auto guard = absl::MutexLock(&get_balance_lock_);
    if (!get_balance_stmt_) return absl::FailedPreconditionError("DB Closed");
    RETURN_IF_ERROR(get_balance_stmt_->Reset());
    RETURN_IF_ERROR(get_balance_stmt_->Bind(0, account));
    RETURN_IF_ERROR(get_balance_stmt_->Bind(1, static_cast<int>(block)));

    // The query produces 0 or 1 results. If there is no result, returning the
    // zero value is what is expected since this is the default balance.
    Balance result{};
    RETURN_IF_ERROR(get_balance_stmt_->Run(
        [&](const SqlRow& row) { result.SetBytes(row.GetBytes(0)); }));
    return result;
  }

  absl::StatusOr<Code> GetCode(BlockId block, const Address& account) {
    // TODO: once account states are tracked, make sure the account exists at
    // that block.
    auto guard = absl::MutexLock(&get_code_lock_);
    if (!get_code_stmt_) return absl::FailedPreconditionError("DB Closed");
    RETURN_IF_ERROR(get_code_stmt_->Reset());
    RETURN_IF_ERROR(get_code_stmt_->Bind(0, account));
    RETURN_IF_ERROR(get_code_stmt_->Bind(1, static_cast<int>(block)));

    // The query produces 0 or 1 results. If there is no result, returning the
    // zero value is what is expected since this is the default code.
    Code result{};
    RETURN_IF_ERROR(get_code_stmt_->Run(
        [&](const SqlRow& row) { result = Code(row.GetBytes(0)); }));
    return result;
  }

  absl::StatusOr<Nonce> GetNonce(BlockId block, const Address& account) {
    // TODO: once account states are tracked, make sure the account exists at
    // that block.
    auto guard = absl::MutexLock(&get_nonce_lock_);
    if (!get_nonce_stmt_) return absl::FailedPreconditionError("DB Closed");
    RETURN_IF_ERROR(get_nonce_stmt_->Reset());
    RETURN_IF_ERROR(get_nonce_stmt_->Bind(0, account));
    RETURN_IF_ERROR(get_nonce_stmt_->Bind(1, static_cast<int>(block)));

    // The query produces 0 or 1 results. If there is no result, returning the
    // zero value is what is expected since this is the default balance.
    Nonce result{};
    RETURN_IF_ERROR(get_nonce_stmt_->Run(
        [&](const SqlRow& row) { result.SetBytes(row.GetBytes(0)); }));
    return result;
  }

  // Fetches the value of a storage slot at the given block height. If the value
  // was not defined at this block (or any time before) a zero value is
  // returned.
  absl::StatusOr<Value> GetStorage(BlockId block, const Address& account,
                                   const Key& key) {
    // TODO: once account states are tracked, make sure the account exists at
    // that block.
    auto guard = absl::MutexLock(&get_value_lock_);
    if (!get_value_stmt_) return absl::FailedPreconditionError("DB Closed");
    RETURN_IF_ERROR(get_value_stmt_->Reset());
    RETURN_IF_ERROR(get_value_stmt_->Bind(0, account));
    RETURN_IF_ERROR(get_value_stmt_->Bind(1, account));
    RETURN_IF_ERROR(get_value_stmt_->Bind(2, static_cast<int>(block)));
    RETURN_IF_ERROR(get_value_stmt_->Bind(3, key));
    RETURN_IF_ERROR(get_value_stmt_->Bind(4, static_cast<int>(block)));

    // The query produces 0 or 1 results. If there is no result, returning the
    // zero value is what is expected since this is the default value of storage
    // slots.
    Value result{};
    RETURN_IF_ERROR(get_value_stmt_->Run(
        [&](const SqlRow& row) { result.SetBytes(row.GetBytes(0)); }));
    return result;
  }

  absl::StatusOr<Hash> GetHash(BlockId block) {
    Sha256Hasher hasher;
    ASSIGN_OR_RETURN(
        auto query,
        db_.Prepare(
            "SELECT hash FROM account_hash a INNER JOIN (SELECT account, "
            "MAX(block) as block FROM account_hash WHERE block <= ? GROUP BY "
            "account) b ON a.account = b.account AND a.block = b.block ORDER "
            "BY a.account"));
    RETURN_IF_ERROR(query.Bind(0, static_cast<int>(block)));
    RETURN_IF_ERROR(
        query.Run([&](const SqlRow& row) { hasher.Ingest(row.GetBytes(0)); }));
    return hasher.GetHash();
  }

  absl::StatusOr<std::vector<Address>> GetAccountList(BlockId block) {
    std::vector<Address> res;
    ASSIGN_OR_RETURN(auto query,
                     db_.Prepare("SELECT DISTINCT account FROM account_hash "
                                 "WHERE block <= ? ORDER BY account"));
    RETURN_IF_ERROR(query.Bind(0, static_cast<int>(block)));
    RETURN_IF_ERROR(query.Run([&](const SqlRow& row) {
      Address addr;
      addr.SetBytes(row.GetBytes(0));
      res.push_back(addr);
    }));
    return res;
  }

  // Fetches the hash of the given account on the given block height. The hash
  // of an account is initially zero. Subsequent updates create a hash chain
  // covering the previous state and the hash of applied diffs.
  absl::StatusOr<Hash> GetAccountHash(BlockId block, const Address& account) {
    auto guard = absl::MutexLock(&get_account_hash_lock_);
    if (!get_account_hash_stmt_)
      return absl::FailedPreconditionError("DB Closed");
    RETURN_IF_ERROR(get_account_hash_stmt_->Reset());
    RETURN_IF_ERROR(get_account_hash_stmt_->Bind(0, account));
    RETURN_IF_ERROR(get_account_hash_stmt_->Bind(1, static_cast<int>(block)));

    // The query produces 0 or 1 results. If there is no result, returning the
    // zero hash is expected, since it is the hash of a non-existing account.
    Hash result{};
    RETURN_IF_ERROR(get_account_hash_stmt_->Run(
        [&](const SqlRow& row) { result.SetBytes(row.GetBytes(0)); }));
    return result;
  }

  absl::Status Verify(BlockId block, const Hash& expected_hash) {
    // Start by checking the DB integrity.
    ASSIGN_OR_RETURN(auto integrity_check_stmt,
                     db_.Prepare("PRAGMA integrity_check"));
    std::vector<std::string> issues;
    RETURN_IF_ERROR(integrity_check_stmt.Run([&](const SqlRow& row) {
      auto msg = row.GetString(0);
      if (msg != "ok") {
        issues.emplace_back(msg);
      }
    }));
    if (!issues.empty()) {
      std::stringstream out;
      for (const auto& cur : issues) {
        out << "\t" << cur << "\n";
      }
      return absl::InternalError("Encountered DB integrity issues:\n" +
                                 out.str());
    }

    // Next, check the expected hash.
    ASSIGN_OR_RETURN(auto hash, GetHash(block));
    if (hash != expected_hash) {
      return absl::InternalError("Archive hash does not match expected hash.");
    }

    // Validate all individual accounts.
    // TODO: run this in parallel
    ASSIGN_OR_RETURN(auto accounts, GetAccountList(block));
    for (const auto& cur : accounts) {
      RETURN_IF_ERROR(VerifyAccount(block, cur));
    }

    // Check that there is no extra information in any of the content tables.
    // TODO: run this in parallel
    for (auto table : {"status", "balance", "nonce", "code", "storage"}) {
      ASSIGN_OR_RETURN(auto state_check,
                       db_.Prepare(absl::StrFormat(
                           "SELECT 1 FROM (SELECT account FROM %s WHERE block "
                           "<= ? EXCEPT SELECT account FROM account_hash WHERE "
                           "block <= ?) LIMIT 1",
                           table)));
      RETURN_IF_ERROR(state_check.Bind(0, static_cast<int>(block)));
      RETURN_IF_ERROR(state_check.Bind(1, static_cast<int>(block)));

      bool found = false;
      RETURN_IF_ERROR(state_check.Run([&](const auto&) { found = true; }));
      if (found) {
        return absl::InternalError(
            absl::StrFormat("Found extra row of data in table `%s`.", table));
      }
    }

    // All checks have been passed. DB is verified.
    return absl::OkStatus();
  }

  // Verifyies the consistency of the provides account up until the given block.
  absl::Status VerifyAccount(BlockId block, const Address& account) {
    using ::carmen::backend::SqlIterator;
    ASSIGN_OR_RETURN(auto list_diffs_stmt,
                     db_.Prepare("SELECT block, hash FROM account_hash WHERE "
                                 "account = ? AND block <= ? ORDER BY block"));

    RETURN_IF_ERROR(list_diffs_stmt.Bind(0, account));
    RETURN_IF_ERROR(list_diffs_stmt.Bind(1, static_cast<int>(block)));

    ASSIGN_OR_RETURN(auto list_state_stmt,
                     db_.Prepare("SELECT block, exist FROM status WHERE "
                                 "account = ? AND block <= ? ORDER BY block"));
    RETURN_IF_ERROR(list_state_stmt.Bind(0, account));
    RETURN_IF_ERROR(list_state_stmt.Bind(1, static_cast<int>(block)));

    ASSIGN_OR_RETURN(auto list_balance_stmt,
                     db_.Prepare("SELECT block, value FROM balance WHERE "
                                 "account = ? AND block <= ? ORDER BY block"));
    RETURN_IF_ERROR(list_balance_stmt.Bind(0, account));
    RETURN_IF_ERROR(list_balance_stmt.Bind(1, static_cast<int>(block)));

    ASSIGN_OR_RETURN(auto list_nonce_stmt,
                     db_.Prepare("SELECT block, value FROM nonce WHERE "
                                 "account = ? AND block <= ? ORDER BY block"));
    RETURN_IF_ERROR(list_nonce_stmt.Bind(0, account));
    RETURN_IF_ERROR(list_nonce_stmt.Bind(1, static_cast<int>(block)));

    ASSIGN_OR_RETURN(auto list_code_stmt,
                     db_.Prepare("SELECT block, code FROM code WHERE "
                                 "account = ? AND block <= ? ORDER BY block"));
    RETURN_IF_ERROR(list_code_stmt.Bind(0, account));
    RETURN_IF_ERROR(list_code_stmt.Bind(1, static_cast<int>(block)));

    ASSIGN_OR_RETURN(
        auto list_storage_stmt,
        db_.Prepare("SELECT block, slot, value FROM storage WHERE "
                    "account = ? AND block <= ? ORDER BY block, slot"));
    RETURN_IF_ERROR(list_storage_stmt.Bind(0, account));
    RETURN_IF_ERROR(list_storage_stmt.Bind(1, static_cast<int>(block)));

    // Open individual result iterators.
    ASSIGN_OR_RETURN(auto hash_iter, list_diffs_stmt.Open());
    ASSIGN_OR_RETURN(auto state_iter, list_state_stmt.Open());
    ASSIGN_OR_RETURN(auto balance_iter, list_balance_stmt.Open());
    ASSIGN_OR_RETURN(auto nonce_iter, list_nonce_stmt.Open());
    ASSIGN_OR_RETURN(auto code_iter, list_code_stmt.Open());
    ASSIGN_OR_RETURN(auto storage_iter, list_storage_stmt.Open());

    // Create and initializer priority queue over block numbers.
    BlockId next = block + 1;
    for (SqlIterator* iter :
         {&state_iter, &balance_iter, &nonce_iter, &code_iter, &storage_iter}) {
      ASSIGN_OR_RETURN(auto has_next, iter->Next());
      if (has_next) {
        next = std::min<BlockId>(next, (*iter)->GetInt64(0));
      }
    }

    Hash hash{};
    BlockId last = next - 1;
    while (next <= block) {
      BlockId current = next;
      if (current <= last) {
        // This should only be possible if primary key constraints are violated.
        return absl::InternalError(
            "Multiple updates for same information in same block found.");
      }

      // --- Recreate Update for Current Block ---
      AccountUpdate update;

      if (!state_iter.Finished() && state_iter->GetInt64(0) == current) {
        if (state_iter->GetInt(1) == 0) {
          update.deleted = true;
        } else {
          update.created = true;
        }
        RETURN_IF_ERROR(state_iter.Next());
      }

      if (!balance_iter.Finished() && balance_iter->GetInt64(0) == current) {
        Balance balance;
        balance.SetBytes(balance_iter->GetBytes(1));
        update.balance = balance;
        RETURN_IF_ERROR(balance_iter.Next());
      }

      if (!nonce_iter.Finished() && nonce_iter->GetInt64(0) == current) {
        Nonce nonce;
        nonce.SetBytes(nonce_iter->GetBytes(1));
        update.nonce = nonce;
        RETURN_IF_ERROR(nonce_iter.Next());
      }

      if (!code_iter.Finished() && code_iter->GetInt64(0) == current) {
        update.code = Code(code_iter->GetBytes(1));
        RETURN_IF_ERROR(code_iter.Next());
      }

      while (!storage_iter.Finished() && storage_iter->GetInt64(0) == current) {
        Key key;
        key.SetBytes(storage_iter->GetBytes(1));
        Value value;
        value.SetBytes(storage_iter->GetBytes(2));
        update.storage.push_back({key, value});
        RETURN_IF_ERROR(storage_iter.Next());
      }

      // --- Check that the current update matches the current block ---

      // Check the update against the list of per-account hashes.
      ASSIGN_OR_RETURN(auto has_next, hash_iter.Next());
      if (!has_next || hash_iter->GetInt64(0) != current) {
        return absl::InternalError(absl::StrFormat(
            "Archive contains update for block %d but no hash for it.",
            current));
      }

      // Compute the hash based on the diff.
      hash = GetSha256Hash(hash, update.GetHash());

      // Compare with hash stored in DB.
      Hash should;
      should.SetBytes(hash_iter->GetBytes(1));
      if (hash != should) {
        return absl::InternalError(
            absl::StrFormat("Hash for block %d does not match.", current));
      }

      // Find next block to be processed.
      next = block + 1;
      for (SqlIterator* iter : {&state_iter, &balance_iter, &nonce_iter,
                                &code_iter, &storage_iter}) {
        if (!iter->Finished()) {
          next = std::min<BlockId>(next, (*iter)->GetInt64(0));
        }
      }
    }

    // Check whether there are additional updates in the hash table.
    ASSIGN_OR_RETURN(auto has_more, hash_iter.Next());
    if (has_more) {
      return absl::InternalError(absl::StrFormat(
          "DB contains hash for update on block %d but no data.",
          hash_iter->GetInt64(0)));
    }

    return absl::OkStatus();
  }

  absl::Status Flush() {
    // Nothing to do.
    return absl::OkStatus();
  }

  // Closes this archive. After this, no more operations are allowed on it (not
  // checked).
  absl::Status Close() {
    // Before closing the DB all prepared statements need to be finalized.
    {
      auto guard = absl::MutexLock(&mutation_lock_);
      add_block_stmt_.reset();
      create_account_stmt_.reset();
      delete_account_stmt_.reset();
      add_balance_stmt_.reset();
      add_code_stmt_.reset();
      add_nonce_stmt_.reset();
      add_value_stmt_.reset();
      add_account_hash_stmt_.reset();
    }
    {
      auto guard = absl::MutexLock(&get_block_height_lock_);
      get_block_height_stmt_.reset();
    }
    {
      auto guard = absl::MutexLock(&get_account_hash_lock_);
      get_account_hash_stmt_.reset();
    }
    {
      auto guard = absl::MutexLock(&get_status_lock_);
      get_status_stmt_.reset();
    }
    {
      auto guard = absl::MutexLock(&get_balance_lock_);
      get_balance_stmt_.reset();
    }
    {
      auto guard = absl::MutexLock(&get_code_lock_);
      get_code_stmt_.reset();
    }
    {
      auto guard = absl::MutexLock(&get_nonce_lock_);
      get_nonce_stmt_.reset();
    }
    {
      auto guard = absl::MutexLock(&get_value_lock_);
      get_value_stmt_.reset();
    }
    return db_.Close();
  }

  MemoryFootprint GetMemoryFootprint() const {
    MemoryFootprint res(*this);
    res.Add("sqlite", db_.GetMemoryFootprint());
    return res;
  }

 private:
  // See reference: https://www.sqlite.org/lang.html

  // -- Blocks --

  static constexpr const std::string_view kCreateBlockTable =
      "CREATE TABLE IF NOT EXISTS block (number INT PRIMARY KEY)";

  static constexpr const std::string_view kAddBlockStmt =
      "INSERT INTO block(number) VALUES (?)";

  static constexpr const std::string_view kGetBlockHeightStmt =
      "SELECT number FROM block ORDER BY number DESC LIMIT 1";

  // -- Account Hashes --

  static constexpr const std::string_view kCreateAccountHashTable =
      "CREATE TABLE IF NOT EXISTS account_hash (account BLOB, block INT, hash "
      "BLOB, PRIMARY KEY(account,block))";

  static constexpr const std::string_view kAddAccountHashStmt =
      "INSERT INTO account_hash(account, block, hash) VALUES (?,?,?)";

  static constexpr const std::string_view kGetAccountHashStmt =
      "SELECT hash FROM account_hash WHERE account = ? AND block <= ? ORDER BY "
      "block DESC LIMIT 1";

  // -- Account Status --

  static constexpr const std::string_view kCreateStatusTable =
      "CREATE TABLE IF NOT EXISTS status (account BLOB, block INT, exist INT, "
      "reincarnation INT, PRIMARY KEY (account,block))";

  static constexpr const std::string_view kCreateAccountStmt =
      "INSERT INTO status(account,block,exist,reincarnation) VALUES "
      "(?,?,1,(SELECT IFNULL(MAX(reincarnation)+1,0) FROM status WHERE account "
      "= ?))";

  static constexpr const std::string_view kDeleteAccountStmt =
      "INSERT INTO status(account,block,exist,reincarnation) VALUES "
      "(?,?,0,(SELECT IFNULL(MAX(reincarnation)+1,0) FROM status WHERE account "
      "= ?))";

  static constexpr const std::string_view kGetStatusStmt =
      "SELECT exist FROM status WHERE account = ? AND block <= ? ORDER BY "
      "block DESC LIMIT 1";

  // -- Balance --

  static constexpr const std::string_view kCreateBalanceTable =
      "CREATE TABLE IF NOT EXISTS balance (account BLOB, block INT, value "
      "BLOB, PRIMARY KEY (account,block))";

  static constexpr const std::string_view kAddBalanceStmt =
      "INSERT INTO balance(account,block,value) VALUES (?,?,?)";

  static constexpr const std::string_view kGetBalanceStmt =
      "SELECT value FROM balance WHERE account = ? AND block <= ? "
      "ORDER BY block DESC LIMIT 1";

  // -- Code --

  static constexpr const std::string_view kCreateCodeTable =
      "CREATE TABLE IF NOT EXISTS code (account BLOB, block INT, code BLOB, "
      "PRIMARY KEY (account,block))";

  static constexpr const std::string_view kAddCodeStmt =
      "INSERT INTO code(account,block,code) VALUES (?,?,?)";

  static constexpr const std::string_view kGetCodeStmt =
      "SELECT code FROM code WHERE account = ? AND block <= ? "
      "ORDER BY block DESC LIMIT 1";

  // -- Nonces --

  static constexpr const std::string_view kCreateNonceTable =
      "CREATE TABLE IF NOT EXISTS nonce (account BLOB, block INT, value BLOB, "
      "PRIMARY KEY (account,block))";

  static constexpr const std::string_view kAddNonceStmt =
      "INSERT INTO nonce(account,block,value) VALUES (?,?,?)";

  static constexpr const std::string_view kGetNonceStmt =
      "SELECT value FROM nonce WHERE account = ? AND block <= ? "
      "ORDER BY block DESC LIMIT 1";

  // -- Storage --

  static constexpr const std::string_view kCreateValueTable =
      "CREATE TABLE IF NOT EXISTS storage (account BLOB, reincarnation INT, "
      "slot BLOB, block INT, value BLOB, PRIMARY KEY "
      "(account,reincarnation,slot,block))";

  static constexpr const std::string_view kAddValueStmt =
      "INSERT INTO storage(account,reincarnation,slot,block,value) VALUES "
      "(?,(SELECT IFNULL(MAX(reincarnation),0) FROM status WHERE account = ? "
      "AND block <= ?),?,?,?)";

  static constexpr const std::string_view kGetValueStmt =
      "SELECT value FROM storage WHERE account = ? AND reincarnation = (SELECT "
      "IFNULL(MAX(reincarnation),0) FROM status WHERE account = ? AND block <= "
      "?) AND slot = ? AND block <= ? ORDER BY block DESC LIMIT 1";

  Archive(Sqlite db, std::unique_ptr<SqlStatement> add_block,
          std::unique_ptr<SqlStatement> get_block_height,
          std::unique_ptr<SqlStatement> add_account_hash,
          std::unique_ptr<SqlStatement> get_account_hash,
          std::unique_ptr<SqlStatement> create_account,
          std::unique_ptr<SqlStatement> delete_account,
          std::unique_ptr<SqlStatement> get_status,
          std::unique_ptr<SqlStatement> add_balance,
          std::unique_ptr<SqlStatement> get_balance,
          std::unique_ptr<SqlStatement> add_code,
          std::unique_ptr<SqlStatement> get_code,
          std::unique_ptr<SqlStatement> add_nonce,
          std::unique_ptr<SqlStatement> get_nonce,
          std::unique_ptr<SqlStatement> add_value,
          std::unique_ptr<SqlStatement> get_value)
      : db_(std::move(db)),
        add_block_stmt_(std::move(add_block)),
        get_block_height_stmt_(std::move(get_block_height)),
        add_account_hash_stmt_(std::move(add_account_hash)),
        get_account_hash_stmt_(std::move(get_account_hash)),
        create_account_stmt_(std::move(create_account)),
        delete_account_stmt_(std::move(delete_account)),
        get_status_stmt_(std::move(get_status)),
        add_balance_stmt_(std::move(add_balance)),
        get_balance_stmt_(std::move(get_balance)),
        add_code_stmt_(std::move(add_code)),
        get_code_stmt_(std::move(get_code)),
        add_nonce_stmt_(std::move(add_nonce)),
        get_nonce_stmt_(std::move(get_nonce)),
        add_value_stmt_(std::move(add_value)),
        get_value_stmt_(std::move(get_value)) {}

  // The DB connection.
  Sqlite db_;

  // TODO: introduce pool of statements to support concurrent reads and writes.

  // Prepared statemetns for logging new data to the archive.
  absl::Mutex mutation_lock_;

  absl::Mutex get_block_height_lock_;
  std::unique_ptr<SqlStatement> add_block_stmt_ GUARDED_BY(mutation_lock_);
  std::unique_ptr<SqlStatement> get_block_height_stmt_
      GUARDED_BY(get_block_height_lock_);

  absl::Mutex get_account_hash_lock_;
  std::unique_ptr<SqlStatement> add_account_hash_stmt_
      GUARDED_BY(mutation_lock_);
  std::unique_ptr<SqlStatement> get_account_hash_stmt_
      GUARDED_BY(get_account_hash_lock_);

  absl::Mutex get_status_lock_;
  std::unique_ptr<SqlStatement> create_account_stmt_ GUARDED_BY(mutation_lock_);
  std::unique_ptr<SqlStatement> delete_account_stmt_ GUARDED_BY(mutation_lock_);
  std::unique_ptr<SqlStatement> get_status_stmt_ GUARDED_BY(get_status_lock_);

  absl::Mutex get_balance_lock_;
  std::unique_ptr<SqlStatement> add_balance_stmt_ GUARDED_BY(mutation_lock_);
  std::unique_ptr<SqlStatement> get_balance_stmt_ GUARDED_BY(get_balance_lock_);

  absl::Mutex get_code_lock_;
  std::unique_ptr<SqlStatement> add_code_stmt_ GUARDED_BY(mutation_lock_);
  std::unique_ptr<SqlStatement> get_code_stmt_ GUARDED_BY(get_code_lock_);

  absl::Mutex get_nonce_lock_;
  std::unique_ptr<SqlStatement> add_nonce_stmt_ GUARDED_BY(mutation_lock_);
  std::unique_ptr<SqlStatement> get_nonce_stmt_ GUARDED_BY(get_nonce_lock_);

  absl::Mutex get_value_lock_;
  std::unique_ptr<SqlStatement> add_value_stmt_ GUARDED_BY(mutation_lock_);
  std::unique_ptr<SqlStatement> get_value_stmt_ GUARDED_BY(get_value_lock_);
};

}  // namespace internal

Archive::Archive(std::unique_ptr<internal::Archive> impl)
    : impl_(std::move(impl)) {}

Archive::Archive(Archive&&) = default;

Archive::~Archive() { Close().IgnoreError(); }

Archive& Archive::operator=(Archive&&) = default;

absl::StatusOr<Archive> Archive::Open(std::filesystem::path directory) {
  // TODO: create directory if it does not exist.
  ASSIGN_OR_RETURN(auto impl,
                   internal::Archive::Open(directory / "archive.sqlite"));
  return Archive(std::move(impl));
}

absl::Status Archive::Add(BlockId block, const Update& update) {
  RETURN_IF_ERROR(CheckState());
  return impl_->Add(block, update);
}

absl::StatusOr<bool> Archive::Exists(BlockId block, const Address& account) {
  RETURN_IF_ERROR(CheckState());
  return impl_->Exists(block, account);
}

absl::StatusOr<Balance> Archive::GetBalance(BlockId block,
                                            const Address& account) {
  RETURN_IF_ERROR(CheckState());
  return impl_->GetBalance(block, account);
}

absl::StatusOr<Code> Archive::GetCode(BlockId block, const Address& account) {
  RETURN_IF_ERROR(CheckState());
  return impl_->GetCode(block, account);
}

absl::StatusOr<Nonce> Archive::GetNonce(BlockId block, const Address& account) {
  RETURN_IF_ERROR(CheckState());
  return impl_->GetNonce(block, account);
}

absl::StatusOr<Value> Archive::GetStorage(BlockId block, const Address& account,
                                          const Key& key) {
  RETURN_IF_ERROR(CheckState());
  return impl_->GetStorage(block, account, key);
}

absl::StatusOr<Hash> Archive::GetHash(BlockId block) {
  RETURN_IF_ERROR(CheckState());
  return impl_->GetHash(block);
}

absl::StatusOr<std::vector<Address>> Archive::GetAccountList(BlockId block) {
  RETURN_IF_ERROR(CheckState());
  return impl_->GetAccountList(block);
}

absl::StatusOr<Hash> Archive::GetAccountHash(BlockId block,
                                             const Address& account) {
  RETURN_IF_ERROR(CheckState());
  return impl_->GetAccountHash(block, account);
}

absl::Status Archive::Verify(BlockId block, const Hash& expected_hash) {
  RETURN_IF_ERROR(CheckState());
  return impl_->Verify(block, expected_hash);
}

absl::Status Archive::VerifyAccount(BlockId block,
                                    const Address& account) const {
  RETURN_IF_ERROR(CheckState());
  return impl_->VerifyAccount(block, account);
}

absl::Status Archive::Flush() {
  if (!impl_) return absl::OkStatus();
  return impl_->Flush();
}

absl::Status Archive::Close() {
  if (!impl_) return absl::OkStatus();
  auto result = impl_->Close();
  impl_ = nullptr;
  return result;
}

MemoryFootprint Archive::GetMemoryFootprint() const {
  MemoryFootprint res(*this);
  if (impl_) {
    res.Add("impl", impl_->GetMemoryFootprint());
  }
  return res;
}

absl::Status Archive::CheckState() const {
  if (impl_) return absl::OkStatus();
  return absl::FailedPreconditionError("Archive not connected to DB.");
}

}  // namespace carmen
