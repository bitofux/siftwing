/*
 * PROJECT : SIFTWING
 * FILE    : search_task_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 准入/响应构造逐次分配失败的输入保留和移动无分配
 */
#include "siftwing/concurrency/completion.hpp"
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
using namespace siftwing::concurrency;
using namespace siftwing::net;
thread_local bool observe = false;
thread_local unsigned int allocations = 0, fail_at = 0;
unsigned int checks = 0;
void require(bool value, const char *label) {
    ++checks;
    if (!value) {
        std::cerr << label << '\n';
        std::exit(1);
    }
}
bool same(const Frame &a, const Frame &b) {
    return a.message_type == b.message_type && a.status == b.status && a.request_id == b.request_id
           && a.body == b.body;
}
}
// 与既有codec故障测试一致，替换可替换的全局new/delete成对使用malloc/free。
// 这能观测libstdc++字符串内部的普通new；不覆盖直接malloc、对齐分配或真实OOM。
void *operator new(std::size_t size) {
    if (observe && ++allocations == fail_at)
        throw std::bad_alloc();
    if (void *p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}
int main() {
    const Frame original{
        MessageType::search_request, 0, 77, "{\"query\":\"" + std::string(256, 'a') + "\"}"};
    Frame source = original;
    allocations = 0;
    observe = true;
    auto admitted = SearchTask::create(source, 1024);
    observe = false;
    const auto task_allocations = allocations;
    require(std::holds_alternative<SearchTask>(admitted) && task_allocations > 0,
        "observed validation allocation");
    for (unsigned int index = 1; index <= task_allocations; ++index) {
        source = original;
        allocations = 0;
        fail_at = index;
        observe = true;
        bool caught = false;
        try {
            (void)SearchTask::create(source, 1024);
        } catch (const std::bad_alloc &) {
            caught = true;
        }
        observe = false;
        fail_at = 0;
        require(caught && allocations == index && same(source, original),
            "all admission failures preserve source");
    }
    auto task = std::get<SearchTask>(std::move(admitted));
    const auto body = "{\"result\":\"" + std::string(512, 'b') + "\"}";
    for (const bool error_response : {false, true}) {
        allocations = 0;
        observe = true;
        auto result = error_response ? Completion::error(task, ErrorStatus::internal_error)
                                     : Completion::success(task, body);
        observe = false;
        const auto response_allocations = allocations;
        require(std::holds_alternative<Completion>(result) && response_allocations > 0,
            "observed response allocations");
        for (unsigned int index = 1; index <= response_allocations; ++index) {
            allocations = 0;
            fail_at = index;
            observe = true;
            bool caught = false;
            try {
                if (error_response)
                    (void)Completion::error(task, ErrorStatus::internal_error);
                else
                    (void)Completion::success(task, body);
            } catch (const std::bad_alloc &) {
                caught = true;
            }
            observe = false;
            fail_at = 0;
            require(
                caught && allocations == index && task.valid() && same(*task.request(), original),
                "all completion failures keep task intact");
        }
        auto output = std::get<Completion>(std::move(result));
        allocations = 0;
        observe = true;
        auto moved_task = std::move(task);
        auto moved_output = std::move(output);
        task = std::move(moved_task);
        output = std::move(moved_output);
        auto frame = std::move(output).take_frame();
        observe = false;
        require(allocations == 0 && task.valid() && frame.request_id == 77 && !output.valid(),
            "moves and take allocate nothing");
    }
    std::cout << "search failure checks=" << checks << " task_allocations=" << task_allocations
              << '\n';
}
