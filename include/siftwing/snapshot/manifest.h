/*
 * PROJECT : SIFTWING
 * FILE    : manifest.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 定义完整快照固定清单、独立来源摘要及内存字节校验合同
 */
#pragma once
#include "siftwing/base/id.h"
#include <array>
#include <string>
#include <string_view>

namespace siftwing::snapshot {
/** @brief manifest格式版本，独立于不透明SnapshotVersion和各payload版本。 */
inline constexpr std::uint32_t manifest_format_version = 1;
/** @brief 固定位置角色；不能把强文档/词身份或任意路径用作文件角色。 */
enum class Artifact : std::size_t { documents, offsets, search_lexicon, search_weights,
    recommendation_dictionary, recommendation_characters };
/** @brief 固定叶子名和格式版本，字符串拥有静态生命，不提供文件系统安全保证。 */
struct FileLayout final {
    std::string_view name; ///< ASCII固定叶子名，无路径组件或外部所有者。
    std::uint32_t version; ///< 该payload当前支持的编码版本。
};
/** @brief 六文件必须齐全；顺序为角色顺序，不按路径或输入排列改变。 */
inline constexpr std::array<FileLayout,6> artifact_layout{{
    {"documents.bin",1}, {"offsets.bin",1}, {"search-lexicon.txt",1}, {"search-weights.txt",1},
    {"recommendation-dictionary.txt",1}, {"recommendation-characters.txt",1}}};
/** @brief 三项正预算，含端点；不计已有输入、容器容量或分配器，不是硬资源限制。 */
struct ManifestLimits final {
    std::uint64_t max_manifest_bytes = 0; ///< 完整manifest输入/输出字节数。
    std::uint64_t max_file_bytes = 0; ///< 每个payload声明/验证的字节数。
    std::uint64_t max_total_file_bytes = 0; ///< 六个payload受检总和，不含manifest/COMPLETE。
};
/** @brief 调用方提供的独立来源声明；合法摘要不能证明原始选择或配置正确。 */
struct Provenance final {
    std::string input_sha256; ///< 拥有输入集合规范描述的64位小写hex SHA-256。
    std::string config_sha256; ///< 拥有该支配置/资源规范描述的SHA-256，不能隐式复用另一支。
};
/** @brief 搜索显式保留集统计，不从推荐次数或最大DocId推导。 */
struct SearchSummary final {
    std::uint64_t documents = 0; ///< N，包括调用方显式选入的空篇。
    std::uint64_t terms = 0; ///< 搜索词表条数。
    std::uint64_t total_tokens = 0; ///< 原整数TF受检总和。
    std::uint64_t postings = 0; ///< 唯一词-文档关系数，等于DF之和。
};
/** @brief 推荐显式交付的累计统计，词ID属于独立推荐词表。 */
struct RecommendationSummary final {
    std::uint64_t documents = 0; ///< 交付次数，不默认搜索去重。
    std::uint64_t terms = 0; ///< 推荐词表条数。
    std::uint64_t total_tokens = 0; ///< 累计出现次数，不是搜索DF。
    std::uint64_t characters = 0; ///< 不同Unicode标量键数，非字素数。
    std::uint64_t character_links = 0; ///< 唯一(标量,推荐TermId)关系数。
};
/** @brief 一项拥有型文件声明，角色由array位置决定；可修改值须再次validate。 */
struct FileDigest final {
    std::uint32_t format_version = 1; ///< 该角色payload格式，不是manifest或快照版本。
    std::uint64_t bytes = 0; ///< 精确完整字节数，文档库之外的空编码也须非空。
    std::string sha256; ///< 完整payload字节的64位小写hex SHA-256，无认证保证。
};
/**
 * @brief 拥有型可修改元数据；消费函数重新完整验证，无文件IO或隐式发布
 * @note 复制独立拥有；默认移动后的字符串有效但内容未指定，不保证仍是有效manifest。
 *     无内部借用、fd或缓存。所有调用期间输入不得失效或并发写；未实测并发。
 */
struct Manifest final {
    base::SnapshotVersion snapshot; ///< 不透明身份，0和UINT64_MAX合法，不代表时间顺序。
    std::string build_version; ///< 1—128字节ASCII [A-Za-z0-9._+-]，拥有稳定构建身份。
    SearchSummary search; ///< 搜索部件生产的整数统计，不能证明跨文件语义一致。
    Provenance search_provenance; ///< 搜索自己的输入/config摘要。
    RecommendationSummary recommendation; ///< 推荐部件生产的独立累计统计。
    Provenance recommendation_provenance; ///< 推荐自己的输入/config摘要。
    std::array<FileDigest,6> files; ///< 按artifact_layout逐位置拥有完整六项声明。
};
/**
 * @brief 验证元数据，不访问目录或证明payload语义
 * @param[in] manifest
 *     同步稳定借用，不排序/改写；摘要、统计、版本、长度和受检总和逐层验证。
 * @param[in] limits
 *     三项正预算先检查；长度含端点，空文档库合法，偏移表长度须32+24*N。
 *
 * @return
 *     success<void>或manifest.limits/metadata/statistics/version/length/overflow/file_bytes/total_bytes。
 * @throws
 *     诊断分配及未知异常上传；成功不冻结可修改manifest。
 */
[[nodiscard]] base::Result<void> validate_manifest(const Manifest &manifest, const ManifestLimits &limits);
/**
 * @brief 完整验证后按SNAPSHOT_FORMAT.md生成规范文本，无部分输出
 * @param[in] manifest
 *     同步稳定借用；不保存输入引用。
 * @param[in] limits
 *     复用完整验证，输出每次append前检查max_manifest_bytes。
 *
 * @return
 *     拥有string或验证/manifest.bytes诊断，to_chars不依赖locale。
 * @throws
 *     分配与未知异常上传。
 */
[[nodiscard]] base::Result<std::string> serialize_manifest(const Manifest &manifest, const ManifestLimits &limits);
/**
 * @brief 只接受恰好一份规范manifest，解码为独立拥有值
 * @param[in] bytes
 *     同步借用；先正预算及输入长度，再固定行/字段/规范十进制，最后语义验证。
 * @param[in] limits
 *     与serialize相同；拒未知版本/角色、缺项、重复、尾随、CRLF和非规范空格/数字。
 *
 * @return
 *     拥有Manifest或manifest.limits/bytes/syntax/version及语义诊断，无部分对象。
 * @throws
 *     分配与未知异常上传。
 */
[[nodiscard]] base::Result<Manifest> parse_manifest(std::string_view bytes, const ManifestLimits &limits);
/**
 * @brief 对同步借用的完整内存字节计算标准SHA-256，不读文件
 * @param[in] bytes
 *     稳定的合法内存范围；空字节合法，最大长度为UINT64_MAX/8以表示位长度。
 *
 * @return
 *     拥有64位小写hex摘要或manifest.hash_length；不证明来源、不可变性或真实性。
 * @throws
 *     输出/诊断分配异常上传；算法仅使用固定大小栈数组，输入不复制。
 */
[[nodiscard]] base::Result<std::string> sha256_bytes(std::string_view bytes);
/**
 * @brief 验证完整manifest后，对一个固定角色核对内存payload长度与SHA-256
 * @param[in] manifest
 *     稳定借用的完整声明。
 * @param[in] artifact
 *     六个固定角色之一，伪造枚举值返回manifest.role。
 * @param[in] bytes
 *     同步稳定借用；成功不保留、不加载或冻结字节，不核跨文件语义。
 * @param[in] limits
 *     先完整validate，然后角色、精确长度、哈希。
 *
 * @return
 *     success<void>或验证/manifest.role/length/checksum诊断。
 * @throws
 *     分配与未知异常上传。
 */
[[nodiscard]] base::Result<void> verify_artifact(const Manifest &manifest, Artifact artifact,
    std::string_view bytes, const ManifestLimits &limits);
/**
 * @brief 完整parse后生成绑定manifest字节及快照身份的规范COMPLETE内容
 * @param[in] manifest_bytes
 *     稳定完整manifest字节；标记不在其payload清单，避免自校验循环。
 * @param[in] limits
 *     复用parse预算，不将标记混入payload总字节预算。
 *
 * @return
 *     拥有标记string或parse/hash诊断；不证明payload完成或发布成功。
 * @throws
 *     分配与未知异常上传。
 */
[[nodiscard]] base::Result<std::string> completion_marker(std::string_view manifest_bytes, const ManifestLimits &limits);
/**
 * @brief 重新生成预期标记并逐字节比较，无IO
 * @param[in] manifest_bytes
 *     同completion_marker，首先完整parse。
 * @param[in] marker
 *     稳定借用，仅接受恰好一个规范标记，不保存引用。
 * @param[in] limits
 *     同completion_marker。
 *
 * @return
 *     success<void>或parse/hash/manifest.completion诊断，标记绑定不是认证。
 * @throws
 *     分配与未知异常上传。
 */
[[nodiscard]] base::Result<void> verify_completion(std::string_view manifest_bytes,
    std::string_view marker, const ManifestLimits &limits);
} // namespace siftwing::snapshot
