// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace hios {
// Bounded candidate cache owned by the keyboard context; vocabulary stays in flash.
class PinyinEngine {
public:
    static constexpr size_t kMaxInput = 32;
    static constexpr size_t kCandidateBytes = 64;
    bool Append(char letter);
    bool Backspace();
    void Clear();
    const char *Preedit() const { return input_; }
    size_t Count() const;
    bool Candidate(size_t index, char *out, size_t capacity) const;
    bool Consume(size_t index=0);
    void SetDoublePinyin(bool enabled);
    bool IsDoublePinyin() const { return double_; }
    static size_t SegmentBytes(const char *text);
private:
    static constexpr size_t kMaxCandidates=96;
    void Rebuild() const;
    void Add(const char *text,size_t bytes,size_t used) const;
    bool double_=false;
    mutable bool dirty_=true;
    mutable size_t count_=0;
    mutable char candidates_[kMaxCandidates][kCandidateBytes]{};
    mutable uint8_t consumed_[kMaxCandidates]{};
    char input_[kMaxInput + 1] = {};
    size_t length_ = 0;
};
// Nine-key input uses 2..9, with a selectable spelling to resolve ambiguity.
// Candidates are computed from the same flash dictionary without a large cache.
class NineKeyEngine {
public:
    bool Append(char digit);
    bool Backspace();
    void Clear();
    const char *Preedit() const { return input_; }
    size_t Count() const;
    bool Candidate(size_t index, char *out, size_t capacity, size_t *consumed = nullptr) const;
    bool Consume(size_t index);
    size_t SpellingCount() const;
    bool Spelling(size_t index, char *out, size_t capacity) const;
    bool SelectSpelling(size_t index);
    const char *SelectedSpelling() const { return spelling_; }
    bool Preview(char *out, size_t capacity) const;
private:
    size_t SegmentLength() const;
    size_t MatchedLength() const;
    char input_[PinyinEngine::kMaxInput + 1] = {};
    char spelling_[7] = {};
    size_t length_ = 0;
};
}
