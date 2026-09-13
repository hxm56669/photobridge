#include "photobridge/lan/upload_session_store.h"

#include <chrono>
#include <cstddef>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <sqlite3.h>
#include <string>
#include <system_error>
#include <utility>

#include <blake3.h>

namespace photobridge {
namespace {

sqlite3* Database(void* value) noexcept
{
    return static_cast<sqlite3*>(value);
}

Status SqliteError(sqlite3* database, std::string operation)
{
    return Status(
        StatusCode::kIoError,
        std::move(operation) + ": " + sqlite3_errmsg(database));
}

Status Invalid(std::string message)
{
    return Status(StatusCode::kInvalidArgument, std::move(message));
}

std::string NewId(std::string_view prefix)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::random_device random;
    std::string id(prefix);
    id.push_back('-');
    for (int index = 0; index < 16; ++index) {
        id.push_back(digits[random() & 0x0fU]);
    }
    return id;
}

StatusOr<std::string> SafeFilename(std::string_view original)
{
    if (original.empty() || original == "." || original == "..") {
        return Invalid("filename must be a non-empty single component");
    }
    if (original.size() > 255) {
        return Invalid("filename exceeds 255 bytes");
    }

    for (const unsigned char byte : original) {
        if (byte == 0 || byte == '/' || byte == '\\' || byte < 0x20
            || byte == 0x7f) {
            return Invalid("filename contains an unsafe path component");
        }
    }
    return std::string(original);
}

std::string WithCollisionSuffix(std::string value, std::size_t suffix)
{
    if (suffix == 1) {
        return value;
    }
    const std::size_t extension = value.find_last_of('.');
    const std::string suffix_text = "__" + std::to_string(suffix);
    if (extension == std::string::npos || extension == 0) {
        return value + suffix_text;
    }
    return value.substr(0, extension) + suffix_text + value.substr(extension);
}

bool AddWouldOverflow(std::uint64_t left, std::uint64_t right) noexcept
{
    return right > std::numeric_limits<std::uint64_t>::max() - left;
}

Status Exec(sqlite3* database, const char* sql)
{
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (result == SQLITE_OK) {
        return Status::Ok();
    }
    std::string message = error == nullptr ? "unknown sqlite error" : error;
    sqlite3_free(error);
    return Status(StatusCode::kIoError, std::move(message));
}

void BindText(sqlite3_stmt* statement, int index, std::string_view value)
{
    sqlite3_bind_text(
        statement,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
}

std::optional<Digest> ReadDigest(sqlite3_stmt* statement, int column)
{
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    const auto* data = static_cast<const std::byte*>(
        sqlite3_column_blob(statement, column));
    const int size = sqlite3_column_bytes(statement, column);
    if (data == nullptr || size != 32) {
        return std::nullopt;
    }
    Digest digest;
    std::copy(data, data + 32, digest.bytes.begin());
    return digest;
}

UploadFileState FileState(int value)
{
    switch (value) {
    case 0: return UploadFileState::kPending;
    case 1: return UploadFileState::kReceiving;
    case 2: return UploadFileState::kReceived;
    default: return UploadFileState::kFailed;
    }
}

UploadSessionState SessionState(int value)
{
    switch (value) {
    case 1: return UploadSessionState::kComplete;
    case 2: return UploadSessionState::kFailed;
    default: return UploadSessionState::kOpen;
    }
}

}  // namespace

UploadSessionStore::UploadSessionStore(
    void* database,
    std::filesystem::path workspace_root,
    UploadQuota quota) noexcept
    : database_(database),
      workspace_root_(std::move(workspace_root)),
      quota_(quota)
{
}

UploadSessionStore::~UploadSessionStore()
{
    if (database_ != nullptr) {
        sqlite3_close(Database(database_));
    }
}

UploadSessionStore::UploadSessionStore(UploadSessionStore&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)),
      workspace_root_(std::move(other.workspace_root_)),
      quota_(other.quota_)
{
}

UploadSessionStore& UploadSessionStore::operator=(UploadSessionStore&& other) noexcept
{
    if (this != &other) {
        if (database_ != nullptr) {
            sqlite3_close(Database(database_));
        }
        database_ = std::exchange(other.database_, nullptr);
        workspace_root_ = std::move(other.workspace_root_);
        quota_ = other.quota_;
    }
    return *this;
}

StatusOr<UploadSessionStore> UploadSessionStore::Open(
    std::filesystem::path workspace_root,
    UploadQuota quota)
{
    if (workspace_root.empty()) {
        return Invalid("receiver workspace root must not be empty");
    }
    std::error_code error;
    std::filesystem::create_directories(
        workspace_root / "incoming" / ".tmp",
        error);
    if (error) {
        return Status(
            StatusCode::kIoError,
            "unable to create receiver staging directory: "
                + error.message());
    }

    sqlite3* database = nullptr;
    const auto database_path = workspace_root / "receiver.db";
    if (sqlite3_open_v2(
            database_path.string().c_str(),
            &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK) {
        const std::string message = database == nullptr
            ? "unable to open receiver database"
            : sqlite3_errmsg(database);
        if (database != nullptr) {
            sqlite3_close(database);
        }
        return Status(StatusCode::kIoError, message);
    }

    const char* schema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
CREATE TABLE IF NOT EXISTS upload_session(
  session_id TEXT PRIMARY KEY,
  created_at_ns INTEGER NOT NULL,
  expected_total_bytes INTEGER NOT NULL,
  received_total_bytes INTEGER NOT NULL,
  state INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS upload_file(
  session_id TEXT NOT NULL,
  file_id TEXT NOT NULL,
  original_filename BLOB NOT NULL,
  safe_filename TEXT NOT NULL,
  expected_size INTEGER NOT NULL,
  received_size INTEGER NOT NULL,
  client_digest BLOB,
  server_digest BLOB,
  state INTEGER NOT NULL,
  PRIMARY KEY(session_id, file_id),
  UNIQUE(session_id, safe_filename),
  FOREIGN KEY(session_id) REFERENCES upload_session(session_id)
);
)SQL";
    const Status schema_status = Exec(database, schema);
    if (!schema_status.ok()) {
        sqlite3_close(database);
        return schema_status;
    }
    UploadSessionStore store(
        database,
        std::move(workspace_root),
        quota);
    const Status recovery_status = store.RecoverInterruptedUploads();
    if (!recovery_status.ok()) {
        return recovery_status;
    }
    return store;
}

StatusOr<UploadSession> UploadSessionStore::CreateSession(
    const std::vector<UploadFileRequest>& files)
{
    if (files.empty()) {
        return Invalid("session must contain at least one file");
    }

    std::uint64_t total = 0;
    std::vector<std::string> safe_names;
    std::set<std::string> used_names;
    safe_names.reserve(files.size());
    for (const auto& file : files) {
        if (file.expected_size > quota_.max_file_bytes) {
            return Invalid("file exceeds receiver file quota");
        }
        if (AddWouldOverflow(total, file.expected_size)) {
            return Invalid("session size overflows uint64");
        }
        total += file.expected_size;
        if (total > quota_.max_session_bytes) {
            return Invalid("session exceeds receiver quota");
        }

        auto safe_name = SafeFilename(file.original_filename);
        if (!safe_name.ok()) {
            return safe_name.status();
        }
        std::string candidate = safe_name.value();
        std::size_t suffix = 1;
        while (!used_names.insert(candidate).second) {
            candidate = WithCollisionSuffix(safe_name.value(), ++suffix);
        }
        safe_names.push_back(std::move(candidate));
    }

    sqlite3* database = Database(database_);
    Status status = Exec(database, "BEGIN IMMEDIATE;");
    if (!status.ok()) {
        return status;
    }

    const auto rollback = [&]() {
        Exec(database, "ROLLBACK;");
    };

    UploadSession session;
    session.session_id = NewId("session");
    session.created_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    session.expected_total_bytes = total;

    sqlite3_stmt* quota_statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "SELECT COALESCE(SUM(expected_total_bytes), 0) "
            "FROM upload_session WHERE state != 2;",
            -1,
            &quota_statement,
            nullptr) != SQLITE_OK) {
        rollback();
        return SqliteError(database, "prepare receiver quota lookup");
    }
    const int quota_result = sqlite3_step(quota_statement);
    const auto reserved = quota_result == SQLITE_ROW
        ? static_cast<std::uint64_t>(sqlite3_column_int64(quota_statement, 0))
        : std::numeric_limits<std::uint64_t>::max();
    sqlite3_finalize(quota_statement);
    if (quota_result != SQLITE_ROW
        || AddWouldOverflow(reserved, total)
        || reserved + total > quota_.max_reserved_bytes) {
        rollback();
        return Status(
            StatusCode::kInvalidArgument,
            "receiver reserved-byte quota exceeded");
    }

    std::error_code directory_error;
    std::filesystem::create_directories(
        workspace_root_ / "incoming" / session.session_id,
        directory_error);
    std::filesystem::create_directories(
        workspace_root_ / "incoming" / ".tmp" / session.session_id,
        directory_error);
    if (directory_error) {
        rollback();
        return Status(
            StatusCode::kIoError,
            "unable to create session staging directory: "
                + directory_error.message());
    }

    sqlite3_stmt* session_statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "INSERT INTO upload_session(session_id, created_at_ns, "
            "expected_total_bytes, received_total_bytes, state) "
            "VALUES(?, ?, ?, 0, 0);",
            -1,
            &session_statement,
            nullptr) != SQLITE_OK) {
        rollback();
        return SqliteError(database, "prepare session insert");
    }
    BindText(session_statement, 1, session.session_id);
    sqlite3_bind_int64(session_statement, 2, session.created_at_ns);
    sqlite3_bind_int64(
        session_statement,
        3,
        static_cast<sqlite3_int64>(total));
    const int session_result = sqlite3_step(session_statement);
    sqlite3_finalize(session_statement);
    if (session_result != SQLITE_DONE) {
        rollback();
        return SqliteError(database, "insert session");
    }

    sqlite3_stmt* file_statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "INSERT INTO upload_file(session_id, file_id, "
            "original_filename, safe_filename, expected_size, "
            "received_size, client_digest, server_digest, state) "
            "VALUES(?, ?, ?, ?, ?, 0, ?, NULL, 0);",
            -1,
            &file_statement,
            nullptr) != SQLITE_OK) {
        rollback();
        return SqliteError(database, "prepare file insert");
    }

    for (std::size_t index = 0; index < files.size(); ++index) {
        const std::string file_id = NewId("file");
        sqlite3_reset(file_statement);
        sqlite3_clear_bindings(file_statement);
        BindText(file_statement, 1, session.session_id);
        BindText(file_statement, 2, file_id);
        BindText(file_statement, 3, files[index].original_filename);
        BindText(file_statement, 4, safe_names[index]);
        sqlite3_bind_int64(
            file_statement,
            5,
            static_cast<sqlite3_int64>(files[index].expected_size));
        if (files[index].client_digest.has_value()) {
            sqlite3_bind_blob(
                file_statement,
                6,
                files[index].client_digest->bytes.data(),
                static_cast<int>(files[index].client_digest->bytes.size()),
                SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(file_statement, 6);
        }
        if (sqlite3_step(file_statement) != SQLITE_DONE) {
            sqlite3_finalize(file_statement);
            rollback();
            return SqliteError(database, "insert upload file");
        }

        session.files.push_back(UploadFile{
            file_id,
            files[index].original_filename,
            safe_names[index],
            files[index].expected_size,
            0,
            files[index].client_digest,
            std::nullopt,
            UploadFileState::kPending,
        });
    }
    sqlite3_finalize(file_statement);

    status = Exec(database, "COMMIT;");
    if (!status.ok()) {
        rollback();
        return status;
    }
    return session;
}

StatusOr<UploadSession> UploadSessionStore::GetSession(
    std::string_view session_id) const
{
    if (session_id.empty()) {
        return Invalid("session id must not be empty");
    }
    sqlite3* database = Database(database_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "SELECT created_at_ns, expected_total_bytes, "
            "received_total_bytes, state FROM upload_session "
            "WHERE session_id = ?;",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        return SqliteError(database, "prepare session lookup");
    }
    BindText(statement, 1, session_id);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return Status(StatusCode::kNotFound, "upload session not found");
    }

    UploadSession session;
    session.session_id = std::string(session_id);
    session.created_at_ns = sqlite3_column_int64(statement, 0);
    session.expected_total_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement, 1));
    session.received_total_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement, 2));
    session.state = SessionState(sqlite3_column_int(statement, 3));
    sqlite3_finalize(statement);

    if (sqlite3_prepare_v2(
            database,
            "SELECT file_id, original_filename, safe_filename, "
            "expected_size, received_size, client_digest, server_digest, "
            "state FROM upload_file WHERE session_id = ? "
            "ORDER BY rowid;",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        return SqliteError(database, "prepare file lookup");
    }
    BindText(statement, 1, session_id);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* original = reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 1));
        const auto* safe = reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 2));
        const auto* file_id = reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 0));
        session.files.push_back(UploadFile{
            file_id == nullptr ? "" : file_id,
            original == nullptr ? "" : original,
            safe == nullptr ? "" : safe,
            static_cast<std::uint64_t>(sqlite3_column_int64(statement, 3)),
            static_cast<std::uint64_t>(sqlite3_column_int64(statement, 4)),
            ReadDigest(statement, 5),
            ReadDigest(statement, 6),
            FileState(sqlite3_column_int(statement, 7)),
        });
    }
    sqlite3_finalize(statement);
    return session;
}

namespace {

StatusOr<Digest> HashRegularFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Status(StatusCode::kIoError, "unable to open recovered upload");
    }
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    std::array<char, 1024 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            blake3_hasher_update(
                &hasher,
                buffer.data(),
                static_cast<std::size_t>(count));
        }
    }
    if (!input.eof()) {
        return Status(StatusCode::kIoError, "unable to read recovered upload");
    }
    std::array<std::uint8_t, 32> bytes{};
    blake3_hasher_finalize(&hasher, bytes.data(), bytes.size());
    Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        digest.bytes[index] = static_cast<std::byte>(bytes[index]);
    }
    return digest;
}

}  // namespace

Status UploadSessionStore::RecoverInterruptedUploads()
{
    sqlite3* database = Database(database_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "SELECT session_id, file_id, safe_filename, expected_size "
            "FROM upload_file WHERE state = 1;",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        return SqliteError(database, "prepare interrupted upload recovery");
    }
    struct Candidate {
        std::string session_id;
        std::string file_id;
        std::string safe_filename;
        std::uint64_t expected_size;
    };
    std::vector<Candidate> candidates;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        candidates.push_back({
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
            static_cast<std::uint64_t>(sqlite3_column_int64(statement, 3)),
        });
    }
    sqlite3_finalize(statement);

    for (const auto& candidate : candidates) {
        const auto final = FinalPath(
            candidate.session_id,
            candidate.safe_filename);
        std::error_code error;
        const bool exists = std::filesystem::is_regular_file(final, error);
        if (!error && exists
            && std::filesystem::file_size(final, error) == candidate.expected_size
            && !error) {
            auto digest = HashRegularFile(final);
            if (digest.ok()) {
                const Status status = CommitUpload(
                    candidate.session_id,
                    candidate.file_id,
                    candidate.expected_size,
                    digest.value());
                if (status.ok()) {
                    std::filesystem::remove(
                        TempPath(candidate.session_id, candidate.file_id),
                        error);
                    continue;
                }
            }
        }
        const Status failed = MarkUploadFailed(
            candidate.session_id,
            candidate.file_id);
        if (!failed.ok()) {
            return failed;
        }
    }
    return Status::Ok();
}

StatusOr<UploadFile> UploadSessionStore::GetFile(
    std::string_view session_id,
    std::string_view file_id) const
{
    auto session = GetSession(session_id);
    if (!session.ok()) {
        return session.status();
    }
    for (const auto& file : session.value().files) {
        if (file.file_id == file_id) {
            return file;
        }
    }
    return Status(StatusCode::kNotFound, "upload file not found");
}

StatusOr<BeginUploadResult> UploadSessionStore::BeginUpload(
    std::string_view session_id,
    std::string_view file_id)
{
    sqlite3* database = Database(database_);
    Status status = Exec(database, "BEGIN IMMEDIATE;");
    if (!status.ok()) {
        return status;
    }
    const auto rollback = [&]() { Exec(database, "ROLLBACK;"); };

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "SELECT state FROM upload_file WHERE session_id = ? "
            "AND file_id = ?;",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        rollback();
        return SqliteError(database, "prepare upload state lookup");
    }
    BindText(statement, 1, session_id);
    BindText(statement, 2, file_id);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        rollback();
        return Status(StatusCode::kNotFound, "upload file not found");
    }
    const int state = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);

    if (state == 2) {
        rollback();
        return BeginUploadResult::kAlreadyReceived;
    }
    if (state == 1) {
        rollback();
        return BeginUploadResult::kBusy;
    }

    if (sqlite3_prepare_v2(
            database,
            "UPDATE upload_file SET state = 1 WHERE session_id = ? "
            "AND file_id = ? AND state IN (0, 3);",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        rollback();
        return SqliteError(database, "prepare begin upload");
    }
    BindText(statement, 1, session_id);
    BindText(statement, 2, file_id);
    const int result = sqlite3_step(statement);
    const int changes = sqlite3_changes(database);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE || changes != 1) {
        rollback();
        return SqliteError(database, "begin upload");
    }

    status = Exec(database, "COMMIT;");
    if (!status.ok()) {
        rollback();
        return status;
    }
    return BeginUploadResult::kStarted;
}

Status UploadSessionStore::MarkUploadFailed(
    std::string_view session_id,
    std::string_view file_id)
{
    sqlite3* database = Database(database_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "UPDATE upload_file SET state = 3 WHERE session_id = ? "
            "AND file_id = ? AND state = 1;",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        return SqliteError(database, "prepare failed upload update");
    }
    BindText(statement, 1, session_id);
    BindText(statement, 2, file_id);
    const int result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        return SqliteError(database, "mark upload failed");
    }
    return Status::Ok();
}

Status UploadSessionStore::CommitUpload(
    std::string_view session_id,
    std::string_view file_id,
    std::uint64_t received_size,
    const Digest& server_digest)
{
    sqlite3* database = Database(database_);
    Status status = Exec(database, "BEGIN IMMEDIATE;");
    if (!status.ok()) {
        return status;
    }
    const auto rollback = [&]() { Exec(database, "ROLLBACK;"); };

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "UPDATE upload_file SET received_size = ?, server_digest = ?, "
            "state = 2 WHERE session_id = ? AND file_id = ? AND state = 1 "
            "AND expected_size = ?;",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        rollback();
        return SqliteError(database, "prepare commit upload");
    }
    sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(received_size));
    sqlite3_bind_blob(
        statement,
        2,
        server_digest.bytes.data(),
        static_cast<int>(server_digest.bytes.size()),
        SQLITE_TRANSIENT);
    BindText(statement, 3, session_id);
    BindText(statement, 4, file_id);
    sqlite3_bind_int64(statement, 5, static_cast<sqlite3_int64>(received_size));
    const int result = sqlite3_step(statement);
    const int changes = sqlite3_changes(database);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE || changes != 1) {
        rollback();
        return Status(
            StatusCode::kInvalidArgument,
            "received upload size does not match the registered file");
    }

    if (sqlite3_prepare_v2(
            database,
            "UPDATE upload_session SET received_total_bytes = "
            "(SELECT COALESCE(SUM(received_size), 0) FROM upload_file "
            "WHERE session_id = ?) WHERE session_id = ?;",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        rollback();
        return SqliteError(database, "prepare session progress update");
    }
    BindText(statement, 1, session_id);
    BindText(statement, 2, session_id);
    if (sqlite3_step(statement) != SQLITE_DONE) {
        sqlite3_finalize(statement);
        rollback();
        return SqliteError(database, "update session progress");
    }
    sqlite3_finalize(statement);

    status = Exec(database, "COMMIT;");
    if (!status.ok()) {
        rollback();
    }
    return status;
}

Status UploadSessionStore::CompleteSession(std::string_view session_id)
{
    sqlite3* database = Database(database_);
    Status status = Exec(database, "BEGIN IMMEDIATE;");
    if (!status.ok()) {
        return status;
    }
    const auto rollback = [&]() { Exec(database, "ROLLBACK;"); };

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "SELECT COUNT(*), COALESCE(SUM(CASE WHEN state != 2 THEN 1 ELSE 0 END), 0) "
            "FROM upload_file WHERE session_id = ?;",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        rollback();
        return SqliteError(database, "prepare session completion check");
    }
    BindText(statement, 1, session_id);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        rollback();
        return SqliteError(database, "check session completion");
    }
    const auto file_count = sqlite3_column_int64(statement, 0);
    const auto incomplete_count = sqlite3_column_int64(statement, 1);
    sqlite3_finalize(statement);
    if (file_count == 0) {
        rollback();
        return Status(StatusCode::kNotFound, "upload session not found");
    }
    if (incomplete_count != 0) {
        rollback();
        return Status(
            StatusCode::kInvalidArgument,
            "upload session still has incomplete files");
    }

    if (sqlite3_prepare_v2(
            database,
            "UPDATE upload_session SET state = 1 WHERE session_id = ? "
            "AND state = 0;",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        rollback();
        return SqliteError(database, "prepare session completion update");
    }
    BindText(statement, 1, session_id);
    const int result = sqlite3_step(statement);
    const int changes = sqlite3_changes(database);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        rollback();
        return SqliteError(database, "complete upload session");
    }
    if (changes == 0) {
        rollback();
        return Status(StatusCode::kAlreadyExists, "upload session is complete");
    }

    status = Exec(database, "COMMIT;");
    if (!status.ok()) {
        rollback();
    }
    return status;
}

std::filesystem::path UploadSessionStore::TempPath(
    std::string_view session_id,
    std::string_view file_id) const
{
    return workspace_root_ / "incoming" / ".tmp"
        / std::string(session_id) / (std::string(file_id) + ".pbtmp");
}

std::filesystem::path UploadSessionStore::FinalPath(
    std::string_view session_id,
    std::string_view safe_filename) const
{
    return workspace_root_ / "incoming" / std::string(session_id)
        / std::string(safe_filename);
}

}  // namespace photobridge
