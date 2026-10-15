/*
 * PROJECT : SIFTWING
 * FILE    : versioned_cache_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 原创真实快照上的参数/预算隔离、共享worker缓存及TCP关联号
 */
#include "siftwing/net/online_server.hpp"
#include "../support/multi_reactor_fixture.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <limits>
#include <set>
namespace {
using namespace siftwing;
using namespace siftwing::net;
using namespace siftwing::test;
unsigned int checks = 0;
void check(bool ok, const char *why) {
    ++checks; if (!ok) throw std::runtime_error(why);
}
concurrency::SearchTask task(std::string body, MessageType type = MessageType::search_request,
        std::uint64_t id = 1, std::size_t cap = frame_max_body_bytes) {
    Frame frame{type, 0, id, std::move(body)};
    auto made = concurrency::SearchTask::create(frame, cap);
    if (!std::holds_alternative<concurrency::SearchTask>(made)) throw std::runtime_error("fixture frame");
    return std::get<concurrency::SearchTask>(std::move(made));
}
std::unique_ptr<query::SearchService> service(const std::string &parent, cache::LruConfig cache,
        std::uint64_t version = 0) {
    query::SearchServiceConfig cfg; cfg.parent = parent; cfg.cache = cache; cfg.version = version;
    auto made = query::SearchService::create(cfg);
    if (!made) throw std::runtime_error(made.error().context);
    return std::move(made).value();
}
std::string body(query::SearchServiceResult result) {
    if (const auto *text = std::get_if<std::string>(&result)) return *text;
    throw std::runtime_error("unexpected business error");
}
void direct(const std::string &parent, const std::string &second) {
    auto cached = service(parent, {64, 1048576, 65536});
    auto plain = service(parent, {});
    const auto query = task(R"({"query":"red"})");
    // 每调用输出不依赖全局计数差；关闭、冷、热及早期拒绝在同一输出变量上验证重置。
    query::CacheObservation observation = query::CacheObservation::hit;
    const auto expected = body(plain->execute(query, &observation));
    check(observation == query::CacheObservation::disabled && plain->index_version() == 0,
        "disabled observation and actual version");
    const auto json = nlohmann::json::parse(expected);
    std::set<std::string> ids;
    for (const auto &hit : json.at("results")) ids.insert(hit.at("document_id").get<std::string>());
    check(ids == std::set<std::string>{"0", "3"}, "independent original red document identities");
    check(body(cached->execute(query, &observation)) == expected
              && cached->cache_stats().misses == 1 && observation == query::CacheObservation::miss,
        "cold computes same Body and exact miss");
    check(body(cached->execute(query, &observation)) == expected
              && observation == query::CacheObservation::hit,
        "per-call exact hit");
    check(cached->execute(task(R"({"query":null})"), &observation)
                  == query::SearchServiceResult{ErrorStatus::invalid_request}
              && observation == query::CacheObservation::not_checked,
        "early error resets observation");
    check(
        body(cached->execute(task(
            R"({"query":"  RED\t","top_k":10,"mode":"or","summary_window":160,"summary_context":40})")))
                == expected
            && cached->cache_stats().hits == 2,
        "normalize and explicit defaults share key");
    // 每个结果参数与有效输出预算都独立；与关闭缓存路径对照，并核确实miss。
    for (const std::string request : {R"({"query":"red","mode":"and"})", R"({"query":"red","top_k":1})",
             R"({"query":"red","summary_window":100})", R"({"query":"red","summary_context":20})"}) {
        const auto before = cached->cache_stats(); const auto input = task(request);
        check(body(cached->execute(input)) == body(plain->execute(input))
            && cached->cache_stats().misses == before.misses + 1, "complete effective search parameters isolated");
    }
    auto small = task(R"({"query":"red"})", MessageType::search_request, 2, expected.size() - 1);
    check(cached->execute(small) == query::SearchServiceResult{ErrorStatus::resource_limit}
        && cached->execute(small) == plain->execute(small), "warm large Body cannot bypass small response budget");
    const auto null_query = task(R"({"query":"red\u0000"})", MessageType::search_request, 3, 128);
    check(cached->execute(null_query) == plain->execute(null_query), "header budget retains original error priority");
    const auto count = cached->cache_stats().entries;
    check(cached->execute(small) == plain->execute(small) && cached->cache_stats().entries == count, "errors never stored");
    for (const std::string invalid : {R"({"query":"red","query":"blue"})", R"({"query":"red","extra":1})",
             R"({"query":"red","top_k":0})"}) {
        const auto before = cached->cache_stats();
        check(cached->execute(task(invalid)) == query::SearchServiceResult{ErrorStatus::invalid_request}
            && cached->cache_stats().hits == before.hits && cached->cache_stats().misses == before.misses,
            "schema validation precedes warm lookup");
    }
    check(cached->execute(task(nlohmann::json{{"query", std::string(65536, ' ') + "red"}}.dump()))
        == query::SearchServiceResult{ErrorStatus::resource_limit}, "raw byte limit precedes normalized hit");
    const auto no_match = task(R"({"query":"unknown"})");
    const auto empty = body(cached->execute(no_match)); const auto before_empty = cached->cache_stats();
    check(nlohmann::json::parse(empty).at("results").empty() && body(cached->execute(no_match)) == empty
        && cached->cache_stats().hits == before_empty.hits + 1, "successful empty results cached");
    for (const std::string request : {R"({"key":"cat"})", R"({"key":"cat","top_k":1})",
             R"({"key":"cat","normalize_key":true})", R"({"key":"CAT"})"}) {
        const auto input = task(request, MessageType::suggest_request);
        const auto before = cached->cache_stats();
        check(body(cached->execute(input)) == body(plain->execute(input))
            && cached->cache_stats().misses == before.misses + 1, "suggest type and parameters isolated");
    }
    const auto before_normalized = cached->cache_stats();
    const auto normalized = body(cached->execute(task(R"({"key":" CAT ","normalize_key":true})", MessageType::suggest_request)));
    check(nlohmann::json::parse(normalized).at("key") == "cat"
        && cached->cache_stats().hits == before_normalized.hits + 1, "suggest optional normalization same effective text");
    const auto before_type = cached->cache_stats();
    const auto search_cat = nlohmann::json::parse(body(cached->execute(task(R"({"query":"cat"})"))));
    check(search_cat.at("command") == "SEARCH"
        && cached->cache_stats().misses == before_type.misses + 1, "SEARCH cannot return SUGGEST Body");
    auto tiny = service(parent, {2, 4096, 16});
    check(body(tiny->execute(query)) == expected && body(tiny->execute(query)) == expected
        && tiny->cache_stats().entries == 0 && tiny->cache_stats().misses == 2, "large success bypass keeps business success");
    auto other = service(second, {2, 4096, 1024}, std::numeric_limits<std::uint64_t>::max());
    const auto newest = body(other->execute(query));
    check(nlohmann::json::parse(newest).at("snapshot_version") == "18446744073709551615"
        && newest != expected && body(other->execute(query)) == newest, "real max version separate snapshot");
    std::atomic<bool> wrong{false}; std::vector<std::thread> threads;
    const auto start = std::chrono::steady_clock::now();
    const auto before_threads = cached->cache_stats();
    for (unsigned int n = 0; n < 4; ++n) threads.emplace_back([&] {
        for (unsigned int i = 0; i < 100; ++i) if (body(cached->execute(query)) != expected) wrong = true;
    });
    for (auto &t : threads) t.join();
    check(!wrong && cached->cache_stats().hits == before_threads.hits + 400, "concurrent service warm hits exact");
    std::cout << "measurement warm4x100_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    // 小型测量只记录当前原创小语料的耗时，不设速度门槛，不推出生产吞吐结论。
    for (auto *selected : {plain.get(), cached.get()}) {
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned int i = 0; i < 200; ++i) check(body(selected->execute(query)) == expected, "measurement equality");
        std::cout << (selected == plain.get() ? " disabled200_us=" : " warm200_us=")
            << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
    }
    std::cout << '\n';
    query::SearchServiceConfig invalid; invalid.parent = parent; invalid.cache.capacity = 4097;
    const auto rejected = query::SearchService::create(invalid);
    check(!rejected && rejected.error().context == "service.cache_config", "service prechecks invalid cache config");
}
std::string response(int fd, std::uint64_t id) {
    const auto head = receive(fd, 24); std::size_t size = 0;
    for (std::size_t i = 12; i < 16; ++i) size = size * 256 + static_cast<unsigned char>(head[i]);
    check(size <= frame_max_body_bytes, "bounded response length");
    const auto bytes = receive(fd, size);
    check(head + bytes == wire(0x8003, id, bytes), "fresh request_id in completion response");
    return bytes;
}
void tcp(const std::string &parent, std::size_t subs) {
    const auto descriptors = fd_count();
    {
        auto loaded = service(parent, {8, 65536, 4096}); const auto *stats = loaded.get();
        OnlineServerConfig config; config.sub_count = subs;
        auto made = OnlineServer::create(std::move(loaded), config);
        if (!std::holds_alternative<std::unique_ptr<OnlineServer>>(made)) throw std::runtime_error("server create");
        auto server = std::get<std::unique_ptr<OnlineServer>>(std::move(made));
        std::string first;
        for (std::size_t i = 0; i < 4; ++i) {
            auto peer = client(server->port());
            await([&] { if (auto e = server->run_once(0)) throw std::system_error(e);
                return server->report().handed_off == i + 1; });
            send_all(peer.get(), wire(3, 17 + i, R"({"query":"red"})") + wire(1, 90 + i));
            check(::shutdown(peer.get(), SHUT_WR) == 0, "half close after cached pipeline");
            const auto output = response(peer.get(), 17 + i);
            if (i == 0) first = output;
            check(first == output && receive(peer.get(), 26) == wire(0x8001, 90 + i), "real worker Body and next PING");
        }
        check(stats->cache_stats().hits == 3 && stats->cache_stats().misses == 1, "cross worker and Sub shared service cache");
        check(!server->stop() && server->report().workers.succeeded == 4, "cached jobs still complete through worker barrier");
    }
    check(fd_count() == descriptors, "cached server releases descriptors");
}
}
int main(int argc, char **argv) {
    try {
        if (argc != 4) throw std::runtime_error("parent second-parent subs required");
        direct(argv[1], argv[2]); tcp(argv[1], std::string(argv[3]) == "1" ? 1U : 2U);
        std::cout << "versioned service checks=" << checks << '\n';
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
