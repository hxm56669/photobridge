# PhotoBridge 教学状态

项目：PhotoBridge
当前层级：L0 进行中
当前里程碑：M0
当前阶段/组：A — 工程与可维护基线 / A9.7 完成，进入 A10

上一组完成：A1～A5（以现有代码为基线，尚未在当前 Windows 环境重新编译）
当前真实代码基线：已存在 Status、StatusOr、spdlog、GoogleTest/CTest 和 CLI11 骨架；原有 build/debug 为其他环境生成的旧构建目录。
已完成能力：建立 `photobridge_core` 静态库；`photobridge` 与 `photobridge_tests` 共同链接该库；测试目标不再直接编译 `status.cpp`；Core 对外公开 `include` 目录。
本阶段不变量：公共实现只有一个 Core 编译来源；可执行文件和测试通过同一实现验证；`main.cpp` 只初始化日志并调用 `RunCli()`；命令通过 `CommandContext` 输出并返回 `Status`；领域状态只在 `ExitCodeForStatus()` 处转换为稳定退出码；`WorkspaceLayout` 只推导路径；`WorkspaceService` 承担 `init` 的文件系统副作用；`UniqueFd` 独占 fd，禁止复制，移动后源对象为空；POSIX errno 必须在系统调用后立即传入 `StatusFromErrno()`，错误分类集中且稳定；`RelativePath` 拒绝绝对路径、空组件、`.`、`..` 和 NUL；`OpenSource` 只从 root fd 逐级打开并拒绝 symlink；`StatFd` 只从已打开 fd 读取 FileIdentity；`CreateTempNoReplace` 只对 parent fd 创建单一文件名，使用 `O_EXCL` 禁止覆盖；`Read`/`Write` 重试 `EINTR`、保留短读短写结果，不把单次系统调用伪装成完整传输；`Fdatasync` 和目录 `fsync` 重试 `EINTR`，成功才报告同步完成；`RenameNoReplace` 使用 `renameat2(RENAME_NOREPLACE)` 不覆盖已有目标；`UnlinkAt` 只删除 parent fd 下的单一文件名；`FileOps` 只定义端口，`LinuxFileOps` 承担 Linux 系统调用。
已通过测试：静态核对 CMake 中 `status.cpp` 仅出现在 `photobridge_core`；两个目标均链接 `photobridge_core`；CLI11 配置已从 `main.cpp` 移入 Core 中的 `RunCli()`；`Command::Run()` 已替换为 `Execute(CommandContext&) -> Status`；已建立 `StatusCode` 到进程退出码的固定映射。已添加 CLI/Dispatcher/退出码行为测试、WorkspaceLayout 测试和 `init` 集成测试，并将 `status_or_test.cpp` 纳入测试目标，但尚未在当前环境运行 CTest。`init` 当前创建数据库占位文件，schema 留给 A10。
当前失败/阻塞：当前 Windows 主机找不到 CMake，且没有可用 WSL 发行版；尚未完成本机配置、编译和 CTest 验证。
活跃 Decision：无。
活跃 Bug：无。
下一知识原子：SQLite 连接生命周期与 schema 版本边界。
下一唯一实现任务：A10 — 建立 SQLite Connection 和 Schema Version。
下一组需要携带的代码模块：`CMakeLists.txt`、`include/photobridge/common/status.h`、`include/photobridge/cli/command.h`、`include/photobridge/app/`、`src/app/`、`include/photobridge/cli/`、`src/cli/`、`tests/`。
