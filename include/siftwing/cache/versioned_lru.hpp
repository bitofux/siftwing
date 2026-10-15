/*
 * PROJECT : SIFTWING
 * FILE    : versioned_lru.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 精确版本键、拥有型结果与线程安全的有界LRU
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace siftwing::cache {
/** @brief 缓存业务域；搜索与推荐永不共用同一个键。 */
enum class Operation {
    search,
    suggest
};
/** @brief 拥有精确键；不拼接分隔符，不保存请求关联号或客户端身份。 */
struct CacheKey {
    std::uint64_t index_version = 0;         ///< 完整快照版本，0与UINT64_MAX均合法。
    Operation operation = Operation::search; ///< 业务域，由调用者按请求类型选择。
    std::string query; ///< 调用者按固定策略规范化的拥有文本；支持空串及内嵌NUL。
    std::string parameters; ///< 无歧义编码的全部结果/预算参数，资源策略须由实例隔离或纳入此键。
    /** @brief 精确元组序，与hash/字符串分隔符无关；不分配、不修改源。 */
    [[nodiscard]] bool operator<(const CacheKey &other) const noexcept;
};
/** @brief 固定容量；字节只计两份键字符串与Body长度，元数据/容量/分配器开销另计。 */
struct LruConfig {
    std::size_t capacity = 0; ///< [0,4096]；0禁用，get仍计miss，put返回false。
    std::size_t bytes = 8U * 1024U * 1024U; ///< 正预算<=64MiB，驻留charge不得超过此值。
    std::size_t max_value_bytes = 256U * 1024U; ///< 正预算<=1MiB且<=bytes，等号可存。
};
/** @brief 同一次锁内取得的值快照；uint64累计计数饱和，不回绕。 */
struct LruStats {
    std::uint64_t hits = 0, misses = 0, evictions = 0; ///< 成功get、未命中get、容量/字节真实淘汰。
    std::size_t entries = 0, bytes = 0; ///< 当前驻留条目与charge，不含返回副本或并发在途结果。
};
/**
 * @brief mutex保护精确键map与最近使用list，查找O(log capacity)，单次可能淘汰capacity项
 * @note 不可copy/move；所有方法可并发，构造/析构须无并行调用。无回调或外部借用。
 *     put/get分配可抛bad_alloc并保持内容、最近使用顺序和统计；锁设施异常传播。
 *     不验证业务JSON/文本，不提供TTL、持久化、single-flight或RSS硬限。
 */
class VersionedLru final {
public:
    /**
     * @brief 固定预算并建立空缓存
     * @param[in] config
     *     按值取得；即使capacity=0也校验字节配置。
     *
     * @throws
     *     非法配置invalid_argument；分配/同步初始化异常传播。
     */
    explicit VersionedLru(LruConfig config = {});
    ~VersionedLru() noexcept;
    VersionedLru(const VersionedLru &) = delete;
    VersionedLru &operator=(const VersionedLru &) = delete;
    VersionedLru(VersionedLru &&) = delete;
    VersionedLru &operator=(VersionedLru &&) = delete;
    /**
     * @brief 命中复制拥有Body并提升MRU；未命中返回nullopt
     * @param[in] key
     *     同步稳定借用，函数返回后不保存此借用。
     *
     * @return
     *     独立string，淘汰/缓存销毁后仍有效；空Body也可命中。
     * @throws
     *     复制/锁异常传播，复制失败不计hit也不改变顺序。
     */
    [[nodiscard]] std::optional<std::string> get(const CacheKey &key);
    /**
     * @brief 原子插入/替换并提升MRU；成功后按LRU真实淘汰至预算内
     * @param[in] key
     *     稳定借用，成功复制拥有；失败不消费源。
     * @param[in] value
     *     稳定借用完整Body，支持空串/NUL，不验证JSON。
     *
     * @return
     *     true已存；禁用/单Body或总charge超限false，旧同键及其他状态保持。
     * @throws
     *     分配/锁异常传播，全部潜在分配先于替换、统计及淘汰提交。
     */
    [[nodiscard]] bool put(const CacheKey &key, std::string_view value);
    /** @brief 同步值快照，不改变LRU；锁设施异常传播。 */
    [[nodiscard]] LruStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> state_; ///< 独占容器与mutex；生命须覆盖全部并发调用。
};
} // namespace siftwing::cache
