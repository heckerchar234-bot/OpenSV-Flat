#include "VoiceEmbeddings.h"
#include <bit>
#include <cmath>
#include <cstdint>

namespace sv::synthesis
{
namespace
{
juce::Result decode(const juce::var& value, std::size_t channels, const char* field, std::vector<float>& output)
{
    const auto fail = [field]() { return juce::Result::fail("Voice embedding '" + juce::String(field) + "' must be a finite little-endian float32 hex vector of the expected length"); };
    if (!value.isString()) return fail();
    const auto text = value.toString();
    if (text.length() != static_cast<int>(channels * 8)) return fail();
    const auto nibble = [](juce::juce_wchar ch) -> int {
        if (ch >= '0' && ch <= '9') return static_cast<int>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<int>(ch - 'a') + 10;
        if (ch >= 'A' && ch <= 'F') return static_cast<int>(ch - 'A') + 10;
        return -1;
    };
    std::vector<float> result(channels);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        std::uint32_t word = 0;
        for (std::size_t byte = 0; byte < 4; ++byte) {
            const int index = static_cast<int>(channel * 8 + byte * 2);
            const int high = nibble(text[index]), low = nibble(text[index + 1]);
            if (high < 0 || low < 0) return fail();
            word |= static_cast<std::uint32_t>((high << 4) | low) << (byte * 8);
        }
        result[channel] = std::bit_cast<float>(word);
        if (!std::isfinite(result[channel])) return fail();
    }
    output = std::move(result);
    return juce::Result::ok();
}
}

juce::Result VoiceEmbeddings::load(const juce::var& document)
{
    if (!document.isObject()) return juce::Result::fail("Voice embeddings require a NOFS JSON object");
    VoiceEmbeddings candidate;
    if (document.hasProperty("styles")) {
        const auto* styles = document["styles"].getArray();
        if (styles == nullptr) return juce::Result::fail("NOFS styles must be an array");
        for (const auto& style : *styles) {
            const auto name = style["name"].toString();
            auto* target = name == "(base)" ? &candidate.acousticSpeaker : name == "(default)" ? &candidate.acousticStyle : nullptr;
            if (target == nullptr) {
                if (name.isEmpty() || name.startsWithChar('(')) continue;
                auto& vector = candidate.vocalModes[name.toStdString()];
                if (!vector.empty()) return juce::Result::fail("Duplicate NOFS vocal mode");
                if (const auto result = decode(style["data"], 32, name.toRawUTF8(), vector); result.failed()) return result;
                continue;
            }
            if (!target->empty()) return juce::Result::fail("Duplicate NOFS speaker/default embedding");
            if (const auto result = decode(style["data"], 32, name.toRawUTF8(), *target); result.failed()) return result;
        }
        if (candidate.acousticSpeaker.empty() != candidate.acousticStyle.empty())
            return juce::Result::fail("NOFS must provide both (base) and (default) embeddings");
    }
    if (document.hasProperty("f0_style")) {
        if (const auto result = decode(document["f0_style"], 32, "f0_style", candidate.pitchSpeaker); result.failed()) return result;
    }
    if (document.hasProperty("duration_style")) {
        if (const auto result = decode(document["duration_style"], 128, "duration_style", candidate.durationVoice); result.failed()) return result;
    }
    *this = std::move(candidate);
    return juce::Result::ok();
}
}
