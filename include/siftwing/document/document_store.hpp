/*
 * PROJECT : SIFTWING
 * FILE    : document_store.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 定义拥有型偏移表、持久化字节编码与只读文件随机读取合同
 */
#pragma once
#include "siftwing/document/record_format.hpp"

#include <vector>

namespace siftwing::document {

/** @brief 偏移表格式版本，与记录 v1 和未来快照版本分别维护。 */
inline constexpr std::uint16_t offset_table_version = 1;

/** @brief 全部预算必须为正；容器开销和进程资源不属于这些字节预算。 */
struct DocumentStoreLimits final {
    /** 复用已封板单字段与完整记录预算，不改变文本或编码语义。 */
    RecordFormatLimits record;
    /** 最大表项数；空库仍合法，不需要分配到最大值。 */
    std::uint64_t max_documents;
    /** 完整记录库最大字节数，不包含偏移表。 */
    std::uint64_t max_library_bytes;
    /** 完整偏移表最大字节数，包含 32 字节头和每项 24 字节。 */
    std::uint64_t max_table_bytes;
};

/** @brief 明确身份对应的文件字节区间，不将 DocId 当数组下标。 */
struct OffsetEntry final {
    /** 显式文档身份；0 与 UINT64_MAX 均合法。 */
    base::DocumentId doc_id;
    /** 从记录库起点算起的 uint64 字节偏移。 */
    std::uint64_t offset;
    /** 完整记录字节数，包含记录头/字段前缀/载荷，至少 88。 */
    std::uint64_t length;
};

/**
 * @brief 拥有型可修改表；构造/修改不自动证明有效，消费函数重新验证
 *
 * @note 有效表按 DocId 严格升序，身份唯一；条目物理顺序可不同，但字节区间必须从 0
 *     无洞无重叠地覆盖 library_bytes。空表仅能对应 0 字节库。
 * @note 复制独立拥有，默认移动后遵守成员有效但内容未指定状态，不保证仍是有效表。
 */
struct OffsetTable final {
    /** 拥有按身份排序的表项，不借用输入字节或文件。 */
    std::vector<OffsetEntry> entries;
    /** 完整记录库字节数；不包含表编码，不证明文件真实性或不可变性。 */
    std::uint64_t library_bytes = 0;
};

/**
 * @brief 验证预算、编码容量、身份顺序和完整物理覆盖，不访问文件
 *
 * @param[in] table
 *     调用期间稳定的借用表，不改写其顺序。
 * @param[in] limits
 *     全部正预算；等于上限合法。
 *
 * @return
 *     成功为无载荷 Result，失败拥有 store.limits/table.length/table.range/table.order 诊断。
 * @throws
 *     临时排序指针表及诊断的分配异常上传；无进程硬资源保证。
 */
[[nodiscard]] base::Result<void> validate_offset_table(
    const OffsetTable &table, const DocumentStoreLimits &limits);

/**
 * @brief 按 docs/DOCUMENT_STORE.md 的 SWDI v1 格式编码有效表
 *
 * @param[in] table
 *     调用期间借用；先完整验证再分配字节，输入不变。
 * @param[in] limits
 *     同 validate_offset_table。
 *
 * @return
 *     成功拥有完整字节；失败无部分编码。不写文件、不发布。
 * @throws
 *     分配与未知异常上传。
 */
[[nodiscard]] base::Result<std::string> encode_offset_table(
    const OffsetTable &table, const DocumentStoreLimits &limits);

/**
 * @brief 验证恰好一个完整表的头、长度、CRC 与区间不变量，解码为拥有值
 *
 * @param[in] bytes
 *     调用期间借用稳定字节；拒绝未知头/保留位、截断、尾随和损坏。
 * @param[in] limits
 *     分配条目前先检查预算、计数和完整编码长度。
 *
 * @return
 *     成功表独立拥有；CRC 非认证，文件内容对应关系由 Reader::open 验证。
 * @throws
 *     分配与未知异常上传。
 */
[[nodiscard]] base::Result<OffsetTable> decode_offset_table(
    std::string_view bytes, const DocumentStoreLimits &limits);

/**
 * @brief 扫描只读普通记录库，校验每条记录并生成按 DocId 排序的完整偏移表
 *
 * @param[in] path
 *     Linux 路径字节，非空且无 NUL；拒绝叶子符号链接及非常规文件，不解析父组件的链接。
 *     调用方负责派生库路径可信、文件调用期间稳定；不使用此接口读取原始私有输入。
 * @param[in] limits
 *     正预算，整体大小及当前记录长度先于分配检查。
 *
 * @return
 *     成功表拥有，函数返回前关闭临时 fd；失败无部分表，重复 ID、尾随字节与损坏拒绝。
 * @throws
 *     分配与未知异常上传，RAII 关闭已打开 fd。
 * @note 只读、无文件写入。不是原子快照；同长并发改写或恶意重算 CRC 无真实性保证。
 */
[[nodiscard]] base::Result<OffsetTable> build_offset_table(
    const std::string &path, const DocumentStoreLimits &limits);

/**
 * @brief 独占拥有只读 fd、表副本和预算；返回文档不依赖本对象存活
 *
 * @note 不可复制；移动转交 fd，源变为 closed/空表。移动赋值先关闭原只读 fd。
 *     析构只 close 一次，忽略只读 close 错误且不重试，不承诺 errno 保持。
 * @note const read 使用 pread、不共享 seek 游标；稳定文件及对象存活由调用方保证。
 *     不允许与移动/析构并发。未提供文件原子快照，不由 const 推导外部文件不可变。
 */
class DocumentStoreReader final {
public:
    DocumentStoreReader(const DocumentStoreReader &) = delete;
    DocumentStoreReader &operator=(const DocumentStoreReader &) = delete;
    /** @brief noexcept 转交资源；源随后 read 返回 store.closed，entries 为空。 */
    DocumentStoreReader(DocumentStoreReader &&other) noexcept;
    /** @brief noexcept 关闭旧资源后转交；自移动保持原对象。 */
    DocumentStoreReader &operator=(DocumentStoreReader &&other) noexcept;
    /** @brief 释放只读 fd，不关闭调用方的任何输入句柄。 */
    ~DocumentStoreReader();

    /**
     * @brief 校验表/文件大小与全部记录 CRC/身份对应，再取得拥有型读者
     *
     * @param[in] path
     *     同 build_offset_table；O_RDONLY/CLOEXEC/NOFOLLOW/NONBLOCK 后 fstat 拒绝非普通文件。
     * @param[in] table
     *     调用期间借用，成功后内部独立复制，外部修改不影响读者。
     * @param[in] limits
     *     同 build_offset_table；成功时按值保存。
     *
     * @return
     *     成功 Result 拥有读者；失败关闭新 fd、无部分读者，不缓存所有正文。
     * @throws
     *     分配与未知异常上传，局部 RAII 保证释放。
     */
    [[nodiscard]] static base::Result<DocumentStoreReader> open(
        const std::string &path, const OffsetTable &table, const DocumentStoreLimits &limits);

    /**
     * @brief 二分查找身份，以 pread 精确读取并复验记录，再返回拥有型文档
     *
     * @param[in] doc_id
     *     同类型强身份；不要求连续，未知身份返回 store.not_found。
     *
     * @return
     *     完整 Record；任何短读到 EOF、IO 错误、大小变动、CRC/身份不符都失败，无部分文档。
     *     pread 短读继续、EINTR 重试，其他 errno 返回 store.io。未修改文件或共享文件位置。
     * @throws
     *     分配与未知异常上传；当前读者资源仍由自身拥有。
     * @note 读前后复核大小；未读记录的同长变动或校验后改写不保证发现，不替代未来 manifest。
     */
    [[nodiscard]] base::Result<DocumentRecord> read(base::DocumentId doc_id) const;

    /**
     * @brief 借用内部只读表项；引用不延长读者生命，移动/赋值/析构使既有借用失效
     *
     * @return
     *     有效至上述失效动作的 const vector 引用；不允许并发修改其所有者。
     */
    [[nodiscard]] const std::vector<OffsetEntry> &entries() const & noexcept {
        return table_.entries;
    }
    /** @brief 禁止从临时读者取得即将失效的引用。 */
    const std::vector<OffsetEntry> &entries() const && = delete;

private:
    DocumentStoreReader(int fd, OffsetTable table, DocumentStoreLimits limits) noexcept;
    /** 独占 fd，-1 为移动后的 closed 状态，不向调用方暴露或借出。 */
    int fd_;
    /** 拥有在 open 完整校验的表副本，不保存来源字节或外部容器引用。 */
    OffsetTable table_;
    /** 拥有本次读取预算副本，不随调用方参数修改而变化。 */
    DocumentStoreLimits limits_;
};
} // namespace siftwing::document
