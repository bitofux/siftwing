/*
 * PROJECT : SIFTWING
 * FILE    : reader.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 从正式版本目录完整校验并加载只读拥有快照
 */
#pragma once
#include "siftwing/document/document_store.h"
#include "siftwing/index/weighted_index.h"
#include "siftwing/snapshot/manifest.h"
#include <memory>

namespace siftwing::snapshot {
/** @brief 26项正预算；含端点，限制字节/统计而非进程内存或CPU硬上限。 */
struct SnapshotReadLimits final {
    document::DocumentStoreLimits documents; ///< 记录、库与偏移表预算。
    ManifestLimits manifest; ///< 清单、单payload和六payload总预算；不含COMPLETE。
    index::InvertedIndexLimits search; ///< 原TF/词表/文档/关系预算，不采用推荐统计。
    index::RecommendationDictionaryLimits recommendation; ///< 独立推荐及字符关系预算。
};
/**
 * @brief 完整加载后只读拥有manifest、文档、搜索与独立推荐；没有长期fd或输入借用
 * @note move-only/noexcept转交，源为空，自移动保持。getter只允许左值，移空返回nullptr；
 *     借用在移动/赋值/析构后失效，不延长生命，不与这些动作并发。只读无可变缓存，
 *     线程调用仍要求所有者稳定；未实测并发。加载完成后不再访问原文件。
 */
class SnapshotReader final {
public:
    /** @brief 禁止复制完整拥有状态，调用者通过移动转交读者。 */
    SnapshotReader(const SnapshotReader &) = delete;
    /** @brief 禁止复制赋值，避免隐式复制全库和部件。 */
    SnapshotReader &operator=(const SnapshotReader &) = delete;
    /** @brief 转交唯一State，源为空；目标部件身份及数据保持。 */
    SnapshotReader(SnapshotReader &&other) noexcept;
    /** @brief 释放原State后转交，源为空，自移动保持。 */
    SnapshotReader &operator=(SnapshotReader &&other) noexcept;
    /** @brief 释放拥有记录及部件；加载结束后没有fd需要关闭。 */
    ~SnapshotReader();
    /**
     * @brief 完整校验正式八文件集合，再一次返回拥有型读者
     * @param[in] parent_path
     *     已存在可信稳定父目录；非空/无NUL/无..，逐组件拒链接，允许相对/绝对/点及斜杠。
     *     调用期间目录命名/内容不得并发改写；不提供敌对写者或网络FS下的原子读取保证。
     * @param[in] expected_version
     *     仅打开其规范十进制叶子，0/UINT64_MAX合法；manifest版本身份必须相同。
     *     不扫描最新版本、接受临时叶子或解释CURRENT。
     * @param[in] limits
     *     全正配置优先；尺寸/元数据预算先于载荷分配，解析不按不可信计数reserve。
     *     核manifest/COMPLETE、六SHA与尺寸、记录/偏移/ID集合、原TF/DF/N/权重及推荐字符。
     * @return
     *     完整对象或reader.limits/path/open/stat/directory/bytes/read/close/version/syntax/
     *     semantic及复用组件诊断；失败无部分读者，任何路径均关闭局部fd且close不重试。
     * @throws 分配与未知异常上传，RAII释放局部资源，不写入/删除/修改任何输入。
     * @note 权重由原整数TF重建并规范重导出逐字节核对；当前CPU/libm须复现保存的策略1
     *     binary64字节，FE_TONEAREST，无静默数值迁移。哈希不是认证，也不证明TF来自原文
     *     或来源/config声明真实。全内存加载，不提供查询、热切换、mmap或断电恢复验证。
     */
    [[nodiscard]] static base::Result<SnapshotReader> open(const std::string &parent_path,
        base::SnapshotVersion expected_version, const SnapshotReadLimits &limits);
    /** @brief 借用完整清单；移空nullptr，失效规则同类合同。 */
    [[nodiscard]] const Manifest *manifest() const & noexcept;
    const Manifest *manifest() const && = delete;
    /** @brief 借用有效搜索部件；其TermId只属于本搜索词典，移空nullptr。 */
    [[nodiscard]] const index::WeightedIndex *search() const & noexcept;
    const index::WeightedIndex *search() const && = delete;
    /** @brief 借用独立推荐部件；不能混用搜索词ID，移空nullptr。 */
    [[nodiscard]] const index::RecommendationDictionary *recommendation() const & noexcept;
    const index::RecommendationDictionary *recommendation() const && = delete;
    /** @brief 借用按DocId升序完整记录；合法空快照返回非空指针指向空vector，移空nullptr。 */
    [[nodiscard]] const std::vector<document::DocumentRecord> *documents() const & noexcept;
    const std::vector<document::DocumentRecord> *documents() const && = delete;
    /** @brief 按身份二分借用记录，未知ID或移空nullptr；不将DocId当下标。 */
    [[nodiscard]] const document::DocumentRecord *document(base::DocumentId id) const & noexcept;
    const document::DocumentRecord *document(base::DocumentId) const && = delete;
private:
    struct State;
    explicit SnapshotReader(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_; ///< 唯一拥有已验证完整状态，未发布局部状态不会逃逸。
};
} // namespace siftwing::snapshot
