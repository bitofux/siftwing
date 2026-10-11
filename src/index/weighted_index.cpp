/*
 * PROJECT : SIFTWING
 * FILE    : weighted_index.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 固定次序计算平滑TF-IDF/L2，受限导出规范文本
 * IMPLEMENTATION : 一次接管，按词序hypot累计；位值编码避免locale/浮点文本格式差异
 */
#include "siftwing/index/weighted_index.h"
#include <algorithm>
#include <array>
#include <cfenv>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#if defined(__FAST_MATH__)
#error "weighted index requires strict floating point arithmetic"
#endif
static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559 &&
    std::numeric_limits<double>::digits == 53 && std::numeric_limits<double>::max_exponent == 1024,
    "weighted index requires IEEE754 binary64");

namespace siftwing::index {
namespace {
template <typename T> base::Result<T> fail(const char *context, const char *message) {
    return base::Result<T>::failure({message, context});
}
std::uint64_t bits(double value) noexcept {
    std::uint64_t out = 0; std::memcpy(&out, &value, sizeof(out)); return out;
}
std::size_t ordinal(const std::vector<DocumentStatistics> &documents, base::DocumentId id) noexcept {
    const auto it = std::lower_bound(documents.begin(), documents.end(), id,
        [](const auto &entry, base::DocumentId key) { return entry.document_id < key; });
    return static_cast<std::size_t>(it - documents.begin());
}
// 只限制实际字符串长度；每次追加前检查，容量/分配器开销不属于max_bytes。
class BoundedText final {
public:
    explicit BoundedText(std::uint64_t limit) : limit_(limit) {}
    bool text(std::string_view value) {
        if (!ok_ || value.size() > limit_ - data_.size()) { ok_ = false; return false; }
        data_.append(value); return true;
    }
    void number(std::uint64_t value) {
        std::array<char,20> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data()+buffer.size(), value);
        // 20位足以表示完整uint64十进制；不经过locale流或浮点转换。
        text({buffer.data(), static_cast<std::size_t>(converted.ptr-buffer.data())});
    }
    void floating(double value) {
        constexpr char alphabet[] = "0123456789abcdef";
        const auto encoded = bits(value); std::array<char,16> buffer{};
        for (std::size_t i=0; i<buffer.size(); ++i) {
            buffer[i] = alphabet[(encoded >> ((15-i)*4)) & 15U];
        }
        text({buffer.data(),buffer.size()});
    }
    bool word(std::string_view value) {
        constexpr char alphabet[] = "0123456789abcdef";
        for (unsigned char byte : value) {
            const std::array<char,2> pair{alphabet[byte >> 4], alphabet[byte & 15U]};
            if (!text({pair.data(),pair.size()})) { return false; }
        }
        return true;
    }
    bool good() const noexcept { return ok_; }
    std::string take() { return std::move(data_); }
private:
    std::uint64_t limit_;
    std::string data_;
    bool ok_ = true;
};
} // namespace

base::Result<double> smoothed_idf(std::uint64_t documents, std::uint64_t frequency) {
    if (frequency == 0 || frequency > documents) { return fail<double>("weights.statistics", "require 1 <= DF <= N"); }
    if (std::fegetround() != FE_TONEAREST) { return fail<double>("weights.rounding", "require FE_TONEAREST"); }
    // ln((N+1)/(DF+1)) = log1p((N-DF)/(DF+1))；差值先受统计边界保护，不溢出整数+1。
    const double ratio = static_cast<double>(documents-frequency) / (static_cast<double>(frequency)+1.0);
    const double value = 1.0 + std::log1p(ratio);
    if (!std::isfinite(value) || value < 1.0) { return fail<double>("weights.numeric", "invalid IDF"); }
    return base::Result<double>::success(value);
}
WeightedIndex::WeightedIndex(InvertedIndex raw, std::vector<double> idfs,
    std::vector<std::vector<double>> weights, std::vector<double> norms) noexcept
    : raw_(std::move(raw)), idfs_(std::move(idfs)), weights_(std::move(weights)), norms_(std::move(norms)) {}
WeightedIndex::WeightedIndex(WeightedIndex &&other) noexcept
    : raw_(std::move(other.raw_)), idfs_(std::move(other.idfs_)),
      weights_(std::move(other.weights_)), norms_(std::move(other.norms_)) {
    other.idfs_.clear(); other.weights_.clear(); other.norms_.clear();
}
WeightedIndex &WeightedIndex::operator=(WeightedIndex &&other) noexcept {
    if (this != &other) {
        raw_ = std::move(other.raw_); idfs_ = std::move(other.idfs_);
        weights_ = std::move(other.weights_); norms_ = std::move(other.norms_);
        other.idfs_.clear(); other.weights_.clear(); other.norms_.clear();
    }
    return *this;
}
base::Result<WeightedIndex> WeightedIndex::build(InvertedIndex input, const WeightLimits &limits) {
    using Output = base::Result<WeightedIndex>;
    if (!limits.max_documents || !limits.max_terms || !limits.max_postings) {
        return fail<WeightedIndex>("weights.limits", "three positive limits required");
    }
    if (std::fegetround() != FE_TONEAREST) { return fail<WeightedIndex>("weights.rounding", "require FE_TONEAREST"); }
    if (bits(1.0) != 0x3ff0000000000000ULL) { return fail<WeightedIndex>("weights.platform", "incompatible floating bit layout"); }
    const auto statistics = input.statistics();
    const auto &documents = input.documents(); const auto &terms = input.lexicon().terms();
    if (statistics.documents > limits.max_documents) { return fail<WeightedIndex>("weights.documents", "document budget exceeded"); }
    if (terms.size() > limits.max_terms) { return fail<WeightedIndex>("weights.terms", "term budget exceeded"); }
    if (statistics.total_postings > limits.max_postings) { return fail<WeightedIndex>("weights.postings", "posting budget exceeded"); }
    std::vector<double> idfs; idfs.reserve(terms.size());
    std::vector<std::vector<double>> weights(terms.size());
    std::vector<double> norms(documents.size(),0.0);
    for (std::size_t t=0; t<terms.size(); ++t) {
        const auto term = base::TermId::from_integer(t).value();
        const auto &postings = *input.postings(term);
        const auto idf = smoothed_idf(statistics.documents, *input.document_frequency(term));
        if (!idf) { return Output::failure(idf.error()); }
        idfs.push_back(idf.value()); weights[t].reserve(postings.size());
        for (const auto &p : postings) {
            const double value = static_cast<double>(p.term_frequency()) * idf.value();
            if (!std::isfinite(value) || value <= 0.0) { return fail<WeightedIndex>("weights.numeric", "invalid unnormalized weight"); }
            weights[t].push_back(value);
            const auto d = ordinal(documents,p.document_id());
            // InvertedIndex不可外部修改，所有Posting必在其文档统计内；稀疏ID通过二分定位。
            norms[d] = std::hypot(norms[d],value);
            if (!std::isfinite(norms[d]) || norms[d] <= 0.0) { return fail<WeightedIndex>("weights.numeric", "invalid document norm"); }
        }
    }
    for (std::size_t t=0; t<terms.size(); ++t) {
        const auto &postings = *input.postings(base::TermId::from_integer(t).value());
        for (std::size_t p=0; p<postings.size(); ++p) {
            const auto d = ordinal(documents,postings[p].document_id());
            weights[t][p] /= norms[d];
            if (!std::isfinite(weights[t][p]) || weights[t][p] <= 0.0 || weights[t][p] > 1.0) {
                return fail<WeightedIndex>("weights.numeric", "invalid normalized weight");
            }
        }
    }
    return Output::success(WeightedIndex{std::move(input),std::move(idfs),std::move(weights),std::move(norms)});
}
const std::vector<double> *WeightedIndex::weights(base::TermId term) const & noexcept {
    return term.value() < weights_.size() ? &weights_[static_cast<std::size_t>(term.value())] : nullptr;
}
std::optional<double> WeightedIndex::inverse_document_frequency(base::TermId term) const noexcept {
    if (term.value() >= idfs_.size()) { return std::nullopt; }
    return idfs_[static_cast<std::size_t>(term.value())];
}
std::optional<double> WeightedIndex::document_norm(base::DocumentId document) const noexcept {
    const auto &documents = raw_.documents(); const auto d = ordinal(documents,document);
    if (d == documents.size() || documents[d].document_id != document) { return std::nullopt; }
    return norms_[d];
}
base::Result<std::string> WeightedIndex::serialize(const WeightSerializationLimits &limits) const {
    if (!limits.max_bytes) { return fail<std::string>("weights.serialization_limits", "positive byte limit required"); }
    BoundedText out(limits.max_bytes);
    out.text("SIFTWING-WEIGHTS "); out.number(weight_serialization_version); out.text(" "); out.number(weight_policy_version); out.text("\nS ");
    const auto statistics = raw_.statistics();
    out.number(statistics.documents); out.text(" "); out.number(statistics.total_tokens); out.text(" "); out.number(statistics.total_postings); out.text("\n");
    if (!out.good()) { return fail<std::string>("weights.serialization_bytes", "serialized byte budget exceeded"); }
    const auto &documents = raw_.documents();
    for (std::size_t d=0; d<documents.size(); ++d) {
        out.text("D "); out.number(documents[d].document_id.value()); out.text(" ");
        out.number(documents[d].total_tokens); out.text(" "); out.number(documents[d].distinct_terms); out.text(" ");
        out.floating(norms_[d]); out.text("\n");
        if (!out.good()) { return fail<std::string>("weights.serialization_bytes", "serialized byte budget exceeded"); }
    }
    const auto &terms = raw_.lexicon().terms();
    for (std::size_t t=0; t<terms.size(); ++t) {
        const auto term = base::TermId::from_integer(t).value();
        out.text("T "); out.number(term.value()); out.text(" ");
        if (!out.word(terms[t])) { return fail<std::string>("weights.serialization_bytes", "serialized byte budget exceeded"); }
        out.text(" "); out.number(*raw_.document_frequency(term)); out.text(" "); out.floating(idfs_[t]); out.text("\n");
        if (!out.good()) { return fail<std::string>("weights.serialization_bytes", "serialized byte budget exceeded"); }
        const auto &postings = *raw_.postings(term);
        for (std::size_t p=0; p<postings.size(); ++p) {
            out.text("P "); out.number(term.value()); out.text(" "); out.number(postings[p].document_id().value()); out.text(" ");
            out.number(postings[p].term_frequency()); out.text(" "); out.floating(weights_[t][p]); out.text("\n");
            if (!out.good()) { return fail<std::string>("weights.serialization_bytes", "serialized byte budget exceeded"); }
        }
    }
    return base::Result<std::string>::success(out.take());
}
} // namespace siftwing::index
