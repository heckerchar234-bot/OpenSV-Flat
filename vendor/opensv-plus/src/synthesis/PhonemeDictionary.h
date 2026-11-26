#pragma once

#include <juce_core/juce_core.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sv::synthesis
{
struct PhonemeDefinition
{
    std::string symbol;
    std::string category;
};

// File order is retained for inspection; it is not a model's phoneme-index mapping.
// Loading and queries allocate memory and must run outside the audio callback.
// Complete load before concurrent read-only queries; reloading requires exclusive access.
class PhonemeDictionary final
{
public:
    // Failure leaves the previously loaded dictionary intact.
    [[nodiscard]] juce::Result load(const juce::File& phonesFile, const juce::File& dictionaryFile);
    [[nodiscard]] juce::Result loadMandarin(const juce::File& phonesFile, const juce::File& dictionaryFile, const juce::File& cedictFile);

    // English ships a phones inventory plus CMUdict instead of a per-word
    // -dict.txt; keys are matched case-insensitively without stress marks.
    [[nodiscard]] juce::Result loadCmudict(const juce::File& phonesFile, const juce::File& cmudictFile);

    // Japanese lyrics are syllabary text, not dictionary keys. The kana maps in
    // a clf-data directory turn them into the romaji morae the dictionary uses.
    [[nodiscard]] juce::Result loadJapaneseKana(const juce::File& directory);

    // Korean lyrics are Hangul. korean-dict.txt maps every syllable to the
    // romanization that korean-xsampa-dict.txt already keys on.
    [[nodiscard]] juce::Result loadKorean(const juce::File& directory);

    // Generic dictionaries use exact, case-sensitive keys. Mandarin also accepts normalized
    // pinyin and the first supported single-syllable CEDICT reading; it does not segment lyrics.
    // Japanese accepts those exact keys and otherwise segments the syllabary into morae.
    // Korean accepts an exact romanized key and otherwise walks the lyric syllable by syllable.
    // All query methods leave output intact on failure and reject empty input.
    [[nodiscard]] juce::Result lookup(std::string_view lyrics, std::vector<std::string>& output) const;
    [[nodiscard]] juce::Result parseExplicitPhonemes(std::string_view text, std::vector<std::string>& output) const;

    [[nodiscard]] bool isLoaded() const;
    [[nodiscard]] const std::vector<PhonemeDefinition>& getPhonemes() const;
    [[nodiscard]] std::size_t getEntryCount() const;

private:
    [[nodiscard]] juce::Result loadPhoneInventory(const juce::File& phonesFile);
    [[nodiscard]] juce::Result lookupJapanese(const juce::String& lyrics, std::vector<std::string>& output) const;
    [[nodiscard]] juce::Result lookupKorean(const juce::String& lyrics, std::vector<std::string>& output) const;

    std::vector<PhonemeDefinition> phonemes;
    std::unordered_set<std::string> symbols;
    std::unordered_map<std::string, std::vector<std::string>> entries;
    std::unordered_map<std::string, std::string> kana;
    bool isMandarin = false;
    bool isJapanese = false;
    bool isEnglish = false;
    bool isKorean = false;
};
} // namespace sv::synthesis
