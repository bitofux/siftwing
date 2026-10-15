/*
 * PROJECT : SIFTWING
 * FILE    : search_service.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 固定只读快照与查询资源的在线SEARCH/SUGGEST服务
 */
#pragma once
#include "siftwing/concurrency/search_task.hpp"
#include "siftwing/base/result.hpp"
#include "siftwing/cache/versioned_lru.hpp"
#include <memory>

namespace siftwing::query {
/** @brief 启动时取得拥有配置；预算仅可降低profile1上限，不是RSS/CPU硬限。 */
struct SearchServiceConfig {
    std::string parent; ///< 完整快照父目录；启动期间稳定、只读，不解释CURRENT。
    std::uint64_t version = 0; ///< 显式快照版本，完整uint64均合法。
    std::string tokenizer = "english"; ///< english或cppjieba；须与搜索建库相容。
    std::string stopwords, dictionary, model, user; ///< 可选停用词与显式中文资源路径；启动装载。
    bool hmm = true; ///< cppjieba固定HMM选择，英文不使用。
    std::uint64_t query_bytes = 65536; ///< 正上限<=65536；原查询/规范化文本分别核验。
    std::uint64_t max_k = 100; ///< 正上限<=100；默认请求10也必须合法，不自动缩小。
    std::uint64_t work = 50000000; ///< 正上限<=50000000；评分/显示/推荐各自计量。
    std::uint64_t output_bytes = net::frame_max_body_bytes; ///< 正上限<=1MiB，实际再取task Body预算。
    std::uint64_t snapshot_bytes = 512U * 1024U * 1024U; ///< 正单payload/文档库预算<=512MiB。
    std::uint64_t resource_bytes = 16U * 1024U * 1024U; ///< 正单资源装载预算<=16MiB。
    cache::LruConfig cache; ///< 可选实例LRU，默认capacity=0关闭；固定快照/资源/预算不跨实例共享。
};
/** @brief 成功拥有完整JSON对象Body（无LF），失败为六种线上安全状态之一，无部分结果。 */
using SearchServiceResult = std::variant<std::string, net::ErrorStatus>;
/**
 * @brief 完整Reader与固定分词/停用资源的拥有者；查询只读内存，不访问socket或文件
 * @note 不可copy/move；同实例execute可并发，配置、Reader及初始化后的分词资源只读，
 *     中间结果为调用局部，可选LRU独立mutex保护，业务计算不持缓存锁。
 *     调用者保证到最后调用/worker join返回才销毁；不认证建库资源相容、不热切换、不取消。
 *     既有业务结果/错误和V1 CLI接口不变，缓存只拥有成功JSON Body，无任务/连接借用。
 */
class SearchService final {
public:
    /**
     * @brief 预检配置、完整装载快照及资源，发布前重核资源SHA
     * @param[in] config
     *     按值取得路径/资源与正预算；启动期间文件不得并发修改，逐组件拒符号链接。
     *
     * @return
     *     非空拥有服务或本地Error诊断；失败不开放监听，不发布部分服务。
     * @throws
     *     分配及未知初始化异常传播，局部RAII释放；文件SHA不是真实性认证。
     */
    [[nodiscard]] static base::Result<std::unique_ptr<SearchService>> create(SearchServiceConfig config);
    /** @brief 无在途调用时释放完整快照和固定资源。 */
    ~SearchService() noexcept;
    SearchService(const SearchService &) = delete;
    SearchService &operator=(const SearchService &) = delete;
    SearchService(SearchService &&) = delete;
    SearchService &operator=(SearchService &&) = delete;
    /**
     * @brief 校验严格业务schema，再复用V1查询、推荐、摘要和高亮生成有界JSON
     * @param[in] task
     *     同步稳定借用已准入拥有请求；移空拒INVALID_REQUEST，不消费或保存其借用。
     *
     * @return
     *     SEARCH/SUGGEST成功Body或INVALID_REQUEST/RESOURCE_LIMIT/INTERNAL_ERROR；空results成功。
     *     拒未知/重复键、错误类型；uint64身份为十进制字符串，不暴露异常文本。
     * @throws
     *     分配与未知异常传播；worker负责固定安全错误或报告无法交付的服务首因。
     */
    [[nodiscard]] SearchServiceResult execute(const concurrency::SearchTask &task) const;
    /** @brief 可与execute并发取得缓存统计值快照；锁异常传播，无文件/socket操作。 */
    [[nodiscard]] cache::LruStats cache_stats() const;
private:
    struct Impl;
    explicit SearchService(std::unique_ptr<Impl> state) noexcept;
    std::unique_ptr<Impl> state_; ///< 独占固定只读查询资源与同步缓存，无客户端资源；覆盖全部execute。
};
} // namespace siftwing::query
