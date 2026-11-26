#include "VoiceSynthesizer.h"

#include "AcousticFeatures.h"
#include "DnniDump.h"
#include "DnniReader.h"
#include "VoiceConfiguration.h"
#include "VoiceDatabase.h"
#include "VoiceEmbeddings.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <optional>
#include <utility>

namespace sv::synthesis
{
namespace
{
juce::Result readModel(VoiceDatabase& database, const ModelReference& reference, const char* architecture, const char* role, DnniReader& reader)
{
    const auto context = juce::String("Voice synthesizer ") + role + " model: ";
    if (reference.architecture != architecture)
    {
        return juce::Result::fail(context + "unsupported architecture '" + juce::String::fromUTF8(reference.architecture.c_str()) + "', expected '" + architecture + "'");
    }
    const auto& entries = database.getEntries();
    const auto entry = std::find_if(entries.begin(), entries.end(), [&reference](const auto& candidate)
                                    { return candidate.key == reference.key; });
    if (entry == entries.end())
    {
        return juce::Result::fail(context + "the configured NOFS entry is missing");
    }
    juce::MemoryBlock bytes;
    if (const auto result = database.readEntry(*entry, bytes); result.failed())
    {
        return juce::Result::fail(context + result.getErrorMessage());
    }
    if (const auto result = reader.load(std::move(bytes)); result.failed())
    {
        return juce::Result::fail(context + result.getErrorMessage());
    }
    return juce::Result::ok();
}

juce::Result readFrameInterval(const DnniReader& reader, float& frameIntervalSeconds)
{
    std::optional<std::size_t> featureNode;
    const auto& nodes = reader.getNodes();
    for (std::size_t index = 0; index < nodes.size(); ++index)
    {
        if (nodes[index].type != "_ftmfv3" && nodes[index].type != "_ftmfv2")
        {
            continue;
        }
        if (featureNode.has_value())
        {
            return juce::Result::fail("Voice synthesizer: acoustic model contains multiple _ftmfv3 feature configurations");
        }
        featureNode = index;
    }
    if (!featureNode.has_value())
    {
        return juce::Result::fail("Voice synthesizer: acoustic model has no supported _ftmfv3 feature configuration");
    }
    AcousticFeatures features;
    if (const auto result = features.load(reader, *featureNode); result.failed())
    {
        return result;
    }
    frameIntervalSeconds = features.getConfig().frameIntervalSeconds;
    return juce::Result::ok();
}
} // namespace

std::size_t VoiceSynthesizer::State::getBytes() const noexcept
{
    return pitch.getBytes() + acoustic.getBytes() + vocoder.getBytes();
}

juce::Result VoiceSynthesizer::load(const juce::File& voice, const juce::File& modelsRoot)
try
{
    VoiceDatabase database;
    if (const auto result = database.open(voice, modelsRoot); result.failed())
    {
        return result;
    }
    VoiceConfiguration configuration;
    if (const auto result = configuration.load(database); result.failed())
    {
        return result;
    }
    VoiceSynthesizer candidate;
    VoiceEmbeddings embeddings;
    if (database.hasDocument())
    {
        if (const auto result = embeddings.load(database.getDocument()); result.failed()) return result;
    }
    for (const auto& language : juce::StringArray::fromTokens(database.getMetadata().properties[".feature_rap_languages"], false))
    {
        candidate.rapLanguages.push_back(language.toStdString());
    }
    if (database.hasDocument()) {
        const auto& doc = database.getDocument();
        if (const auto* languages = doc[doc.hasProperty("rap_languages") ? "rap_languages" : "support_languages"].getArray()) {
            for (const auto& language : *languages) candidate.rapLanguages.push_back(language.toString().toStdString());
        }
    }
    candidate.defaultStyle = embeddings.acousticStyle;
    candidate.vocalModes = embeddings.vocalModes;
    {
        const auto* entry = database.findEntry("f0model-dds");
        if (entry == nullptr)
        {
            return juce::Result::fail("Voice synthesizer: the voice has no supported f0model-dds pitch model");
        }
        juce::MemoryBlock bytes;
        if (const auto result = database.readEntry(*entry, bytes); result.failed())
        {
            return result;
        }
        DnniReader reader;
        if (const auto result = reader.load(std::move(bytes)); result.failed())
        {
            return result;
        }
        if (const auto result = candidate.pitch.load(reader, 0, embeddings.pitchSpeaker); result.failed())
        {
            return juce::Result::fail("Voice synthesizer pitch model: " + result.getErrorMessage());
        }
    }
    {
        DnniReader reader;
        if (const auto result = readModel(database, configuration.getDurationModel(), "gen2a", "duration", reader); result.failed())
        {
            return result;
        }
        if (const auto result = candidate.timing.load(reader, 0, embeddings.durationVoice); result.failed())
        {
            return result;
        }
    }
    {
        DnniReader reader;
        if (const auto result = readModel(database, configuration.getAcousticModel(), "dds", "acoustic", reader); result.failed())
        {
            return result;
        }
        if (const auto result = candidate.acoustic.load(reader, 0, embeddings.acousticSpeaker, embeddings.acousticStyle); result.failed())
        {
            return result;
        }
        if (const auto result = readFrameInterval(reader, candidate.frameIntervalSeconds); result.failed())
        {
            return result;
        }
    }
    {
        DnniReader reader;
        if (const auto result = readModel(database, configuration.getVocoderModel(), "nhv", "vocoder", reader); result.failed())
        {
            return result;
        }
        if (const auto result = candidate.vocoder.load(reader); result.failed())
        {
            return result;
        }
    }
    *this = std::move(candidate);
    return juce::Result::ok();
}
catch (const std::bad_alloc&)
{
    return juce::Result::fail("Voice synthesizer: insufficient memory to load the voice");
}

float VoiceSynthesizer::getFrameIntervalSeconds() const noexcept
{
    return frameIntervalSeconds;
}

float VoiceSynthesizer::getPitchFrameIntervalSeconds() const noexcept
{
    return pitch.getFrameIntervalSeconds();
}

juce::Result VoiceSynthesizer::predictPitch(std::span<const PitchNote> notes, std::vector<float>& midiPitch, const std::function<bool()>& shouldCancel, SynthesisStatistics* statistics, State* state, std::span<const float> vibratoEnvelope, PitchControls* predictedControls, std::span<const PhonemeDuration> suppliedPhonemes) const
try
{
    if (frameIntervalSeconds == 0.0f || notes.empty())
    {
        return juce::Result::fail("Pitch prediction requires a loaded voice and score notes.");
    }
    if (shouldCancel && shouldCancel())
    {
        return juce::Result::fail("Pitch prediction was cancelled.");
    }
    const double started = juce::Time::getMillisecondCounterHiRes();
    DnniRunStatistics networkStatistics;
    PitchNote leading;
    leading.syllable = {notes.front().syllable.language, {"sil"}, pitchContextSeconds, notes.front().syllable.midiPitch};
    leading.isSilence = true;
    PitchNote trailing;
    trailing.syllable = {notes.back().syllable.language, {"sil"}, pitchContextSeconds, notes.back().syllable.midiPitch};
    trailing.isSilence = true;
    std::vector<PitchNote> supportedNotes;
    supportedNotes.reserve(notes.size() + 2);
    supportedNotes.push_back(std::move(leading));
    supportedNotes.insert(supportedNotes.end(), notes.begin(), notes.end());
    supportedNotes.push_back(std::move(trailing));
    std::vector<TimingSyllable> syllables;
    syllables.reserve(supportedNotes.size());
    for (auto& note : supportedNotes)
    {
        note.isRap = note.isRap && std::find(rapLanguages.begin(), rapLanguages.end(), note.syllable.language) != rapLanguages.end();
        syllables.push_back(note.syllable);
    }
    // 编辑器传入含首尾半秒静音的实际音素时序；独立调用仍预测自己的时序。
    std::vector<PhonemeDuration> phonemes;
    if (suppliedPhonemes.empty()) {
        if (const auto result = timing.predict(syllables, phonemes); result.failed()) return result;
    } else {
        phonemes.assign(suppliedPhonemes.begin(), suppliedPhonemes.end());
        double expected = 0, actual = 0;
        for (const auto& syllable : syllables) expected += syllable.durationSeconds;
        for (const auto& phone : phonemes) actual += phone.durationSeconds;
        if (std::abs(expected - actual) > 0.02) return juce::Result::fail("Pitch phoneme timing does not cover the score context");
    }
    const dnnidump::Scope pitchScope("pitch");
    const auto result = pitch.run(supportedNotes, phonemes, midiPitch, 0, state != nullptr ? &state->pitch : nullptr, shouldCancel, &networkStatistics, vibratoEnvelope, predictedControls);
    if (result.wasOk() && predictedControls != nullptr) {
        predictedControls->intonation.erase(predictedControls->intonation.begin()); predictedControls->intonation.pop_back();
        predictedControls->tone.erase(predictedControls->tone.begin()); predictedControls->tone.pop_back();
    }
    if (statistics != nullptr)
    {
        statistics->pitchMilliseconds += juce::Time::getMillisecondCounterHiRes() - started;
        statistics->pitchFrames += result.wasOk() && (state == nullptr || !state->pitch.reusedPrediction) ? midiPitch.size() : 0;
        statistics->dnniComputedFrames += networkStatistics.computedFrames;
        statistics->dnniReusedFrames += networkStatistics.reusedFrames;
        statistics->dnniContextFrames += networkStatistics.contextFrames;
    }
    return result;
}
catch (const std::bad_alloc&)
{
    return juce::Result::fail("Voice synthesizer: insufficient memory to predict pitch");
}

juce::Result VoiceSynthesizer::predict(std::span<const TimingSyllable> syllables, std::vector<PhonemeDuration>& output) const
try
{
    if (frameIntervalSeconds == 0.0f)
    {
        return juce::Result::fail("Voice synthesizer: no voice has been loaded");
    }
    return timing.predict(syllables, output);
}
catch (const std::bad_alloc&)
{
    return juce::Result::fail("Voice synthesizer: insufficient memory to predict phoneme timing");
}

juce::Result VoiceSynthesizer::render(std::span<const TimedPhoneme> phonemes, std::span<const float> logF0, NeuralVocoderOutput& output, std::uint32_t seed, const std::function<bool()>& shouldCancel, SynthesisStatistics* statistics, State* state, const SynthesisControls& controls) const
try
{
    DnniRunStatistics networkStatistics;
    const juce::ScopeGuard finishStatistics([statistics, &networkStatistics]
                                            {
        if (statistics != nullptr)
        {
            statistics->dnniComputedFrames += networkStatistics.computedFrames;
            statistics->dnniReusedFrames += networkStatistics.reusedFrames;
            statistics->dnniContextFrames += networkStatistics.contextFrames;
        } });
    if (frameIntervalSeconds == 0.0f)
    {
        return juce::Result::fail("Voice synthesizer: no voice has been loaded");
    }
    if (shouldCancel && shouldCancel())
    {
        return juce::Result::fail("Cancelled");
    }
    DnniTensor frames;
    DnniTensor style;
    if (!controls.vocalModes.empty()) {
        if (defaultStyle.size() != 32) return juce::Result::fail("Vocal modes require a NOFS default style");
        style = {logF0.size(), 32, std::vector<float>(logF0.size() * 32)};
        for (std::size_t frame = 0; frame < logF0.size(); ++frame) {
            std::copy(defaultStyle.begin(), defaultStyle.end(), style.values.begin() + static_cast<std::ptrdiff_t>(frame * 32));
        }
        for (const auto& [name, weights] : controls.vocalModes) {
            const auto mode = vocalModes.find(name);
            if (mode == vocalModes.end() || weights.size() != logF0.size()) return juce::Result::fail("Unknown vocal mode or invalid dimensions");
            // 命名模式保存的是偏移向量，可同时混合多个模式。
            for (std::size_t frame = 0; frame < logF0.size(); ++frame) for (std::size_t channel = 0; channel < 32; ++channel)
                style.values[frame * 32 + channel] += weights[frame] * mode->second[channel];
        }
    }
    const double acousticStarted = juce::Time::getMillisecondCounterHiRes();
    const dnnidump::Scope acousticScope("acoustic");
    const auto acousticResult = acoustic.run(phonemes, logF0, frames, seed, state != nullptr ? &state->acoustic : nullptr, shouldCancel, &networkStatistics, style.frames > 0 ? &style : nullptr, controls.toneShift);
    if (statistics != nullptr)
    {
        statistics->acousticMilliseconds += juce::Time::getMillisecondCounterHiRes() - acousticStarted;
        statistics->acousticFrames += acousticResult.wasOk() ? frames.frames : 0;
    }
    if (acousticResult.failed())
    {
        return acousticResult;
    }
    if (shouldCancel && shouldCancel())
    {
        return juce::Result::fail("Cancelled");
    }
    NeuralVocoderOutput candidate;
    const dnnidump::Scope vocoderScope("vocoder");
    std::vector<float> shape;
    for (const auto value : controls.tension) shape.push_back(-value);
    if (const auto result = vocoder.run(frames, candidate, shape, statistics, state != nullptr ? &state->vocoder : nullptr, shouldCancel, &networkStatistics, controls); result.failed())
    {
        return result;
    }
    if (shouldCancel && shouldCancel())
    {
        return juce::Result::fail("Cancelled");
    }
    output = std::move(candidate);
    return juce::Result::ok();
}
catch (const std::bad_alloc&)
{
    return juce::Result::fail("Voice synthesizer: insufficient memory to render the phrase");
}
} // namespace sv::synthesis
