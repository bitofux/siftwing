/*
 * PROJECT : SIFTWING
 * FILE    : lexicon.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 完整校验词键后确定编号，生成独立推荐频次与标量关系
 * IMPLEMENTATION : 借用排序不逃逸；预算先验、关系唯一，失败不发布部分对象
 */
#include "siftwing/index/lexicon.h"
#include <utf8.h>
#include <algorithm>
#include <set>
#include <utility>

namespace siftwing::index {
namespace {
template <typename T> base::Result<T> fail(const char *context, const char *message) {
    return base::Result<T>::failure({message, context});
}
bool valid_limits(const LexiconLimits &v) {
    return v.max_input_terms && v.max_input_bytes && v.max_term_bytes &&
           v.max_distinct_terms && v.max_vocabulary_bytes;
}
bool space(std::uint32_t cp) {
    return (cp >= 9U && cp <= 13U) || cp == 0x20U || cp == 0x85U || cp == 0xa0U ||
           cp == 0x1680U || (cp >= 0x2000U && cp <= 0x200aU) || cp == 0x2028U ||
           cp == 0x2029U || cp == 0x202fU || cp == 0x205fU || cp == 0x3000U;
}
base::Result<void> validate_terms(const std::vector<std::string_view> &terms,
                                 const LexiconLimits &limits) {
    if (!valid_limits(limits)) { return fail<void>("lexicon.limits", "five positive limits required"); }
    if (terms.size() > limits.max_input_terms) { return fail<void>("lexicon.input_terms", "too many input terms"); }
    std::uint64_t bytes = 0;
    for (const auto word : terms) {
        if (word.size() > limits.max_term_bytes) { return fail<void>("lexicon.term_bytes", "term exceeds byte budget"); }
        if (word.size() > limits.max_input_bytes - bytes) { return fail<void>("lexicon.input_bytes", "input bytes exceed budget"); }
        bytes += word.size();
    }
    // 完整编码/NUL优先于词形，重复词也必须校验，借用只在本调用内使用。
    for (std::size_t index = 0; index < terms.size(); ++index) {
        const auto word = terms[index];
        const auto invalid = utf8::find_invalid(word.begin(), word.end());
        if (invalid != word.end()) {
            return base::Result<void>::failure({"term " + std::to_string(index) + " invalid UTF-8 at byte " +
                std::to_string(invalid - word.begin()), "lexicon.utf8"});
        }
        const auto nul = word.find('\0');
        if (nul != word.npos) {
            return base::Result<void>::failure({"term " + std::to_string(index) + " NUL at byte " +
                std::to_string(nul), "lexicon.nul"});
        }
    }
    for (const auto word : terms) {
        if (word.empty()) { return fail<void>("lexicon.term", "term must be nonempty without whitespace"); }
        auto cursor = word.begin();
        while (cursor != word.end()) {
            if (space(utf8::next(cursor, word.end()))) {
                return fail<void>("lexicon.term", "term must be nonempty without whitespace");
            }
        }
    }
    return base::Result<void>::success();
}
} // namespace

Lexicon::Lexicon(Lexicon &&other) noexcept : terms_(std::move(other.terms_)) { other.terms_.clear(); }
Lexicon &Lexicon::operator=(Lexicon &&other) noexcept {
    if (this != &other) { terms_ = std::move(other.terms_); other.terms_.clear(); }
    return *this;
}
base::Result<Lexicon> Lexicon::build(const std::vector<std::string_view> &terms,
                                    const LexiconLimits &limits) {
    auto valid = validate_terms(terms, limits);
    if (!valid) { return base::Result<Lexicon>::failure(valid.error()); }
    std::vector<std::string_view> sorted = terms;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (sorted.size() > limits.max_distinct_terms || sorted.size() > std::vector<std::string>{}.max_size()) {
        return fail<Lexicon>("lexicon.distinct_terms", "unique term count exceeds budget or capacity");
    }
    std::uint64_t bytes = 0;
    for (const auto word : sorted) {
        if (word.size() > limits.max_vocabulary_bytes - bytes) {
            return fail<Lexicon>("lexicon.vocabulary_bytes", "vocabulary bytes exceed budget");
        }
        bytes += word.size();
    }
    std::vector<std::string> owned;
    owned.reserve(sorted.size());
    for (const auto word : sorted) { owned.emplace_back(word); }
    return base::Result<Lexicon>::success(Lexicon{std::move(owned)});
}
std::optional<base::TermId> Lexicon::find(std::string_view word) const {
    const auto it = std::lower_bound(terms_.begin(), terms_.end(), word,
        [](const std::string &candidate, std::string_view key) { return std::string_view{candidate} < key; });
    if (it == terms_.end() || *it != word) { return std::nullopt; }
    // 构建已用uint64预算限制输入条数；有效迭代器距离不会超过该范围。
    return base::TermId::from_integer(static_cast<std::uint64_t>(it - terms_.begin())).value();
}
std::optional<std::string_view> Lexicon::term(base::TermId id) const & noexcept {
    if (id.value() >= terms_.size()) { return std::nullopt; }
    return std::string_view{terms_[static_cast<std::size_t>(id.value())]};
}

RecommendationDictionary::RecommendationDictionary(Lexicon lexicon,
    std::vector<std::uint64_t> frequencies, CharacterIndex characters,
    std::uint64_t documents, std::uint64_t total_tokens) noexcept
    : lexicon_(std::move(lexicon)), frequencies_(std::move(frequencies)),
      characters_(std::move(characters)), documents_(documents), total_tokens_(total_tokens) {}
RecommendationDictionary::RecommendationDictionary(RecommendationDictionary &&other) noexcept
    : lexicon_(std::move(other.lexicon_)), frequencies_(std::move(other.frequencies_)),
      characters_(std::move(other.characters_)), documents_(std::exchange(other.documents_, 0)),
      total_tokens_(std::exchange(other.total_tokens_, 0)) {
    other.frequencies_.clear(); other.characters_.clear();
}
RecommendationDictionary &RecommendationDictionary::operator=(RecommendationDictionary &&other) noexcept {
    if (this != &other) {
        lexicon_ = std::move(other.lexicon_); frequencies_ = std::move(other.frequencies_);
        characters_ = std::move(other.characters_); documents_ = std::exchange(other.documents_, 0);
        total_tokens_ = std::exchange(other.total_tokens_, 0);
        other.frequencies_.clear(); other.characters_.clear();
    }
    return *this;
}
base::Result<RecommendationDictionary> RecommendationDictionary::build(
    const text::RecommendationFrequencies &input, const RecommendationDictionaryLimits &limits) {
    if (!valid_limits(limits.lexicon) || !limits.max_documents || !limits.max_total_tokens ||
        !limits.max_characters || !limits.max_character_links) {
        return fail<RecommendationDictionary>("dictionary.limits", "nine positive limits required");
    }
    if (input.documents > limits.max_documents) { return fail<RecommendationDictionary>("dictionary.documents", "too many deliveries"); }
    if (input.terms.size() > limits.lexicon.max_input_terms) {
        return fail<RecommendationDictionary>("lexicon.input_terms", "too many input terms");
    }
    std::vector<std::string_view> keys;
    keys.reserve(input.terms.size());
    for (const auto &entry : input.terms) { keys.push_back(entry.first); }
    auto valid = validate_terms(keys, limits.lexicon);
    if (!valid) { return base::Result<RecommendationDictionary>::failure(valid.error()); }
    std::uint64_t total = 0;
    for (const auto &entry : input.terms) {
        if (!entry.second) { return fail<RecommendationDictionary>("dictionary.table", "frequencies must be positive"); }
        auto next = base::checked_add(total, entry.second, "dictionary.overflow");
        if (!next) { return base::Result<RecommendationDictionary>::failure(next.error()); }
        total = next.value();
    }
    if (total != input.total_tokens || (!input.documents && (!input.terms.empty() || total))) {
        return fail<RecommendationDictionary>("dictionary.table", "statistics do not match frequency table");
    }
    if (total > limits.max_total_tokens) { return fail<RecommendationDictionary>("dictionary.total_tokens", "total exceeds budget"); }
    auto lexicon = Lexicon::build(keys, limits.lexicon);
    if (!lexicon) { return base::Result<RecommendationDictionary>::failure(lexicon.error()); }
    std::vector<std::uint64_t> counts;
    counts.reserve(keys.size());
    CharacterIndex characters;
    std::uint64_t links = 0;
    for (std::size_t index = 0; index < lexicon.value().terms().size(); ++index) {
        const auto &word = lexicon.value().terms()[index];
        counts.push_back(input.terms.at(word));
        std::set<char32_t> unique;
        auto cursor = word.begin();
        while (cursor != word.end()) { unique.insert(static_cast<char32_t>(utf8::next(cursor, word.end()))); }
        const auto id = base::TermId::from_integer(static_cast<std::uint64_t>(index)).value();
        for (const auto cp : unique) {
            if (characters.find(cp) == characters.end() && characters.size() == limits.max_characters) {
                return fail<RecommendationDictionary>("dictionary.characters", "distinct characters exceed budget");
            }
            if (links == limits.max_character_links) {
                return fail<RecommendationDictionary>("dictionary.character_links", "character relations exceed budget");
            }
            // 词项按ID递增且本词字符先去重，因此列表无需再排序去重，也不会乘以词频。
            characters[cp].push_back(id); ++links;
        }
    }
    return base::Result<RecommendationDictionary>::success(RecommendationDictionary{
        std::move(lexicon).value(), std::move(counts), std::move(characters), input.documents, total});
}
std::optional<std::uint64_t> RecommendationDictionary::frequency(base::TermId id) const noexcept {
    if (id.value() >= frequencies_.size()) { return std::nullopt; }
    return frequencies_[static_cast<std::size_t>(id.value())];
}
} // namespace siftwing::index
