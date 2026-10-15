/*
 * PROJECT : SIFTWING
 * FILE    : logging_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 验证日志并发整行、固定上下文、独占文件与失败隔离合同
 */
#include "siftwing/logging/logger.hpp"
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace siftwing::logging;
using Json = nlohmann::json;
thread_local int fault = 0;
std::atomic<unsigned> calls{0};
void check(bool good, const char *label) {
    if (!good)
        throw std::runtime_error(label);
}
std::unique_ptr<Logger> create(Level level, const std::filesystem::path &path) {
    auto result = Logger::create(level, path.string());
    check(static_cast<bool>(result), "新文件准入");
    return std::move(result).value();
}
// 只替换当前测试线程的stderr；logger取得独立dup，恢复stderr不撤销已建立目标。
std::unique_ptr<Logger> redirected(int target) {
    const int saved = ::dup(STDERR_FILENO);
    check(saved >= 0 && ::dup2(target, STDERR_FILENO) >= 0, "保存stderr");
    auto result = Logger::create(Level::debug);
    check(::dup2(saved, STDERR_FILENO) >= 0 && ::close(saved) == 0, "恢复stderr");
    check(static_cast<bool>(result), "复制stderr");
    return std::move(result).value();
}
std::unique_ptr<Logger> fd_log(const std::filesystem::path &path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    check(fd >= 0, "故障注入独占文件");
    auto logger = redirected(fd);
    check(::close(fd) == 0, "释放故障文件原fd");
    return logger;
}
std::vector<Json> rows(const std::filesystem::path &path) {
    std::ifstream input(path);
    check(static_cast<bool>(input), "读取测试输出");
    std::vector<Json> output;
    std::string line;
    while (std::getline(input, line)) {
        check(line.size() + 1 <= 2048, "整行上限");
        output.push_back(Json::parse(line));
    }
    return output;
}
void exercise(const std::filesystem::path &root) {
    Record record;
    const auto file = root / "concurrent.jsonl";
    auto logger = create(Level::debug, file);
    constexpr unsigned count = 6, per_thread = 100;
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < count; ++thread)
        workers.emplace_back([&, thread] {
            Record own;
            own.level = Level::debug;
            own.module = Module::worker;
            own.event = Event::query_finished;
            own.connection = siftwing::net::ConnectionId{0, 0, UINT64_MAX};
            own.index_version = UINT64_MAX;
            for (unsigned n = 0; n < per_thread; ++n) {
                own.request_id = thread * per_thread + n;
                logger->write(own);
            }
        });
    for (auto &worker : workers)
        worker.join();
    check(!logger->failed(), "并发实例健康");
    auto output = rows(file);
    check(output.size() == count * per_thread, "并发无丢行");
    std::set<std::string> requests, threads;
    for (const auto &row : output) {
        requests.insert(row.at("request_id").get<std::string>());
        threads.insert(row.at("thread_id").get<std::string>());
        check(row.at("reactor_id") == 0 && row.at("connection_id") == 0
                  && row.at("generation") == "18446744073709551615"
                  && row.at("index_version") == "18446744073709551615",
            "0与完整uint64");
    }
    check(requests.size() == output.size() && threads.size() == count, "独立请求与实际线程");
    struct stat metadata {};
    check(::stat(file.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0600, "权限0600");
    check(!Logger::create(Level::debug, file.string()), "不得覆盖既有日志");
    check(rows(file).size() == output.size(), "拒绝后原字节保持");
    std::filesystem::create_directory_symlink(root, root / "link");
    check(!Logger::create(Level::debug, (root / "link/new").string()), "拒父链接");
    std::filesystem::create_symlink(file, root / "leaf");
    check(!Logger::create(Level::debug, (root / "leaf").string()), "拒末端链接");
    check(!Logger::create(Level::debug, (root / "../escape").string()), "拒点点");
    check(!Logger::create(Level::debug, (root / "missing/new").string()), "不创建目录");
    check(::mkfifo((root / "fifo").c_str(), 0600) == 0, "测试FIFO");
    check(!Logger::create(Level::debug, (root / "fifo").string()), "FIFO立即拒绝");
    check(!Logger::create(Level::off, (root / "off").string()), "off与文件不相容");
    auto disabled = Logger::create(Level::off);
    check(static_cast<bool>(disabled) && !disabled.value()->enabled(Level::error), "关闭阈值");
    disabled.value()->write_query(record, "secret invalid");
    check(!disabled.value()->failed(), "禁用不解析Body");
    const auto filtered = root / "filtered";
    auto info = create(Level::info, filtered);
    record.level = Level::debug;
    info->write(record);
    record.level = Level::info;
    info->write(record);
    output = rows(filtered);
    check(output.size() == 1 && output[0]["request_id"].is_null()
              && output[0]["connection_id"].is_null() && output[0]["index_version"].is_null(),
        "过滤和缺省null");
    const auto query_file = root / "query";
    auto query = create(Level::debug, query_file);
    query->write_query(record, R"({"results":[{"secret":"PRIVATE-PATH\nQUERY"},{}]})");
    output = rows(query_file);
    check(output.size() == 1 && output[0]["result_count"] == 2
              && output[0].dump().find("PRIVATE") == std::string::npos,
        "只提取数组长度");
    query->write_query(record, "{\"results\":0}");
    query->write(record);
    check(query->failed() && rows(query_file).size() == 1, "解析失败停用且不传播");
    // wrapper产生EINTR及短写，真实剩余字节仍走内核；期望是完整独立JSON行。
    const auto short_file = root / "short";
    auto short_log = fd_log(short_file);
    fault = 1;
    short_log->write(record);
    fault = 0;
    check(!short_log->failed() && rows(short_file).size() == 1, "EINTR和短写恢复");
    const auto partial_file = root / "partial";
    auto partial = fd_log(partial_file);
    fault = 3;
    partial->write(record);
    fault = 0;
    const auto after_failure = calls.load();
    std::vector<std::thread> later;
    for (unsigned n = 0; n < count; ++n)
        later.emplace_back([&] {
            partial->write(record);
        });
    for (auto &worker : later)
        worker.join();
    check(partial->failed() && calls.load() == after_failure
              && std::filesystem::file_size(partial_file) == 3,
        "部分写失败后不拼接后续记录");
    const int full = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
    check(full >= 0, "坏设备打开");
    auto broken = redirected(full);
    check(::close(full) == 0, "关闭原设备fd");
    broken->write(record);
    check(broken->failed(), "ENOSPC隔离");
    // 保持SIGPIPE默认动作，真实闭管道写入若未局部屏蔽会终止本测试进程。
    struct sigaction action {};
    check(::sigaction(SIGPIPE, nullptr, &action) == 0 && action.sa_handler == SIG_DFL,
        "进程未全局忽略SIGPIPE");
    int pipefd[2];
    check(::pipe(pipefd) == 0 && ::close(pipefd[0]) == 0, "真实闭管道");
    auto pipe_log = redirected(pipefd[1]);
    check(::close(pipefd[1]) == 0, "原管道fd关闭");
    pipe_log->write(record);
    sigset_t pending{}, mask{};
    check(::sigpending(&pending) == 0 && ::pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0,
        "读取线程信号状态");
    check(pipe_log->failed() && ::sigismember(&pending, SIGPIPE) == 0
              && ::sigismember(&mask, SIGPIPE) == 0,
        "EPIPE隔离并恢复线程mask");
    // 调用前已有pending必须保留，不能把它冒充本次日志产生的信号消费。
    check(::pipe(pipefd) == 0 && ::close(pipefd[0]) == 0, "第二个闭管道");
    auto pending_log = redirected(pipefd[1]);
    check(::close(pipefd[1]) == 0, "第二管道源关闭");
    sigset_t one{}, previous{};
    ::sigemptyset(&one);
    ::sigaddset(&one, SIGPIPE);
    check(::pthread_sigmask(SIG_BLOCK, &one, &previous) == 0 && ::raise(SIGPIPE) == 0,
        "测试先产生pending");
    pending_log->write(record);
    check(::sigpending(&pending) == 0 && ::sigismember(&pending, SIGPIPE) == 1
              && pending_log->failed(),
        "原pending保留");
    const timespec zero{};
    check(::sigtimedwait(&one, nullptr, &zero) == SIGPIPE
              && ::pthread_sigmask(SIG_SETMASK, &previous, nullptr) == 0,
        "测试清理自身信号");
}
void rotation_contract(const std::filesystem::path &root) {
    const auto folder = root / "rotation";
    std::filesystem::create_directory(folder);
    check(::chmod(folder.c_str(), 0700) == 0, "轮转目录由本实例独占管理");
    const auto file = folder / "history.jsonl";
    auto result = Logger::create(Level::debug, file.string(), {2048, 2});
    check(static_cast<bool>(result), "启用有界轮转");
    auto logger = std::move(result).value();
    Record record;
    // 顺序输入及独立JSON读取证明只保留连续最新后缀，而非只检查产生了归档文件。
    for (unsigned n = 0; n < 100; ++n) {
        record.request_id = n;
        logger->write(record);
    }
    check(!logger->failed(), "多轮淘汰后健康");
    std::vector<unsigned long> retained;
    for (const auto &name : {"history.2.jsonl", "history.1.jsonl", "history.jsonl"}) {
        const auto path = folder / name;
        check(std::filesystem::file_size(path) <= 2048, "每文件不越大小上限");
        struct stat metadata {};
        check(::stat(path.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0600,
            "当前及归档0600");
        for (const auto &row : rows(path))
            retained.push_back(std::stoul(row.at("request_id").get<std::string>()));
    }
    check(!retained.empty() && retained.front() > 0 && retained.back() == 99,
        "保留最新记录且淘汰旧记录");
    for (std::size_t i = 1; i < retained.size(); ++i)
        check(retained[i] == retained[i - 1] + 1, "完整连续后缀无拆行或重排");
    check(std::distance(
              std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator{})
              == 3,
        "两归档加当前文件");
    logger.reset();
    check(!Logger::create(Level::info, file.string(), {2048, 2}), "重启不认领旧文件族");

    const auto parallel = root / "parallel.jsonl";
    auto parallel_result = Logger::create(Level::debug, parallel.string(), {2048, 20});
    check(static_cast<bool>(parallel_result), "并发轮转建立");
    logger = std::move(parallel_result).value();
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t)
        workers.emplace_back([&, t] {
            Record own;
            for (unsigned n = 0; n < 8; ++n) {
                own.request_id = t * 8 + n;
                logger->write(own);
            }
        });
    for (auto &worker : workers)
        worker.join();
    check(!logger->failed(), "并发轮转健康");
    std::set<std::string> ids;
    std::size_t total = 0, archives = 0;
    for (unsigned i = 0; i <= 20; ++i) {
        const auto path = i == 0 ? parallel : root / ("parallel." + std::to_string(i) + ".jsonl");
        if (!std::filesystem::exists(path))
            continue;
        if (i > 0)
            ++archives;
        check(std::filesystem::file_size(path) <= 2048, "并发轮转文件有界");
        for (const auto &row : rows(path)) {
            ++total;
            ids.insert(row.at("request_id").get<std::string>());
        }
    }
    check(archives > 1 && ids.size() == 32 && total == 32, "未满保留范围时并发不丢不重");

    const auto collision = root / "collision.1.jsonl";
    std::ofstream(collision) << "UNCHANGED";
    check(!Logger::create(Level::info, (root / "collision.jsonl").string()), "已有归档拒绝准入");
    std::ifstream input(collision);
    std::string saved;
    input >> saved;
    check(saved == "UNCHANGED" && !std::filesystem::exists(root / "collision.jsonl"),
        "已有归档保持且不先创建当前文件");
    std::filesystem::create_symlink(collision, root / "archive.1.jsonl");
    check(!Logger::create(Level::info, (root / "archive.jsonl").string()), "拒绝归档符号链接");
    const auto insecure = root / "insecure";
    std::filesystem::create_directory(insecure);
    check(::chmod(insecure.c_str(), 0777) == 0, "构造共享可写目录");
    check(!Logger::create(Level::info, (insecure / "log").string()), "拒绝共享可写目录");
    check(::chmod(insecure.c_str(), 0700) == 0, "还原测试目录权限");
    check(!Logger::create(Level::info, {}, {2047, 3}) && !Logger::create(Level::info, {}, {2048, 0})
              && !Logger::create(Level::info, {}, {2048, 21}),
        "库接口独立拒绝非法轮转参数");

    // 建立后注入非空目录阻止rename，验证失败隔离并阻止原生sink回退截断旧当前文件。
    const auto blocked = root / "blocked.jsonl";
    auto blocked_result = Logger::create(Level::debug, blocked.string(), {2048, 2});
    check(static_cast<bool>(blocked_result), "轮转故障目标建立");
    auto broken = std::move(blocked_result).value();
    broken->write(record);
    std::filesystem::create_directory(root / "blocked.2.jsonl");
    std::ofstream(root / "blocked.2.jsonl/sentinel") << "KEEP";
    for (unsigned n = 0; n < 20 && !broken->failed(); ++n)
        broken->write(record);
    check(broken->failed() && !rows(blocked).empty(), "rename失败停用且旧当前文件未截断");
    const auto size = std::filesystem::file_size(blocked);
    broken->write(record);
    check(std::filesystem::file_size(blocked) == size
              && std::filesystem::exists(root / "blocked.2.jsonl/sentinel"),
        "失败后不再写入且障碍内容保持");
}
} // namespace
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t size) {
    ++calls;
    if (fault == 1) {
        fault = 2;
        errno = EINTR;
        return -1;
    }
    if (fault == 2)
        return __real_write(fd, data, size > 7 ? 7 : size);
    if (fault == 3) {
        fault = 4;
        return __real_write(fd, data, size > 3 ? 3 : size);
    }
    if (fault == 4) {
        errno = ENOSPC;
        return -1;
    }
    return __real_write(fd, data, size);
}
int main() {
    const auto *temp = std::getenv("TMPDIR");
    std::string pattern = std::string(temp ? temp : "/tmp") + "/siftwing-logging-XXXXXX";
    const char *made = ::mkdtemp(pattern.data());
    if (!made)
        return 1;
    const std::filesystem::path root(made);
    try {
        exercise(root);
        rotation_contract(root);
        std::filesystem::remove_all(root);
        std::cout << "日志合同通过\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "；保留=" << root << '\n';
        return 1;
    }
}
