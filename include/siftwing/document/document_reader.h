/*
 * PROJECT : SIFTWING
 * FILE    : document_reader.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-07
 * BRIEF   : 此模块负责：
 * -- 定义同步输入适配、拥有型读取结果及可定位诊断
 * -- 统一完整/警告/无文字/拒绝/失败与显式部分结果策略
 * -- 在接收结果时检查限额、确定顺序并保留批量停止之前的报告
 */

#pragma once

#include "siftwing/base/result.h"
#include "siftwing/document/document_record.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace siftwing::document {

/** @brief 一条逻辑来源记录的读取状态；数值不构成持久化格式。 */
enum class ReadStatus {
    success,  ///< 完整、可交付的记录，无诊断。
    warning,  ///< 有可交付记录和诊断；部分结果须另有未覆盖范围。
    no_text,  ///< 没有可交付正文，以诊断说明原因。
    rejected, ///< 因支持范围或限制拒绝，未交付文档。
    failed    ///< 读取/解析失败，未交付文档。
};

/** @brief 可区分的读取原因；状态由 ReadStatus 表达，不从枚举值推导严重程度。 */
enum class ReadIssueCode {
    empty_content,        ///< 抽取后无可索引正文。
    unsupported_format,   ///< 当前适配器不支持请求种类。
    unsupported_encoding, ///< 当前编码不支持或非法。
    input_too_large,      ///< 单个输入字节数超限。
    document_too_large,   ///< 单篇标题加正文的字节数超限。
    encrypted,            ///< 加密输入无法在当前范围处理。
    malformed_input,      ///< 输入损坏或格式不成立。
    io_error,             ///< 生产者报告输入 IO 失败。
    title_fallback,       ///< 使用确定的标题回退。
    missing_metadata,     ///< 可选来源元数据缺失。
    unparsed_date,        ///< 原日期保存为文本，未获得有效解析结果。
    partial_extraction,   ///< 已知部分内容未覆盖，region 必须定位未覆盖范围。
    partial_not_allowed   ///< 调用方未允许接纳部分结果。
};

/** @brief 拥有型诊断；详细格式与具体格式适配器对应，不解析 region 文本。 */
struct ReadIssue final {
    ReadIssueCode code = ReadIssueCode::io_error; ///< 语义原因；不是 errno 或底层库的错误编号。
    std::string message; ///< 非空、拥有型原因文本；不保存输入视图。
    std::string region; ///< 可为空的格式内定位；partial_extraction 时须描述未覆盖范围。
};

/** @brief 一条逻辑记录的拥有型结果；read_documents 在接收时检查状态与载荷一致性。 */
struct ReadItem final {
    SourceIdentity source_id; ///< 拥有来源位置；输入名必须与请求一致，序号按交付顺序严格递增。
    ReadStatus status = ReadStatus::failed; ///< success/warning 才交付 record，其余状态不含 record。
    std::optional<DocumentRecord> record; ///< 拥有抽取记录；有载荷时来源/种类必须与请求一致。
    std::vector<ReadIssue> issues; ///< success 为空，其余状态至少有一条非空原因；顺序由适配器确定。
};

/** @brief 本次同步调用的输入；已装载字节的所有者仍为调用者。 */
struct ReaderInput final {
    SourceKind kind = SourceKind::txt; ///< 来源种类；组装默认TXT，不按扩展名猜测。
    std::string input_path; ///< 拥有型相对标识；非空，无 NUL、空路径分段、. 或 .. 分段。
    std::string_view bytes; ///< 仅本次同步调用借用，调用结束前须存活且不被修改。
};

/**
 * @brief 调用方必须显式提供的四项正数限额，单位均为字节或逻辑结果条数
 *
 * @note max_records 包括所有状态的结果，防止大量无文字/失败诊断绕过批量条数上限。
 * @note 文本字节只计算标题加正文，不含来源/诊断或容器开销；不是整个进程内存上限。
 *     共用层检查接纳；适配器还须在抽取、拼接与分配前执行相同单篇限制和自身资源限制。
 *     bytes 已由调用方装载，本接口不能追回装载前的 IO/分配成本。
 */
struct ReadLimits final {
    std::uint64_t max_input_bytes = 0; ///< 每个输入最大字节数；0表示尚未配置，等于正数上限合法。
    std::uint64_t max_document_bytes = 0; ///< 每篇标题加正文最大字节数；0尚未配置。
    std::uint64_t max_records = 0; ///< 批量全部状态结果最大条数；0尚未配置。
    std::uint64_t max_total_text_bytes = 0; ///< 批量接纳记录累计文本最大字节数；0尚未配置。
};

/** @brief rejected/failed 之后的批量控制；warning/no_text 本身不触发失败停止。 */
enum class FailurePolicy { stop, continue_reading };
/** @brief 部分抽取记录是否允许进入交付集合；接纳时仍保留警告及未覆盖范围。 */
enum class PartialPolicy { reject, accept };

/** @brief 显式传入的批量策略；组装默认值为遇错停止、拒绝部分结果。 */
struct ReadPolicy final {
    FailurePolicy failure = FailurePolicy::stop; ///< 仅控制普通 rejected/failed，不吞掉诊断。
    PartialPolicy partial = PartialPolicy::reject; ///< 部分结果必须显式选择 accept 才接纳。
};

/** @brief 报告各状态条数；总和等于 items.size()，部分结果接纳后计入 warning。 */
struct ReadCounts final {
    std::uint64_t success = 0; ///< 完整记录数。
    std::uint64_t warning = 0; ///< 带警告的已接纳记录数。
    std::uint64_t no_text = 0; ///< 无文字结果数。
    std::uint64_t rejected = 0; ///< 已拒绝结果数。
    std::uint64_t failed = 0; ///< 已失败结果数。
};

/** @brief 批量提前停止的原因；没有 stop 表示已遍历全部请求。 */
enum class ReadStopReason { failure_policy, record_limit, total_text_limit };

/** @brief 拥有型停止位置；容量不足的候选未写入 items，也不计入 counts。 */
struct ReadStop final {
    ReadStopReason reason = ReadStopReason::record_limit; ///< 区分策略停止、条数不足和累计文本不足。
    SourceIdentity source_id; ///< 触发停止的输入/逻辑位置；文件尚未调用时序号为0。
    std::string message; ///< 拥有型原因；不表示尚未处理的后续输入已成功或失败。
};

/** @brief 批量读取的拥有型报告；可在输入销毁后使用。 */
struct ReadReport final {
    std::vector<ReadItem> items; ///< 按输入名 std::string 字典序、文件内严格递增序号保存结果。
    ReadCounts counts; ///< 包含拒绝与失败；空批量合法，统计全0。
    std::uint64_t total_text_bytes = 0; ///< 已接纳记录的标题与正文总字节数。
    std::optional<ReadStop> stop; ///< 提前停止才存在；已有结果保留，没有静默截断。
};

/** @brief 仅在 DocumentReader::read 同步调用期间借用的接收端。 */
class ReadSink {
public:
    /** @brief 多态析构；不拥有适配器或原始输入。 */
    virtual ~ReadSink() = default;

    /**
     * @brief 移交一条拥有型结果并取得是否允许继续的信号
     *
     * @param[in] item
     *     按值移交的逻辑结果；不得包含输入视图或不符合当前来源的记录。
     *
     * @return
     *     true 允许继续；false 要求立即结束当前 read，不再提交下一条。
     *
     * @note 不得保存本接收端引用、跨线程调用、重入或在返回 false 后再次 emit。
     * @throws
     *     载荷、诊断或容器分配异常向上传递；没有自动转换成普通读取失败。
     */
    [[nodiscard]] bool emit(ReadItem item) { return accept(std::move(item)); }

protected:
    ReadSink() = default;
    ReadSink(const ReadSink&) = delete;
    ReadSink& operator=(const ReadSink&) = delete;

    /**
     * @brief 实现同步接纳；仅由带 nodiscard 的普通成员入口 emit 转发调用
     *
     * @param[in] item
     *     已按值移交的结果；所有权和停止行为沿 emit 合同。
     *
     * @return
     *     是否允许适配器继续。
     *
     * @note 普通成员入口使返回值检查不依赖编译器对虚函数 nodiscard 的诊断差异。
     */
    virtual bool accept(ReadItem item) = 0;
};

/** @brief 格式适配器的同步公共接口；共用层不在此解析 TXT/XML/JSONL。 */
class DocumentReader {
public:
    /** @brief 多态析构；具体解析资源由适配器 RAII 成员释放。 */
    virtual ~DocumentReader() = default;

    /**
     * @brief 为一个输入按逻辑顺序交付结果，包括无文字、拒绝或读取失败
     *
     * @param[in] input
     *     当前输入；引用及字节视图仅本次调用有效，不允许保存或写回。
     * @param[in] limits
     *     四项正数接纳限额；解析器须在增长输出前检查单篇与自身资源限制。
     * @param[in,out] sink
     *     同步接收端；每条结果移交后处理返回值，false 后立即结束。
     *
     * @note 至少交付一条结果，空输入也须明确报告 no_text；普通输入失败交付 failed/rejected。
     *     载荷须为符合适配合同的 UTF-8 文本，有载荷的正文非空。UTF-8/空白语义、标题回退和
     *     格式识别由具体适配器负责；本接口不生成 ID，适配器/调用方须给批量记录提供唯一 ID。
     * @note 调用线程同步执行；同一适配器的并发使用/重复读取复位规则由具体类型约定，
     *     不能从接口推断任意适配器线程安全。输出顺序及相同输入的语义须确定。
     * @throws
     *     未预期异常向上传递；不把分配异常伪装为完整报告，不捕获任意异常。
     */
    virtual void read(const ReaderInput& input, const ReadLimits& limits, ReadSink& sink) = 0;

protected:
    DocumentReader() = default;
    DocumentReader(const DocumentReader&) = delete;
    DocumentReader& operator=(const DocumentReader&) = delete;
};

/**
 * @brief 同步排序输入、调用适配器并接收有界结果，保留普通失败之前的报告
 *
 * @param[in] inputs
 *     唯一相对输入标识与借用字节；不修改输入，调用结束前所有字节必须存活。
 * @param[in,out] reader
 *     当前批量适配器；允许在接口内部派发不同种类，所有调用依次执行。
 * @param[in] limits
 *     四项正数限额；单输入超限在调用适配器前产生 rejected，单篇超限拒绝整篇。
 * @param[in] policy
 *     显式策略；部分结果拒绝时清除文档并保留原因，普通拒绝/失败按 failure 决定是否继续。
 *
 * @return
 *     success 拥有完整或已明确提前停止的报告（其中可含普通 failed/rejected）；failure 是
 *     请求/配置非法或适配器违反合同，含上下文。调用方必须再检查 report.stop 和逐条状态，
 *     不能把 Result 成功解读成全部输入成功。合同错误时不发布不可信的部分报告。
 *
 * @note 文件内序号严格递增、有载荷的来源匹配、内部 ID 批量唯一；接收端检查这些不变量。
 *     条数或累计文本不足总是停止，候选未接纳，有明确 stop；不会无提示截短文本或丢掉来源。
 *     路径只是逻辑标识，词法检查不证明实际文件不存在 symlink；本函数没有 IO、联网或原始写回。
 * @throws
 *     适配器及标准库分配异常上传；没有通用 noexcept 或整体内存/CPU 时间上限保证。
 */
[[nodiscard]] base::Result<ReadReport> read_documents(const std::vector<ReaderInput>& inputs,
                                                     DocumentReader& reader,
                                                     const ReadLimits& limits,
                                                     ReadPolicy policy);

} // namespace siftwing::document
