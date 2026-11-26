#include "PhonemeDictionary.h"

#include <limits>
#include <utility>

namespace sv::synthesis
{
namespace
{
juce::Result decodeUtf8(std::string_view bytes, juce::String& text)
{
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return juce::Result::fail("Text exceeds the supported UTF-8 input size.");
    }
    if (bytes.find('\0') != std::string_view::npos)
    {
        return juce::Result::fail("Text contains an embedded NUL.");
    }
    if (bytes.empty())
    {
        text.clear();
        return juce::Result::ok();
    }
    if (!juce::CharPointer_UTF8::isValidString(bytes.data(), static_cast<int>(bytes.size())))
    {
        return juce::Result::fail("Text is not valid UTF-8.");
    }
    for (const auto byte : bytes)
    {
        const auto code = static_cast<unsigned char>(byte);
        if ((code < 0x20 && byte != '\t' && byte != '\r' && byte != '\n') || code == 0x7f)
        {
            return juce::Result::fail("Text contains an unsupported control character.");
        }
    }
    text = juce::String::fromUTF8(bytes.data(), static_cast<int>(bytes.size()));
    return juce::Result::ok();
}

juce::Result readText(const juce::File& file, juce::String& text)
{
    if (file.getSize() > std::numeric_limits<int>::max())
    {
        return juce::Result::fail("Dictionary file is too large: " + file.getFullPathName());
    }
    juce::MemoryBlock contents;
    if (!file.loadFileAsData(contents))
    {
        return juce::Result::fail("Could not read dictionary file: " + file.getFullPathName());
    }
    if (contents.getSize() == 0)
    {
        return juce::Result::fail("Dictionary file is empty: " + file.getFullPathName());
    }
    std::string_view bytes(static_cast<const char*>(contents.getData()), contents.getSize());
    if (bytes.starts_with("\xef\xbb\xbf"))
    {
        bytes.remove_prefix(3);
    }
    const auto result = decodeUtf8(bytes, text);
    if (result.failed())
    {
        return juce::Result::fail(file.getFullPathName() + ": " + result.getErrorMessage());
    }
    return juce::Result::ok();
}

juce::StringArray splitFields(const juce::String& line)
{
    auto fields = juce::StringArray::fromTokens(line, " \t", "");
    fields.removeEmptyStrings();
    return fields;
}

juce::Result lineError(const juce::File& file, int line, const juce::String& message)
{
    return juce::Result::fail(file.getFullPathName() + ":" + juce::String(line) + ": " + message);
}

juce::String normalizePinyin(const juce::String& text)
{
    auto normalized = text.toLowerCase().replace("u:", "v").replace(juce::String::charToString(0x00fc), "v");
    if (normalized.isNotEmpty() && normalized.getLastCharacter() >= '1' && normalized.getLastCharacter() <= '5')
    {
        normalized = normalized.dropLastCharacters(1);
    }
    for (const auto character : normalized)
    {
        if (character < 'a' || character > 'z')
        {
            return {};
        }
    }
    return normalized;
}

juce::Result parseCedictEntry(const juce::String& line, juce::StringArray& headwords, juce::StringArray& reading)
{
    const auto openingBracket = line.indexOfChar('[');
    const auto closingBracket = line.indexOfChar(']');
    if (openingBracket <= 0 || closingBracket <= openingBracket || !juce::CharacterFunctions::isWhitespace(line[openingBracket - 1]))
    {
        return juce::Result::fail("Expected a CEDICT entry: traditional simplified [pinyin].");
    }
    headwords = splitFields(line.substring(0, openingBracket));
    if (headwords.size() != 2 || headwords[0].containsAnyOf("[]") || headwords[1].containsAnyOf("[]"))
    {
        return juce::Result::fail("Expected exactly two CEDICT headwords before the pinyin reading.");
    }
    const auto pinyin = line.substring(openingBracket + 1, closingBracket);
    if (pinyin.containsAnyOf("[]"))
    {
        return juce::Result::fail("CEDICT pinyin reading contains an unexpected bracket.");
    }
    reading = splitFields(pinyin);
    if (reading.isEmpty())
    {
        return juce::Result::fail("CEDICT pinyin reading is empty.");
    }
    const auto suffix = line.substring(closingBracket + 1);
    const auto definitions = suffix.trim();
    if (definitions.isNotEmpty() && (!juce::CharacterFunctions::isWhitespace(suffix[0]) || !definitions.startsWithChar('/') || !definitions.endsWithChar('/')))
    {
        return juce::Result::fail("Unexpected text after the CEDICT pinyin reading; definitions must be slash-delimited.");
    }
    return juce::Result::ok();
}
} // namespace

juce::Result PhonemeDictionary::loadPhoneInventory(const juce::File& phonesFile)
{
    juce::String phonesText;
    if (const auto result = readText(phonesFile, phonesText); result.failed())
    {
        return result;
    }
    const auto phoneLines = juce::StringArray::fromLines(phonesText);
    for (int line = 0; line < phoneLines.size(); ++line)
    {
        const auto fields = splitFields(phoneLines[line]);
        if (fields.isEmpty())
        {
            continue;
        }
        if (fields.size() != 2)
        {
            return lineError(phonesFile, line + 1, "Expected exactly two fields: phoneme category.");
        }
        const auto symbol = fields[0].toStdString();
        if (!symbols.insert(symbol).second)
        {
            return lineError(phonesFile, line + 1, "Duplicate phoneme '" + fields[0] + "'.");
        }
        phonemes.push_back({symbol, fields[1].toStdString()});
    }
    if (phonemes.empty())
    {
        return juce::Result::fail("Phoneme inventory contains no definitions: " + phonesFile.getFullPathName());
    }
    return juce::Result::ok();
}

juce::Result PhonemeDictionary::load(const juce::File& phonesFile, const juce::File& dictionaryFile)
{
    juce::String dictionaryText;
    if (const auto result = readText(dictionaryFile, dictionaryText); result.failed())
    {
        return result;
    }

    PhonemeDictionary loaded;
    if (const auto result = loaded.loadPhoneInventory(phonesFile); result.failed())
    {
        return result;
    }

    const auto dictionaryLines = juce::StringArray::fromLines(dictionaryText);
    for (int line = 0; line < dictionaryLines.size(); ++line)
    {
        const auto fields = splitFields(dictionaryLines[line]);
        if (fields.isEmpty())
        {
            continue;
        }
        if (fields.size() < 2)
        {
            return lineError(dictionaryFile, line + 1, "Expected a dictionary key followed by one or more phonemes.");
        }
        const auto key = fields[0].toStdString();
        std::vector<std::string> pronunciation;
        pronunciation.reserve(static_cast<std::size_t>(fields.size() - 1));
        for (int field = 1; field < fields.size(); ++field)
        {
            const auto symbol = fields[field].toStdString();
            if (!loaded.symbols.contains(symbol))
            {
                return lineError(dictionaryFile, line + 1, "Undefined phoneme '" + fields[field] + "' in entry '" + fields[0] + "'.");
            }
            pronunciation.push_back(symbol);
        }
        // Some lexicons list the same key once per context (initial/final jamo
        // sections, for example). Repeats are safe only when they agree.
        if (const auto existing = loaded.entries.find(key); existing != loaded.entries.end())
        {
            if (existing->second == pronunciation)
            {
                continue;
            }
            return lineError(dictionaryFile, line + 1, "Duplicate dictionary key '" + fields[0] + "' with a conflicting pronunciation; alternative pronunciations require a confirmed format.");
        }
        loaded.entries.emplace(key, std::move(pronunciation));
    }
    if (loaded.entries.empty())
    {
        return juce::Result::fail("Pronunciation dictionary contains no entries: " + dictionaryFile.getFullPathName());
    }
    *this = std::move(loaded);
    return juce::Result::ok();
}

juce::Result PhonemeDictionary::loadCmudict(const juce::File& phonesFile, const juce::File& cmudictFile)
{
    juce::String dictionaryText;
    if (const auto result = readText(cmudictFile, dictionaryText); result.failed())
    {
        return result;
    }

    PhonemeDictionary loaded;
    if (const auto result = loaded.loadPhoneInventory(phonesFile); result.failed())
    {
        return result;
    }

    const auto lines = juce::StringArray::fromLines(dictionaryText);
    std::size_t parsed = 0;
    for (int line = 0; line < lines.size(); ++line)
    {
        const auto content = lines[line].trim();
        if (content.isEmpty() || content.startsWith(";;;") || content.startsWithChar('#'))
        {
            continue;
        }
        const auto fields = splitFields(content);
        if (fields.size() < 2)
        {
            continue;
        }
        auto word = fields[0].toLowerCase();
        if (word.containsChar('('))
        {
            // WORD(2) style alternates; the unmarked primary pronunciation already exists.
            continue;
        }
        std::vector<std::string> pronunciation;
        pronunciation.reserve(static_cast<std::size_t>(fields.size() - 1));
        for (int field = 1; field < fields.size(); ++field)
        {
            auto symbol = fields[field].toLowerCase();
            while (symbol.isNotEmpty() && symbol.getLastCharacter() >= '0' && symbol.getLastCharacter() <= '9')
            {
                symbol = symbol.dropLastCharacters(1);
            }
            const auto key = symbol.toStdString();
            if (!loaded.symbols.contains(key))
            {
                pronunciation.clear();
                break;
            }
            pronunciation.push_back(key);
        }
        if (pronunciation.empty())
        {
            continue;
        }
        loaded.entries.emplace(word.toStdString(), std::move(pronunciation));
        ++parsed;
    }
    if (parsed == 0)
    {
        return juce::Result::fail("CMUdict contains no supported pronunciations: " + cmudictFile.getFullPathName());
    }
    loaded.isEnglish = true;
    *this = std::move(loaded);
    return juce::Result::ok();
}

juce::Result PhonemeDictionary::loadMandarin(const juce::File& phonesFile, const juce::File& dictionaryFile, const juce::File& cedictFile)
{
    PhonemeDictionary loaded;
    if (const auto result = loaded.load(phonesFile, dictionaryFile); result.failed())
    {
        return result;
    }

    std::unordered_map<std::string, std::vector<std::string>> normalizedEntries;
    normalizedEntries.reserve(loaded.entries.size());
    for (auto& [key, pronunciation] : loaded.entries)
    {
        const auto normalized = normalizePinyin(juce::String::fromUTF8(key.c_str()));
        const auto normalizedKey = normalized.isEmpty() ? key : normalized.toStdString();
        const auto found = normalizedEntries.find(normalizedKey);
        if (found != normalizedEntries.end())
        {
            if (found->second != pronunciation)
            {
                return juce::Result::fail(dictionaryFile.getFullPathName() + ": Conflicting pronunciations for normalized pinyin '" + juce::String::fromUTF8(normalizedKey.c_str()) + "'.");
            }
            continue;
        }
        normalizedEntries.emplace(normalizedKey, std::move(pronunciation));
    }
    loaded.entries = normalizedEntries;

    juce::String cedictText;
    if (const auto result = readText(cedictFile, cedictText); result.failed())
    {
        return result;
    }
    const auto lines = juce::StringArray::fromLines(cedictText);
    std::size_t supportedReadings = 0;
    for (int line = 0; line < lines.size(); ++line)
    {
        const auto content = lines[line].trim();
        if (content.isEmpty() || content.startsWithChar('#'))
        {
            continue;
        }
        juce::StringArray headwords;
        juce::StringArray reading;
        if (const auto result = parseCedictEntry(content, headwords, reading); result.failed())
        {
            return lineError(cedictFile, line + 1, result.getErrorMessage());
        }
        if (reading.size() != 1)
        {
            continue;
        }
        const auto pinyin = normalizePinyin(reading[0]);
        if (pinyin.isEmpty())
        {
            continue;
        }
        const auto found = normalizedEntries.find(pinyin.toStdString());
        if (found == normalizedEntries.end())
        {
            continue;
        }
        // Insertion order chooses the first supported pronunciation, without guessing context.
        for (const auto& headword : headwords)
        {
            loaded.entries.try_emplace(headword.toStdString(), found->second);
        }
        ++supportedReadings;
    }
    if (supportedReadings == 0)
    {
        return juce::Result::fail("CEDICT contains no supported single-syllable readings: " + cedictFile.getFullPathName());
    }
    loaded.isMandarin = true;
    *this = std::move(loaded);
    return juce::Result::ok();
}

juce::Result PhonemeDictionary::lookup(std::string_view lyrics, std::vector<std::string>& output) const
{
    if (!isLoaded())
    {
        return juce::Result::fail("Phoneme dictionary is not loaded.");
    }
    juce::String text;
    if (const auto result = decodeUtf8(lyrics, text); result.failed())
    {
        return result;
    }
    if (text.isEmpty())
    {
        return juce::Result::fail("Dictionary lookup key is empty.");
    }
    if (text.containsAnyOf(" \t\r\n"))
    {
        return juce::Result::fail("Dictionary lookup requires exactly one key without surrounding whitespace.");
    }
    if (isJapanese)
    {
        return lookupJapanese(text, output);
    }
    if (isKorean)
    {
        return lookupKorean(text, output);
    }
    const auto normalized = isMandarin ? normalizePinyin(text) : (isEnglish ? text.toLowerCase() : juce::String{});
    const auto found = entries.find(normalized.isEmpty() ? std::string(lyrics) : normalized.toStdString());
    if (found == entries.end())
    {
        if (isMandarin)
        {
            return juce::Result::fail("No Mandarin pronunciation for '" + text + "'. Use one dictionary-listed syllable per note: a Chinese character or pinyin (optional tone 1-5). Split multi-character lyrics across notes, or enter phonemes explicitly.");
        }
        return juce::Result::fail("No pronunciation for dictionary key '" + text + "'.");
    }
    output = found->second;
    return juce::Result::ok();
}

juce::Result PhonemeDictionary::loadJapaneseKana(const juce::File& directory)
{
    std::unordered_map<std::string, std::string> loaded;
    for (const auto* name : {"japanese-hira2romaji-dict.txt", "japanese-kata2romaji-dict.txt", "japanese-sute2romaji-dict.txt"})
    {
        const auto file = directory.getChildFile(name);
        if (!file.existsAsFile())
        {
            continue;
        }
        juce::String text;
        if (const auto result = readText(file, text); result.failed())
        {
            return result;
        }
        const auto lines = juce::StringArray::fromLines(text);
        for (int line = 0; line < lines.size(); ++line)
        {
            const auto fields = splitFields(lines[line]);
            if (fields.isEmpty())
            {
                continue;
            }
            if (fields.size() != 2)
            {
                return lineError(file, line + 1, "Expected exactly two fields: kana and romaji.");
            }
            loaded.emplace(fields[0].toStdString(), fields[1].toStdString());
        }
    }
    if (loaded.empty())
    {
        return juce::Result::fail("No Japanese kana maps found in " + directory.getFullPathName() + ".");
    }
    kana = std::move(loaded);
    isJapanese = true;
    return juce::Result::ok();
}

juce::Result PhonemeDictionary::loadKorean(const juce::File& directory)
{
    const auto file = directory.getChildFile("korean-dict.txt");
    if (!file.existsAsFile())
    {
        return juce::Result::fail("No Korean syllable map found in " + directory.getFullPathName() + ".");
    }
    juce::String text;
    if (const auto result = readText(file, text); result.failed())
    {
        return result;
    }
    std::unordered_map<std::string, std::string> loaded;
    const auto lines = juce::StringArray::fromLines(text);
    for (int line = 0; line < lines.size(); ++line)
    {
        const auto fields = splitFields(lines[line]);
        if (fields.isEmpty())
        {
            continue;
        }
        if (fields.size() != 2)
        {
            return lineError(file, line + 1, "Expected exactly two fields: Hangul and romanization.");
        }
        loaded.emplace(fields[0].toStdString(), fields[1].toStdString());
    }
    if (loaded.empty())
    {
        return juce::Result::fail("The Korean syllable map is empty: " + file.getFullPathName());
    }
    kana = std::move(loaded);
    isKorean = true;
    return juce::Result::ok();
}

namespace
{
std::string contractSyllable(const std::string& syllable, const std::string& glide)
{
    // し and ち palatalise by shortening, every other i-final syllable by
    // dropping the trailing vowel, so しゃ is "sha" but きゃ is "kya".
    if (syllable == "chi")
    {
        return glide == "ya" ? "cha" : (glide == "yu" ? "chu" : "cho");
    }
    if (syllable == "shi")
    {
        return glide == "ya" ? "sha" : (glide == "yu" ? "shu" : "sho");
    }
    if (!syllable.empty() && syllable.back() == 'i')
    {
        return syllable.substr(0, syllable.size() - 1) + glide;
    }
    return syllable + glide;
}
} // namespace

juce::Result PhonemeDictionary::lookupJapanese(const juce::String& text, std::vector<std::string>& output) const
{
    if (const auto direct = entries.find(text.toStdString()); direct != entries.end())
    {
        output = direct->second;
        return juce::Result::ok();
    }

    std::vector<juce::juce_wchar> characters;
    characters.reserve(static_cast<std::size_t>(text.length()));
    for (const auto character : text)
    {
        characters.push_back(character);
    }
    auto romaji = [&](juce::juce_wchar character) -> std::string {
        const auto found = kana.find(juce::String::charToString(character).toStdString());
        return found == kana.end() ? std::string{} : found->second;
    };

    std::vector<std::string> result;
    bool sokuon = false;
    std::string previous;
    for (std::size_t index = 0; index < characters.size(); ++index)
    {
        const auto character = characters[index];
        if (character == 0x3063 || character == 0x30c3) // っ ッ
        {
            sokuon = true;
            continue;
        }
        if (character == 0x30fc) // ー
        {
            if (previous.empty() || std::string_view("aiueo").find(previous.back()) == std::string_view::npos)
            {
                return juce::Result::fail("The lyric '" + text + "' prolongs a sound without a preceding vowel; enter phonemes explicitly.");
            }
            result.push_back(std::string(1, previous.back()));
            sokuon = false;
            continue;
        }
        auto syllable = romaji(character);
        if (syllable.empty())
        {
            return juce::Result::fail("The lyric '" + text + "' contains a character the Japanese lexicon cannot read; enter phonemes explicitly.");
        }
        if (index + 1 < characters.size())
        {
            const auto glide = romaji(characters[index + 1]);
            if (glide == "ya" || glide == "yu" || glide == "yo")
            {
                syllable = contractSyllable(syllable, glide);
                ++index;
            }
        }
        const auto found = entries.find(syllable);
        if (found == entries.end())
        {
            return juce::Result::fail("No pronunciation for '" + juce::String(syllable) + "' in the lyric '" + text + "'.");
        }
        if (sokuon)
        {
            result.push_back("cl");
            sokuon = false;
        }
        result.insert(result.end(), found->second.begin(), found->second.end());
        previous = syllable;
    }
    if (sokuon)
    {
        result.push_back("cl");
    }
    if (result.empty())
    {
        return juce::Result::fail("The lyric '" + text + "' produced no phonemes.");
    }
    output = std::move(result);
    return juce::Result::ok();
}

juce::Result PhonemeDictionary::lookupKorean(const juce::String& text, std::vector<std::string>& output) const
{
    if (const auto direct = entries.find(text.toStdString()); direct != entries.end())
    {
        output = direct->second;
        return juce::Result::ok();
    }

    std::vector<std::string> result;
    for (const auto character : text)
    {
        const auto key = juce::String::charToString(character).toStdString();
        std::string syllable;
        if (const auto mapped = kana.find(key); mapped != kana.end())
        {
            syllable = mapped->second;
        }
        else if (entries.contains(key))
        {
            syllable = key; // already-romanized input
        }
        else
        {
            return juce::Result::fail("The lyric '" + text + "' contains a character the Korean lexicon cannot read; enter phonemes explicitly.");
        }
        const auto found = entries.find(syllable);
        if (found == entries.end())
        {
            return juce::Result::fail("No pronunciation for '" + juce::String::fromUTF8(syllable.c_str()) + "' in the lyric '" + text + "'.");
        }
        result.insert(result.end(), found->second.begin(), found->second.end());
    }
    if (result.empty())
    {
        return juce::Result::fail("The lyric '" + text + "' produced no phonemes.");
    }
    output = std::move(result);
    return juce::Result::ok();
}

juce::Result PhonemeDictionary::parseExplicitPhonemes(std::string_view text, std::vector<std::string>& output) const
{
    if (!isLoaded())
    {
        return juce::Result::fail("Phoneme dictionary is not loaded.");
    }
    juce::String decoded;
    if (const auto result = decodeUtf8(text, decoded); result.failed())
    {
        return result;
    }
    auto fields = juce::StringArray::fromTokens(decoded, " \t\r\n", "");
    fields.removeEmptyStrings();
    if (fields.isEmpty())
    {
        return juce::Result::fail("Explicit phoneme sequence is empty.");
    }
    std::vector<std::string> parsed;
    parsed.reserve(static_cast<std::size_t>(fields.size()));
    for (const auto& field : fields)
    {
        const auto symbol = field.toStdString();
        if (!symbols.contains(symbol))
        {
            return juce::Result::fail("Unknown explicit phoneme '" + field + "'.");
        }
        parsed.push_back(symbol);
    }
    output = std::move(parsed);
    return juce::Result::ok();
}

bool PhonemeDictionary::isLoaded() const
{
    return !phonemes.empty() && !entries.empty();
}

const std::vector<PhonemeDefinition>& PhonemeDictionary::getPhonemes() const
{
    return phonemes;
}

std::size_t PhonemeDictionary::getEntryCount() const
{
    return entries.size();
}
} // namespace sv::synthesis
