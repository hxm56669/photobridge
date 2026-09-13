#pragma once

#include <iosfwd>

#include "photobridge/common/status.h"
#include "photobridge/lan/serve_bootstrap.h"
#include "photobridge/lan/upload_session_store.h"

namespace photobridge {

struct UploadPageResponse;

Status RunUploadHttpServer(
    const ServeBootstrap& bootstrap,
    const UploadPageResponse& page,
    UploadSessionStore& store,
    std::ostream& out,
    std::ostream& err);

}  // namespace photobridge
