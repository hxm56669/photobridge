#pragma once

#include "photobridge/app/sqlite_connection.h"
#include "photobridge/common/status.h"

namespace photobridge {

inline constexpr int kCurrentSchemaVersion = 5;

Status EnsureSchema(SqliteConnection& connection);

}  // namespace photobridge
