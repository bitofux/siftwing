/*
 * PROJECT : SIFTWING
 * FILE    : service_info_source.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : Sub可稳定借用的只读服务元信息来源
 */
#pragma once
#include <cstdint>
#include <string>

namespace siftwing::net {
/**
 * @brief 提供按次一致的公开服务信息，不访问文件/socket；所有者覆盖Sub最后调用与join
 * @note 各const方法可并发，内部同步由实现负责；两个独立方法不承诺同一版本。
 */
class ServiceInfoSource {
public:
    virtual ~ServiceInfoSource() = default;
    /** @brief 返回完整拥有JSON Body；单个Body内版本/构建字段一致，分配等异常传播。 */
    [[nodiscard]] virtual std::string server_info() const = 0;
    /** @brief 取得观察时点的当前版本；不表示某个排队/在途查询实际使用的版本。 */
    [[nodiscard]] virtual std::uint64_t index_version() const noexcept = 0;
};
} // namespace siftwing::net
