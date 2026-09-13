#include "photobridge/cli/serve_command.h"

#include <utility>

#include "photobridge/lan/http_server.h"
#include "photobridge/lan/serve_bootstrap.h"
#include "photobridge/lan/upload_page.h"
#include "photobridge/lan/upload_session_store.h"

namespace photobridge {

ServeCommand::ServeCommand(
    std::string workspace_path,
    std::string bind_address,
    std::uint16_t port,
    bool bootstrap_only)
    : workspace_path_(std::move(workspace_path)),
      bind_address_(std::move(bind_address)),
      port_(port),
      bootstrap_only_(bootstrap_only)
{
}

Status ServeCommand::Execute(CommandContext& context)
{
    if (workspace_path_.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "serve workspace must not be empty");
    }

    auto bootstrap = ServeBootstrap::Create(ServeOptions{
        bind_address_,
        port_,
    });
    if (!bootstrap.ok()) {
        return bootstrap.status();
    }

    context.out << "serve bootstrap\n"
                << "bind: " << bootstrap.value().bind_address << "\n"
                << "port: " << bootstrap.value().port << "\n"
                << "token: " << bootstrap.value().token << "\n"
                << "url: " << bootstrap.value().url << "\n"
                << "qr:\n" << bootstrap.value().qr_code;
    if (bootstrap_only_) {
        return Status::Ok();
    }

    auto store = UploadSessionStore::Open(workspace_path_);
    if (!store.ok()) {
        return store.status();
    }
    const auto page = GetUploadPageResponse();
    return RunUploadHttpServer(
        bootstrap.value(),
        page,
        store.value(),
        context.out,
        context.err);
}

}  // namespace photobridge
