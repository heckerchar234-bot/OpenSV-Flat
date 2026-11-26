#include "synthesis/VoiceSynthesizer.h"
#include <cmath>
#include <memory>
#include <stdexcept>

#include <opensv/opensv.h>

using namespace sv::synthesis;
namespace {
struct Response {
    std::string json;
    std::vector<float> samples;
    double sampleRate = 0;
};
void check(const juce::Result& result) {
    if (result.failed()) throw std::runtime_error(result.getErrorMessage().toStdString());
}
std::vector<float> curve(const juce::var& value, std::size_t frames, float minimum, float maximum) {
    if (value.isVoid()) return {};
    const auto* values = value.getArray();
    if (!values || values->size() > 120400 || (frames != 0 && static_cast<std::size_t>(values->size()) != frames))
        throw std::runtime_error("Invalid parameter curve dimensions");
    std::vector<float> result;
    for (const auto& item : *values) {
        const float number = static_cast<float>(item);
        if (!std::isfinite(number) || number < minimum || number > maximum) throw std::runtime_error("Parameter curve value is out of range");
        result.push_back(number);
    }
    return result;
}
std::vector<TimingSyllable> syllables(const juce::var& request) {
    const auto* notes = request["notes"].getArray();
    if (!notes || notes->isEmpty() || notes->size() > 10000) throw std::runtime_error("Expected 1..10000 notes");
    std::vector<TimingSyllable> output;
    double duration = 0;
    for (const auto& note : *notes) {
        TimingSyllable item;
        item.language = note["language"].toString().toStdString();
        item.durationSeconds = static_cast<double>(note["duration"]);
        item.midiPitch = static_cast<int>(note["tone"]);
        const auto* phones = note["phones"].getArray();
        if (!phones || phones->isEmpty() || phones->size() > 256 || !std::isfinite(item.durationSeconds) || item.durationSeconds <= 0 || item.midiPitch < 0 || item.midiPitch > 127)
            throw std::runtime_error("Invalid note duration, pitch or phonemes");
        duration += item.durationSeconds;
        if (duration > 600) throw std::runtime_error("Phrase exceeds 600 seconds");
        for (const auto& phone : *phones) item.phonemes.push_back(phone.toString().toStdString());
        output.push_back(std::move(item));
    }
    return output;
}
Response* error(const std::string& message) {
    auto output = std::make_unique<Response>();
    auto object = new juce::DynamicObject();
    object->setProperty("error", juce::String::fromUTF8(message.c_str()));
    output->json = juce::JSON::toString(juce::var(object)).toStdString();
    return output.release();
}
}

OPENSV_API int opensv_api_version(void) { return 3; }
OPENSV_API void* opensv_create(const char* voice, const char* models, char* errorBuffer, int errorCapacity) {
    try {
        auto output = std::make_unique<VoiceSynthesizer>();
        const auto voicePath = voice == nullptr ? juce::String() : juce::String::fromUTF8(voice);
        const auto modelsPath = models == nullptr ? juce::String() : juce::String::fromUTF8(models);
        check(output->load(juce::File(voicePath), juce::File(modelsPath)));
        return output.release();
    } catch (const std::exception& exception) {
        if (errorBuffer && errorCapacity > 0) juce::String::fromUTF8(exception.what()).copyToUTF8(errorBuffer, static_cast<std::size_t>(errorCapacity));
        return nullptr;
    } catch (...) { return nullptr; }
}
OPENSV_API void opensv_destroy(void* handle) { delete static_cast<VoiceSynthesizer*>(handle); }
OPENSV_API void* opensv_execute(void* handle, const char* operation, const char* json, int (*cancelled)(void*), void* cancellationContext) {
    try {
        if (!handle || !operation || !json) throw std::runtime_error("Invalid native request");
        auto& engine = *static_cast<VoiceSynthesizer*>(handle);
        auto request = juce::JSON::parse(juce::String::fromUTF8(json));
        if (!request.isObject()) throw std::runtime_error("Expected JSON object");
        auto output = std::make_unique<Response>();
        auto object = new juce::DynamicObject();
        juce::var document(object);
        const auto shouldCancel = [cancelled, cancellationContext] { return cancelled && cancelled(cancellationContext) != 0; };
        if (shouldCancel()) throw std::runtime_error("Cancelled");
        const std::string action(operation);
        if (action == "duration") {
            auto notes = syllables(request);
            std::vector<PhonemeDuration> durations;
            check(engine.predict(notes, durations));
            juce::Array<juce::var> phones;
            for (const auto& item : durations) {
                auto phone = new juce::DynamicObject();
                phone->setProperty("language", juce::String(item.language));
                phone->setProperty("symbol", juce::String(item.symbol));
                phone->setProperty("note", static_cast<int>(item.syllableIndex));
                phone->setProperty("duration", item.durationSeconds);
                phones.add(juce::var(phone));
            }
            object->setProperty("phones", phones);
        } else if (action == "pitch") {
            auto score = syllables(request);
            std::vector<PitchNote> notes;
            std::size_t index = 0;
            for (auto& item : score) {
                const auto& raw = (*request["notes"].getArray())[static_cast<int>(index++)];
                PitchNote note;
                note.isSilence = item.phonemes.size() == 1 && item.phonemes[0] == "sil";
                note.isBreath = item.phonemes.size() == 1 && item.phonemes[0] == "br";
                note.isContinuation = static_cast<bool>(raw["continuation"]);
                note.isRap = raw["mode"].toString() == "rap";
                note.vibratoModulation = raw.hasProperty("vibrato") ? static_cast<float>(raw["vibrato"]) : 1.0f;
                note.intonation = raw["intonation"].isVoid() ? std::numeric_limits<float>::quiet_NaN() : static_cast<float>(raw["intonation"]);
                note.rapTone = raw["rapTone"].isVoid() ? std::numeric_limits<float>::quiet_NaN() : static_cast<float>(raw["rapTone"]);
                note.tone = static_cast<int>(raw["lexicalTone"]);
                if (note.vibratoModulation < 0 || note.vibratoModulation > 2) throw std::runtime_error("Vibrato modulation must be 0..2");
                note.syllable = std::move(item);
                notes.push_back(std::move(note));
            }
            std::vector<float> pitch;
            auto envelope = curve(request["vibratoEnvelope"], 0, 0, 2);
            PitchControls predicted;
            std::vector<PhonemeDuration> supplied;
            if (!request["phones"].isVoid()) {
                const auto* phones = request["phones"].getArray();
                if (!phones || phones->isEmpty() || phones->size() > 100000) throw std::runtime_error("Invalid pitch phoneme timing");
                for (const auto& raw : *phones) {
                    const int owner = static_cast<int>(raw["note"]);
                    const double seconds = static_cast<double>(raw["duration"]);
                    if (owner < 0 || owner >= static_cast<int>(notes.size()) + 2 || !std::isfinite(seconds) || seconds <= 0) throw std::runtime_error("Invalid pitch phoneme interval");
                    supplied.push_back({raw["language"].toString().toStdString(), raw["symbol"].toString().toStdString(), static_cast<std::size_t>(owner), 0, seconds});
                }
            }
            check(engine.predictPitch(notes, pitch, shouldCancel, nullptr, nullptr, envelope, &predicted, supplied));
            juce::Array<juce::var> intonation, tone;
            for (const auto value : predicted.intonation) intonation.add(value);
            for (const auto value : predicted.tone) tone.add(value);
            object->setProperty("intonation", intonation); object->setProperty("rapTone", tone);
            juce::Array<juce::var> values;
            for (float value : pitch) values.add(value);
            object->setProperty("pitch", values);
            object->setProperty("frameMs", engine.getPitchFrameIntervalSeconds() * 1000.0);
            object->setProperty("leadingMs", VoiceSynthesizer::pitchContextSeconds * 1000.0);
        } else if (action == "render") {
            const auto* rawPhones = request["phones"].getArray();
            const auto* pitch = request["pitch"].getArray();
            if (!rawPhones || rawPhones->isEmpty() || rawPhones->size() > 100000 || !pitch || pitch->isEmpty() || pitch->size() > 120000)
                throw std::runtime_error("Invalid rendering dimensions");
            std::vector<TimedPhoneme> phones;
            std::size_t frames = 0;
            for (const auto& item : *rawPhones) {
                const int count = static_cast<int>(item["frames"]);
                if (count <= 0 || count > 120000 || frames + static_cast<std::size_t>(count) > 120000) throw std::runtime_error("Invalid phoneme frame count");
                phones.push_back({item["language"].toString().toStdString(), item["symbol"].toString().toStdString(), static_cast<std::size_t>(count)});
                frames += static_cast<std::size_t>(count);
            }
            if (frames != static_cast<std::size_t>(pitch->size())) throw std::runtime_error("Pitch frames do not match phonemes");
            std::vector<float> logF0;
            for (const auto& value : *pitch) {
                const double midi = static_cast<double>(value);
                if (!std::isfinite(midi) || midi < -24 || midi > 160) throw std::runtime_error("Invalid MIDI pitch");
                logF0.push_back(static_cast<float>(std::log(440.0) + (midi - 69.0) * std::log(2.0) / 12.0));
            }
            NeuralVocoderOutput audio;
            const auto params = request["controls"];
            auto tension = curve(params["tension"], frames, -1, 1);
            auto breathiness = curve(params["breathiness"], frames, -1, 1);
            auto gender = curve(params["gender"], frames, -1, 1);
            auto toneShift = curve(params["toneShift"], frames, -400, 400);
            auto voicing = curve(params["voicing"], frames, 0, 1);
            SynthesisControls controls{tension, breathiness, gender, toneShift, voicing, {}};
            std::map<std::string, std::vector<float>> modeValues;
            if (const auto* modes = params["vocalModes"].getDynamicObject()) {
                for (const auto& property : modes->getProperties())
                    modeValues[property.name.toString().toStdString()] = curve(property.value, frames, 0, 1.5f);
                for (const auto& [name, weights] : modeValues) controls.vocalModes[name] = weights;
            }
            check(engine.render(phones, logF0, audio, 5489, shouldCancel, nullptr, nullptr, controls));
            output->samples = std::move(audio.samples);
            output->sampleRate = audio.sampleRate;
            object->setProperty("frameMs", engine.getFrameIntervalSeconds() * 1000.0);
        } else throw std::runtime_error("Unknown native operation");
        if (shouldCancel()) throw std::runtime_error("Cancelled");
        output->json = juce::JSON::toString(document).toStdString();
        return output.release();
    } catch (const std::exception& exception) { return error(exception.what()); }
    catch (...) { return error("Unexpected native inference failure"); }
}
OPENSV_API const char* opensv_result_json(void* result) { return static_cast<Response*>(result)->json.c_str(); }
OPENSV_API const float* opensv_result_samples(void* result) { return static_cast<Response*>(result)->samples.data(); }
OPENSV_API int opensv_result_count(void* result) { return static_cast<int>(static_cast<Response*>(result)->samples.size()); }
OPENSV_API double opensv_result_rate(void* result) { return static_cast<Response*>(result)->sampleRate; }
OPENSV_API void opensv_result_free(void* result) { delete static_cast<Response*>(result); }
