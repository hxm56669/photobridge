#include "photobridge/lan/http_server.h"

#include <ostream>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <string_view>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <blake3.h>
#include <linux/fs.h>

#include "photobridge/app/byte_permit_pool.h"
#include "photobridge/app/fd_permit_pool.h"
#include "photobridge/lan/upload_page.h"

namespace photobridge {
namespace {

void SetText(httplib::Response& response, int status, std::string body)
{
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(std::move(body), "text/plain; charset=utf-8");
}

bool WriteAll(int fd, const char* data, std::size_t size)
{
    while (size > 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool PublishNoReplace(
    const std::filesystem::path& temporary,
    const std::filesystem::path& final)
{
    const long result = syscall(
        SYS_renameat2,
        AT_FDCWD,
        temporary.c_str(),
        AT_FDCWD,
        final.c_str(),
        RENAME_NOREPLACE);
    return result == 0;
}

class FdPermitGuard final {
public:
    explicit FdPermitGuard(FdPermitPool& pool) noexcept
        : pool_(&pool) {}

    FdPermitGuard(const FdPermitGuard&) = delete;
    FdPermitGuard& operator=(const FdPermitGuard&) = delete;

    ~FdPermitGuard()
    {
        pool_->Release();
    }

private:
    FdPermitPool* pool_;
};

StatusOr<Digest> ReceiveBody(
    const httplib::ContentReader& reader,
    const std::filesystem::path& temporary,
    std::uint64_t expected_size,
    bool& size_overflow,
    BytePermitPool& byte_budget)
{
    std::error_code error;
    std::filesystem::remove(temporary, error);
    const int fd = ::open(
        temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        0600);
    if (fd < 0) {
        return Status(
            StatusCode::kIoError,
            "unable to create upload temporary file");
    }

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    std::uint64_t received = 0;
    const bool read_ok = reader([&](const char* data, std::size_t size) {
        if (size > expected_size - received) {
            size_overflow = true;
            return false;
        }
        const Status permit_status = byte_budget.Acquire(size);
        if (!permit_status.ok()) {
            return false;
        }
        if (!WriteAll(fd, data, size)) {
            byte_budget.Release(size);
            return false;
        }
        blake3_hasher_update(&hasher, data, size);
        received += static_cast<std::uint64_t>(size);
        byte_budget.Release(size);
        return true;
    });

    const bool sync_ok = read_ok && received == expected_size && fsync(fd) == 0;
    std::array<std::uint8_t, 32> digest_bytes{};
    if (sync_ok) {
        blake3_hasher_finalize(&hasher, digest_bytes.data(), digest_bytes.size());
    }
    const int close_result = ::close(fd);
    if (!sync_ok || close_result != 0) {
        ::unlink(temporary.c_str());
        if (size_overflow) {
            return Status(
                StatusCode::kInvalidArgument,
                "upload body exceeds registered file size");
        }
        return Status(
            StatusCode::kIoError,
            received == expected_size
                ? "unable to synchronize upload temporary file"
                : "upload body size does not match registered file");
    }

    Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        digest.bytes[index] = static_cast<std::byte>(digest_bytes[index]);
    }
    return digest;
}

}  // namespace

Status RunUploadHttpServer(
    const ServeBootstrap& bootstrap,
    const UploadPageResponse& page,
    UploadSessionStore& store,
    std::ostream& out,
    std::ostream& err)
{
    httplib::Server server;
    server.set_read_timeout(30, 0);
    server.set_keep_alive_max_count(32);
    server.set_payload_max_length(
        2ULL * 1024ULL * 1024ULL * 1024ULL);
    FdPermitPool active_uploads(3);
    BytePermitPool byte_budget(16ULL * 1024ULL * 1024ULL);

    const auto unauthorized = [](httplib::Response& response) {
        response.status = 401;
        response.set_header("Cache-Control", "no-store");
        response.set_content("unauthorized\n", "text/plain; charset=utf-8");
    };
    const auto api_authorized = [&](const httplib::Request& request) {
        constexpr std::string_view prefix = "Bearer ";
        const std::string authorization = request.get_header_value(
            "Authorization");
        return authorization.size() > prefix.size()
            && authorization.compare(0, prefix.size(), prefix) == 0
            && ConstantTimeTokenEquals(
                std::string_view(authorization).substr(prefix.size()),
                bootstrap.token);
    };
    const auto session_json = [](const UploadSession& session) {
        nlohmann::json result{
            {"session_id", session.session_id},
            {"created_at_ns", session.created_at_ns},
            {"expected_total_bytes", session.expected_total_bytes},
            {"received_total_bytes", session.received_total_bytes},
            {"state", session.state == UploadSessionState::kOpen
                ? "OPEN" : session.state == UploadSessionState::kComplete
                    ? "COMPLETE" : "FAILED"},
            {"files", nlohmann::json::array()},
        };
        for (const auto& file : session.files) {
            result["files"].push_back({
                {"file_id", file.file_id},
                {"original_filename", file.original_filename},
                {"safe_filename", file.safe_filename},
                {"expected_size", file.expected_size},
                {"received_size", file.received_size},
                {"state", file.state == UploadFileState::kPending
                    ? "PENDING" : file.state == UploadFileState::kReceiving
                        ? "RECEIVING" : file.state == UploadFileState::kReceived
                            ? "RECEIVED" : "FAILED"},
            });
        }
        return result.dump();
    };

    server.Get("/", [&](const httplib::Request& request, httplib::Response& response) {
        if (!request.has_param("t")
            || !ConstantTimeTokenEquals(
                request.get_param_value("t"),
                bootstrap.token)) {
            unauthorized(response);
            return;
        }
        response.status = 200;
        response.set_header("Cache-Control", "no-store");
        response.set_content(page.body, page.content_type);
    });

    server.Post("/api/v1/sessions", [&](const httplib::Request& request, httplib::Response& response) {
        if (!api_authorized(request)) {
            unauthorized(response);
            return;
        }
        try {
            const auto json = nlohmann::json::parse(request.body);
            if (!json.is_object() || !json.contains("files")
                || !json["files"].is_array()) {
                response.status = 400;
                response.set_content(
                    "request must contain a files array\n",
                    "text/plain; charset=utf-8");
                return;
            }
            std::vector<UploadFileRequest> files;
            for (const auto& value : json["files"]) {
                if (!value.is_object()
                    || !value.contains("original_filename")
                    || !value.contains("file_size")
                    || !value["original_filename"].is_string()
                    || !value["file_size"].is_number_unsigned()) {
                    response.status = 400;
                    response.set_content(
                        "each file requires original_filename and file_size\n",
                        "text/plain; charset=utf-8");
                    return;
                }
                UploadFileRequest file{
                    value["original_filename"].get<std::string>(),
                    value["file_size"].get<std::uint64_t>(),
                    std::nullopt,
                };
                if (value.contains("client_hash")) {
                    if (!value["client_hash"].is_string()) {
                        response.status = 400;
                        response.set_content(
                            "client_hash must be a hex string\n",
                            "text/plain; charset=utf-8");
                        return;
                    }
                    auto digest = Digest::FromHex(
                        value["client_hash"].get<std::string>());
                    if (!digest.ok()) {
                        response.status = 400;
                        response.set_content(
                            digest.status().message() + "\n",
                            "text/plain; charset=utf-8");
                        return;
                    }
                    file.client_digest = digest.value();
                }
                files.push_back(std::move(file));
            }
            auto session = store.CreateSession(files);
            if (!session.ok()) {
                response.status = session.status().code()
                    == StatusCode::kInvalidArgument ? 400 : 500;
                response.set_content(
                    session.status().message() + "\n",
                    "text/plain; charset=utf-8");
                return;
            }
            response.status = 201;
            response.set_header("Cache-Control", "no-store");
            response.set_content(
                session_json(session.value()),
                "application/json; charset=utf-8");
        } catch (const nlohmann::json::exception& error) {
            response.status = 400;
            response.set_content(
                std::string("invalid JSON: ") + error.what() + "\n",
                "text/plain; charset=utf-8");
        }
    });

    server.Post("/api/v1/sessions/([^/]+)/complete", [&](const httplib::Request& request, httplib::Response& response) {
        if (!api_authorized(request)) {
            unauthorized(response);
            return;
        }
        const Status status = store.CompleteSession(request.matches[1].str());
        if (!status.ok()) {
            response.status = status.code() == StatusCode::kNotFound ? 404
                : status.code() == StatusCode::kInvalidArgument ? 400 : 500;
            response.set_content(
                status.message() + "\n",
                "text/plain; charset=utf-8");
            return;
        }
        response.status = 200;
        response.set_content(
            "{\"state\":\"COMPLETE\"}\n",
            "application/json; charset=utf-8");
    });

    server.Get("/api/v1/sessions/(.*)", [&](const httplib::Request& request, httplib::Response& response) {
        if (!api_authorized(request)) {
            unauthorized(response);
            return;
        }
        const auto session = store.GetSession(request.matches[1].str());
        if (!session.ok()) {
            response.status = session.status().code() == StatusCode::kNotFound
                ? 404 : 500;
            response.set_content(
                session.status().message() + "\n",
                "text/plain; charset=utf-8");
            return;
        }
        response.status = 200;
        response.set_header("Cache-Control", "no-store");
        response.set_content(
            session_json(session.value()),
            "application/json; charset=utf-8");
    });

    server.Put("/api/v1/sessions/([^/]+)/files/([^/]+)",
        [&](const httplib::Request& request,
            httplib::Response& response,
            const httplib::ContentReader& reader) {
            if (!api_authorized(request)) {
                unauthorized(response);
                return;
            }
            const std::string session_id = request.matches[1].str();
            const std::string file_id = request.matches[2].str();
            auto file = store.GetFile(session_id, file_id);
            if (!file.ok()) {
                SetText(response, 404, file.status().message() + "\n");
                return;
            }
            auto begin = store.BeginUpload(session_id, file_id);
            if (!begin.ok()) {
                SetText(response, 500, begin.status().message() + "\n");
                return;
            }
            if (begin.value() == BeginUploadResult::kAlreadyReceived) {
                response.status = 200;
                response.set_content(
                    "{\"state\":\"RECEIVED\"}\n",
                    "application/json; charset=utf-8");
                return;
            }
            if (begin.value() == BeginUploadResult::kBusy) {
                SetText(response, 409, "upload is already receiving\n");
                return;
            }

            const Status permit_status = active_uploads.Acquire();
            if (!permit_status.ok()) {
                SetText(response, 503, "upload capacity is temporarily full\n");
                return;
            }
            FdPermitGuard permit_guard(active_uploads);

            bool size_overflow = false;
            const auto temporary = store.TempPath(session_id, file_id);
            auto digest = ReceiveBody(
                reader,
                temporary,
                file.value().expected_size,
                size_overflow,
                byte_budget);
            if (!digest.ok()) {
                store.MarkUploadFailed(session_id, file_id);
                SetText(
                    response,
                    size_overflow ? 413 : 400,
                    digest.status().message() + "\n");
                return;
            }
            if (file.value().client_digest.has_value()
                && file.value().client_digest.value() != digest.value()) {
                store.MarkUploadFailed(session_id, file_id);
                ::unlink(temporary.c_str());
                SetText(response, 422, "client hash does not match upload\n");
                return;
            }

            const auto final = store.FinalPath(
                session_id,
                file.value().safe_filename);
            if (!PublishNoReplace(temporary, final)) {
                const int publish_error = errno;
                store.MarkUploadFailed(session_id, file_id);
                SetText(response, publish_error == EEXIST ? 409 : 500,
                    publish_error == EEXIST
                        ? "upload target already exists\n"
                        : "unable to publish upload\n");
                return;
            }
            const int directory_fd = ::open(
                final.parent_path().c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            const bool directory_synced = directory_fd >= 0
                && ::fsync(directory_fd) == 0;
            if (directory_fd >= 0) {
                ::close(directory_fd);
            }
            if (!directory_synced
                || !store.CommitUpload(
                    session_id,
                    file_id,
                    file.value().expected_size,
                    digest.value()).ok()) {
                SetText(response, 500, "upload commit is not durable\n");
                return;
            }
            response.status = 201;
            response.set_content(
                std::string("{\"state\":\"RECEIVED\",\"server_hash\":\"")
                    + digest.value().ToHex() + "\"}\n",
                "application/json; charset=utf-8");
        });

    out << "listening on " << bootstrap.url << "\n";
    out << "press Ctrl-C to stop\n";
    if (!server.listen(bootstrap.bind_address.c_str(), bootstrap.port)) {
        err << "unable to listen on " << bootstrap.bind_address << ":"
            << bootstrap.port << "\n";
        return Status(
            StatusCode::kIoError,
            "serve listener could not be started");
    }
    return Status::Ok();
}

}  // namespace photobridge
