#include "mokv/raft/service.hpp"
#include "mokv/config.hpp"
#include "mokv/raft/config.hpp"
#include "mokv/resource_manager.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <getopt.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace {

std::filesystem::path ResolveDataDir(const std::filesystem::path& configured,
                                     const std::filesystem::path& config_file,
                                     const std::filesystem::path& launch_dir) {
    const auto base_dir = config_file.has_parent_path() ? config_file.parent_path() : launch_dir;
    if (configured.empty()) {
        return std::filesystem::absolute(base_dir / "data");
    }
    if (configured.is_absolute()) {
        return configured;
    }
    return std::filesystem::absolute(base_dir / configured);
}

std::unique_ptr<grpc::Server> g_server;
std::atomic_bool g_shutdown_flag{false};
std::atomic_bool g_server_shutdown_requested{false};

// 信号处理函数
void SignalHandler(int signum) {
    std::cout << "Received signal " << signum << ", shutting down..." << std::endl;
    g_shutdown_flag = true;
    // 设置关闭标志，由主循环处理关闭
    g_server_shutdown_requested = true;
}

// 打印使用帮助
void PrintUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -d, --daemon       Run as daemon\n"
              << "  -c, --config <file> Config file path (default: raft.cfg)\n"
              << "  -l, --log <file>   Log file path (default: mokv.log)\n"
              << "  -P, --pid <file>   PID file path (default: mokv.pid)\n"
              << "  -h, --help         Show this help message\n"
              << "  -v, --version      Show version\n";
}

// Daemonize the process
bool Daemonize(const std::string& log_file, const std::string& pid_file) {
    // 1. 创建子进程，父进程退出
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork: " << strerror(errno) << std::endl;
        return false;
    }
    if (pid > 0) {
        // 父进程等待子进程启动后退出
        sleep(1);
        exit(0);
    }

    // 2. 创建新会话，成为会话 leader
    if (setsid() < 0) {
        std::cerr << "Failed to setsid: " << strerror(errno) << std::endl;
        return false;
    }

    // 3. 再次 fork，防止重新获得控制终端
    pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork (second): " << strerror(errno) << std::endl;
        return false;
    }
    if (pid > 0) {
        exit(0);
    }

    // 4. 改变工作目录到根目录，防止占用目录
    if (chdir("/") < 0) {
        std::cerr << "Failed to chdir: " << strerror(errno) << std::endl;
        return false;
    }

    // 5. 重定向标准输入输出错误到日志文件
    umask(0);

    // 打开日志文件
    int log_fd = open(log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        std::cerr << "Failed to open log file: " << strerror(errno) << std::endl;
        return false;
    }

    // 重定向
    dup2(log_fd, STDIN_FILENO);
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);

    if (log_fd > STDERR_FILENO) {
        close(log_fd);
    }

    // 6. 写入 PID 文件
    std::ofstream pid_file_stream(pid_file);
    if (pid_file_stream.is_open()) {
        pid_file_stream << getpid() << std::endl;
        pid_file_stream.close();
    }

    return true;
}

// 检查进程是否在运行
bool IsProcessRunning(pid_t pid) {
    if (pid <= 0) return false;
    // 发送信号 0 检查进程是否存在
    return kill(pid, 0) == 0;
}

}  // namespace

bool RunServer(const mokv::MokvConfig& config) {
    if (!mokv::ResourceManager::instance().LoadConfig(config)) {
        std::cerr << "failed to initialize raft config" << std::endl;
        return false;
    }
    mokv::ResourceManager::instance().InitDb(config);
    mokv::ResourceManager::instance().InitPod();

    auto local_addr = mokv::ResourceManager::instance().config_manager().local_address();
    std::string server_addr = local_addr.ip() + ":" + std::to_string(local_addr.port());

    mokv::MokvServiceImpl service;
    grpc::EnableDefaultHealthCheckService(true);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    g_server = builder.BuildAndStart();
    std::cout << "mokv server listening on " << server_addr << std::endl;
    std::cout << "mokv version: 0.1.0" << std::endl;

    // 使用条件等待，支持信号中断
    while (!g_server_shutdown_requested && g_server) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 关闭服务器
    if (g_server) {
        g_server->Shutdown();
    }
    return true;
}

int main(int argc, char* argv[]) {
    // 命令行参数
    bool daemonize = false;
    // uint16_t port = 0;  // 预留：端口覆盖功能
    const auto current_dir = std::filesystem::current_path();
    std::filesystem::path config_file = current_dir / mokv::raft::ConfigManager::DefaultPath();
    std::filesystem::path log_file = current_dir / "mokv.log";
    std::filesystem::path pid_file = current_dir / "mokv.pid";

    // 解析命令行参数
    static struct option long_options[] = {
        {"daemon", no_argument, nullptr, 'd'},
        {"port", required_argument, nullptr, 'p'},
        {"config", required_argument, nullptr, 'c'},
        {"log", required_argument, nullptr, 'l'},
        {"pid", required_argument, nullptr, 'P'},
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, 'v'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "dc:l:P:hv", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'd':
                daemonize = true;
                break;
            case 'c':
                config_file = std::filesystem::absolute(optarg);
                break;
            case 'l':
                log_file = std::filesystem::absolute(optarg);
                break;
            case 'P':
                pid_file = std::filesystem::absolute(optarg);
                break;
            case 'h':
                PrintUsage(argv[0]);
                return 0;
            case 'v':
                std::cout << "mokv version 0.1.0" << std::endl;
                return 0;
            default:
                PrintUsage(argv[0]);
                return 1;
        }
    }

    // 检查是否已有进程在运行
    std::ifstream pid_file_stream(pid_file);
    if (pid_file_stream.is_open()) {
        pid_t old_pid;
        pid_file_stream >> old_pid;
        pid_file_stream.close();

        if (IsProcessRunning(old_pid)) {
            std::cerr << "Another instance is already running with PID " << old_pid << std::endl;
            return 1;
        }
    }

    // 注册信号处理
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGHUP, SignalHandler);  // 日志轮转等

    mokv::MokvConfig config = mokv::DefaultConfig();
    if (!config.LoadFromFile(config_file)) {
        std::cerr << "Failed to load config file: " << config_file << std::endl;
        return 1;
    }
    config.LoadFromEnv();
    config.data_dir = ResolveDataDir(config.data_dir, config_file, current_dir);
    if (!config.Validate()) {
        std::cerr << "Invalid config: " << config_file << std::endl;
        return 1;
    }

    if (daemonize) {
        if (!Daemonize(log_file.string(), pid_file.string())) {
            std::cerr << "Failed to daemonize" << std::endl;
            return 1;
        }
    }

    if (!RunServer(config)) {
        return 1;
    }

    while (!g_shutdown_flag) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    mokv::ResourceManager::instance().Close();

    // 清理 PID 文件
    std::remove(pid_file.c_str());

    std::cout << "Server shutdown complete" << std::endl;
    return 0;
}
