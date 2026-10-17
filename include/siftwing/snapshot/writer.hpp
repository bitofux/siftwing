/*
 * PROJECT : SIFTWING
 * FILE    : writer.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 将独立搜索和推荐部件写为完整快照并原子无覆盖发布
 */
#pragma once
#include "siftwing/document/document_store.hpp"
#include "siftwing/index/weighted_index.hpp"
#include "siftwing/snapshot/manifest.hpp"

namespace siftwing::snapshot {
/** @brief 调用方拥有的身份/来源声明；不读取来源文件或证明配置真实性。 */
struct SnapshotMetadata final {
    base::SnapshotVersion snapshot; ///< 不透明版本，0/UINT64_MAX合法；正式叶子为规范十进制。
    std::string build_version;    ///< 拥有稳定构建身份，沿用Manifest的ASCII合同。
    Provenance search_provenance; ///< 搜索输入/config的独立摘要声明。
    Provenance recommendation_provenance; ///< 推荐自己的输入/config声明，不隐式复用搜索。
};
/** @brief 八项正预算，含端点；复用单记录/库/偏移和清单预算，不是硬进程内存限制。 */
struct SnapshotWriteLimits final {
    document::DocumentStoreLimits documents; ///< 两记录预算及文档数、库字节、偏移表字节预算。
    ManifestLimits manifest; ///< 清单字节、单payload字节、六payload总字节，不含COMPLETE。
};
/** @brief rename已成功后，父目录同步/关闭是否也成功；不等于断电恢复验证。 */
enum class PublicationDurability {
    synced,
    unconfirmed
};
/**
 * @brief 正式目录已经发布的拥有型结果；无fd或内部借用
 * @note 函数产生的synced结果system_errno=0，unconfirmed保存第一次父目录fsync/close错误。
 *     后者仍是已发布结果，不应盲目重试同版本或删除正式目录。复制独立，默认移动后
 *     成员有效但内容未指定。该普通值可修改，不提供目录锁或真实性保证。
 */
struct PublishedSnapshot final {
    Manifest manifest; ///< 拥有实际发布的完整文件声明；目录叶子可从snapshot生成。
    PublicationDurability durability; ///< 同步完成或持久性未确认，二者都已发布。
    int system_errno; ///< 原始Linux errno值，可用generic_category解释；不是跨平台业务码。
};
/**
 * @brief 编码、写入/回读完整八文件集合，再无覆盖原子发布一个新版本
 * @param[in] parent_path
 *     已存在可信输出父目录，非空/无NUL/无..组件；逐组件openat拒绝符号链接，允许
 *     绝对/相对路径及多余斜杠/点组件。不创建父目录，不从来源路径派生输出。
 *     父目录命名和本次临时内容在调用期间不得被外部改写；不承诺敌对写者安全。
 * @param[in] documents
 *     同步稳定借用的显式搜索保留记录，ID集合须与search完整一致，重复拒绝；按ID
 *     升序编码库/偏移，不改输入顺序。记录字节合同沿用encode_record，不重做分词/去重。
 * @param[in] search
 *     稳定借用有效WeightedIndex；生产规范搜索词表/原TF/DF/N/权重，不验证TF对应原文。
 * @param[in] recommendation
 *     稳定借用独立有效推荐词典，按自己的交付数/频次/标量关系编码，不按搜索去重。
 * @param[in] metadata
 *     同步借用身份和独立来源声明，不保存输入引用。
 * @param[in] limits
 *     先全正配置/路径词法，再文档数与ID集合、编码/受检字节预算和manifest，后文件IO。
 *
 * @return
 *     success表示rename已发布；检查durability，unconfirmed保留正式目录并给出errno。
 *     failure只发生在rename成功前，不覆盖任何已有对象；诊断为writer.limits/path/
 *     documents/identity/bytes/exists/random/mkdir/open/stat/write/sync/close/read/directory/
 *     rename或复用的record/store/weights/manifest上下文。本次自建失败临时目录保留，
 *     诊断message含其叶子；不会自动清理调用方目录。所有局部fd均RAII关闭，close不重试。
 * @throws
 *     分配及未知异常在发布前上传，RAII关闭fd，自建临时内容仍保留。返回结果在rename前
 *     构造完毕，rename成功后不分配，仅系统调用/标量标记及noexcept移动，不抛分配异常。
 * @note 临时叶子.siftwing-<版本>-<128位随机hex>.tmp，目录0700/文件0600（受umask收紧）；
 *     六payload分别写入/fsync/检查close后重新打开回读，再写manifest/COMPLETE并回读；
 *     核八普通文件、同步/关闭临时目录，Linux renameat2(RENAME_NOREPLACE)，最后父同步/关闭。
 *     无该内核/文件系统能力就拒绝，无普通rename降级；短IO/EINTR重试，零写/提前EOF失败。
 *     无网络、后台线程或CURRENT指针；不提供Reader、在线切换或跨文件系统发布。
 */
[[nodiscard]] base::Result<PublishedSnapshot> write_snapshot(const std::string &parent_path,
    const std::vector<document::DocumentRecord> &documents,
    const index::WeightedIndex &search,
    const index::RecommendationDictionary &recommendation,
    const SnapshotMetadata &metadata,
    const SnapshotWriteLimits &limits);
} // namespace siftwing::snapshot
