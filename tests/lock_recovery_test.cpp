#include <cerrno>
#include <csignal>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

TEST(LockRecoveryTest, KilledOwnerReleasesStableLockForNextOwner)
{
    const auto path = std::filesystem::temp_directory_path()
        / ("photobridge_lock_recovery_" + std::to_string(getpid()));
    const int parent_fd = ::open(
        path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    ASSERT_NE(parent_fd, -1);

    int ready_pipe[2] = {-1, -1};
    ASSERT_EQ(::pipe(ready_pipe), 0);
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        ::close(ready_pipe[0]);
        const int child_fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (child_fd == -1 || ::flock(child_fd, LOCK_EX) != 0) {
            _exit(2);
        }
        const char ready = '1';
        if (::write(ready_pipe[1], &ready, 1) != 1) _exit(3);
        for (;;) ::pause();
    }

    ::close(ready_pipe[1]);
    char ready = 0;
    ASSERT_EQ(::read(ready_pipe[0], &ready, 1), 1);
    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int wait_status = 0;
    ASSERT_EQ(::waitpid(child, &wait_status, 0), child);
    EXPECT_TRUE(WIFSIGNALED(wait_status));

    const int next_fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    ASSERT_NE(next_fd, -1);
    EXPECT_EQ(::flock(next_fd, LOCK_EX | LOCK_NB), 0);
    ::close(next_fd);
    ::close(parent_fd);
    ::close(ready_pipe[0]);
    std::error_code error;
    std::filesystem::remove(path, error);
    EXPECT_FALSE(error);
}
