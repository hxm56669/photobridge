#pragma once

#include <filesystem>
#include <string_view>

#include "photobridge/common/status.h"
#include "photobridge/common/status_or.h"

struct sqlite3;

namespace photobridge {

class SqliteConnection {
public:
    SqliteConnection() = default;
    ~SqliteConnection();

    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;

    SqliteConnection(SqliteConnection&& other) noexcept;
    SqliteConnection& operator=(SqliteConnection&& other) noexcept;

    static StatusOr<SqliteConnection> Open(
        const std::filesystem::path& path);

    Status Execute(std::string_view sql) const;

    sqlite3* native_handle() const noexcept;

private:
    explicit SqliteConnection(sqlite3* handle) noexcept;

    sqlite3* handle_ = nullptr;
};

}  // namespace photobridge
