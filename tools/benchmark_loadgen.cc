/*
 * PROJECT : SIFTWING
 * FILE    : benchmark_loadgen.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 独立TCP闭环负载发生器，完整响应准入后才记录成功与纳秒延迟
 * IMPLEMENTATION : 每线程独占连接/样本，屏障外建连预热；最后请求排空计入窗口。
 */
#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
using nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Peer {
    int fd = -1;
    ~Peer() {
        if (fd >= 0)
            ::close(fd);
    }
};
void transfer(int fd, char *data, std::size_t n, bool write, Clock::time_point deadline) {
    while (n) {
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (ms <= 0)
            throw std::runtime_error("timeout");
        pollfd p{fd, static_cast<short>(write ? POLLOUT : POLLIN), 0};
        const int rc = ::poll(&p, 1, static_cast<int>(ms));
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0)
            throw std::runtime_error(rc == 0 ? "timeout" : "poll");
        const auto moved = write ? ::send(fd, data, n, MSG_NOSIGNAL | MSG_DONTWAIT)
                                 : ::recv(fd, data, n, MSG_DONTWAIT);
        if (moved < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (moved <= 0)
            throw std::runtime_error("transport");
        const auto used = static_cast<std::size_t>(moved);
        data += used;
        n -= used;
    }
}
void put(std::string &s, std::uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 255U));
}
std::uint64_t get(const std::array<char, 24> &h, std::size_t off, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i)
        v = (v << 8U) | static_cast<unsigned char>(h[off + i]);
    return v;
}
std::string exchange(int fd,
    int kind,
    std::uint64_t id,
    const json &body,
    std::uint64_t &bytes,
    Clock::time_point deadline) {
    std::string data = "SFTW";
    data.push_back(1);
    data.push_back(24);
    const auto raw = body.dump();
    put(data, static_cast<std::uint64_t>(kind), 2);
    put(data, 0, 4);
    put(data, raw.size(), 4);
    put(data, id, 8);
    data += raw;
    transfer(fd, data.data(), data.size(), true, deadline);
    std::array<char, 24> h{};
    transfer(fd, h.data(), h.size(), false, deadline);
    if (std::string(h.data(), 4) != "SFTW" || get(h, 4, 1) != 1 || get(h, 5, 1) != 24
        || get(h, 8, 2) != 0 || get(h, 16, 8) != id)
        throw std::runtime_error("header");
    const auto length = get(h, 12, 4), type = get(h, 6, 2), status = get(h, 10, 2);
    if (length < 2 || length > 1048576)
        throw std::runtime_error("length");
    std::string answer(static_cast<std::size_t>(length), '\0');
    transfer(fd, answer.data(), answer.size(), false, deadline);
    if (type == 65535)
        throw std::runtime_error("remote_status_" + std::to_string(status));
    if (type != static_cast<std::uint64_t>(0x8000 + kind) || status != 0)
        throw std::runtime_error("type_status");
    // JSON解析检查UTF-8、语法；黄金Body的逐字节对照同时校验完整结果内容。
    const auto parsed = json::parse(answer);
    if (!parsed.is_object())
        throw std::runtime_error("json_object");
    if (kind == 1 && answer != "{}")
        throw std::runtime_error("pong");
    if (kind != 1 && (parsed.at("schema_version") != 1 || parsed.at("snapshot_version") != "1"))
        throw std::runtime_error("schema_version");
    if (kind == 2 && (parsed.at("command") != "SERVER_INFO" || parsed.at("protocol_version") != 1))
        throw std::runtime_error("info");
    if (kind == 3 || kind == 4) {
        if (parsed.at("command") != (kind == 3 ? "SEARCH" : "SUGGEST")
            || !parsed.at("results").is_array())
            throw std::runtime_error("business_schema");
    }
    if (Clock::now() >= deadline)
        throw std::runtime_error("timeout_admission");
    bytes += length + 24;
    return answer;
}
struct Result {
    std::vector<std::int64_t> latencies;
    std::map<std::string, std::size_t> errors;
    std::uint64_t bytes = 0, attempts = 0;
    double end = 0;
    std::string setup_error;
};
int main(int argc, char **argv) {
    // 参数仅由外部驱动提供：端口 并发 秒数 每线程固定请求数(0=时长) 预热 spec.json。
    if (argc != 7)
        return 2;
    try {
        const auto port = std::stoi(argv[1]), concurrency = std::stoi(argv[2]);
        const double seconds = std::stod(argv[3]);
        const auto count = std::stoull(argv[4]);
        const auto warmup = std::stoull(argv[5]);
        if (port <= 0 || port > 65535 || concurrency < 1 || concurrency > 128
            || !std::isfinite(seconds) || seconds <= 0 || seconds > 60 || count > 1000000
            || warmup > 4096)
            return 2;
        std::ifstream file(argv[6]);
        json spec;
        file >> spec;
        const int kind = spec.at("kind");
        const auto &bodies = spec.at("bodies"), &expected = spec.at("expected");
        if ((kind != 1 && kind != 3 && kind != 4) || !bodies.is_array() || bodies.empty()
            || bodies.size() > 4096 || !expected.is_array() || expected.size() != bodies.size())
            throw std::invalid_argument("bounded nonempty reference spec");
        std::vector<Result> results(static_cast<std::size_t>(concurrency));
        std::mutex mutex;
        std::condition_variable cv;
        int ready = 0;
        bool go = false, cancelled = false;
        Clock::time_point begin, deadline;
        std::vector<std::thread> threads;
        try {
            for (int w = 0; w < concurrency; ++w)
                threads.emplace_back([&, w] {
                    auto &r = results[static_cast<std::size_t>(w)];
                    Peer peer;
                    std::uint64_t id = 1, ignored = 0;
                    const auto request = [&](std::uint64_t sequence, bool measured) {
                        const auto index =
                            (sequence + static_cast<std::uint64_t>(w)) % bodies.size();
                        const auto deadline_for_request = Clock::now() + std::chrono::seconds(5);
                        std::uint64_t transferred = 0;
                        const auto answer = exchange(peer.fd,
                            kind,
                            id++,
                            bodies.at(index),
                            transferred,
                            deadline_for_request);
                        if (answer != expected.at(index).get<std::string>())
                            throw std::runtime_error("reference_body");
                        if (Clock::now() >= deadline_for_request)
                            throw std::runtime_error("timeout_admission");
                        (measured ? r.bytes : ignored) += transferred;
                    };
                    try {
                        peer.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
                        sockaddr_in address{};
                        address.sin_family = AF_INET;
                        address.sin_port = htons(static_cast<std::uint16_t>(port));
                        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                        if (peer.fd < 0
                            || ::connect(
                                peer.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
                            throw std::runtime_error("connect");
                        const auto info = json::parse(exchange(peer.fd,
                            2,
                            id++,
                            json::object(),
                            ignored,
                            Clock::now() + std::chrono::seconds(5)));
                        if (info.at("build_version") != spec.at("build_version"))
                            throw std::runtime_error("build_identity");
                        for (std::uint64_t i = 0; i < warmup; ++i)
                            request(i, false);
                    } catch (const json::exception &) {
                        r.setup_error = "json_validation";
                    } catch (const std::exception &e) {
                        r.setup_error = e.what();
                    }
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        ++ready;
                        cv.notify_all();
                        cv.wait(lock, [&] {
                            return go;
                        });
                    }
                    if (r.setup_error.empty() && !cancelled) {
                        for (std::uint64_t i = 0; count ? i < count : Clock::now() < deadline;
                             ++i) {
                            ++r.attempts;
                            const auto start = Clock::now();
                            try {
                                request(i, true);
                                r.latencies.push_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        Clock::now() - start)
                                        .count());
                            } catch (const json::exception &) {
                                ++r.errors["json_validation"];
                                break;
                            } catch (const std::exception &e) {
                                ++r.errors[e.what()];
                                break;
                            }
                        }
                    }
                    r.end = std::chrono::duration<double>(Clock::now() - begin).count();
                });
        } catch (...) {
            // 部分线程创建失败也必须解除已创建线程的门禁，join后再报告主线程错误。
            {
                std::lock_guard<std::mutex> lock(mutex);
                begin = Clock::now();
                cancelled = true;
                go = true;
            }
            cv.notify_all();
            for (auto &thread : threads)
                thread.join();
            throw;
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] {
                return ready == concurrency;
            });
            begin = Clock::now();
            deadline = begin
                       + std::chrono::duration_cast<Clock::duration>(
                           std::chrono::duration<double>(seconds));
            go = true;
        }
        cv.notify_all();
        for (auto &thread : threads)
            thread.join();
        json raw = json::array(), errors = json::object(), setup = json::array();
        std::vector<std::int64_t> all;
        std::uint64_t bytes = 0, attempts = 0;
        double elapsed = 0;
        for (const auto &r : results) {
            raw.push_back(r.latencies);
            all.insert(all.end(), r.latencies.begin(), r.latencies.end());
            for (const auto &e : r.errors)
                errors[e.first] = errors.value(e.first, std::size_t{0}) + e.second;
            if (!r.setup_error.empty())
                setup.push_back(r.setup_error);
            elapsed = std::max(elapsed, r.end);
            bytes += r.bytes;
            attempts += r.attempts;
        }
        std::sort(all.begin(), all.end());
        json quantiles = json::object();
        if (!all.empty())
            for (const auto &p : std::vector<std::pair<std::string, double>>{
                     {"p50", .5}, {"p95", .95}, {"p99", .99}}) {
                const auto index =
                    static_cast<std::size_t>(std::ceil(p.second * static_cast<double>(all.size())))
                    - 1;
                quantiles[p.first] = static_cast<double>(all[index]) / 1e6;
            }
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        const auto secs = [](timeval v) {
            return static_cast<double>(v.tv_sec) + static_cast<double>(v.tv_usec) / 1e6;
        };
        std::cout << json{{"connections", concurrency},
            {"target_seconds", seconds},
            {"fixed_per_thread", count},
            {"warmup_per_thread", warmup},
            {"elapsed_seconds", elapsed},
            {"attempts", attempts},
            {"successes", all.size()},
            {"successful_qps", static_cast<double>(all.size()) / elapsed},
            {"latency_ms", quantiles},
            {"errors", errors},
            {"setup_errors", setup},
            {"response_wire_bytes", bytes},
            {"latency_ns_by_thread", raw},
            {"client_cpu_seconds", secs(usage.ru_utime) + secs(usage.ru_stime)},
            {"client_peak_rss_kib", usage.ru_maxrss}}
                         .dump()
                  << '\n';
        return errors.empty() && setup.empty() ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 3;
    }
}
