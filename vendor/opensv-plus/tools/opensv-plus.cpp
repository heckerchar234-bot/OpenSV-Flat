#include <opensv/opensv.h>
#include <opensv/opensv.hpp>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {

constexpr std::size_t maxRequestBytes = 16 * 1024 * 1024;

struct Options {
    std::map<std::string, std::string> values;
};

std::string utf8(const juce::String& value) {
    return value.toStdString();
}

juce::File path(const std::string& value) {
    return juce::File(juce::String::fromUTF8(value.c_str()));
}

juce::Result parseOptions(const std::vector<std::string>& args, Options& options) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& key = args[i];
        if (key == "--help" || key == "-h" || key == "--version" || key == "--api-version") {
            options.values[key] = "";
            continue;
        }
        if (key.size() < 3 || key.substr(0, 2) != "--")
            return juce::Result::fail("Unexpected argument: " + key);
        if (i + 1 >= args.size() || args[i + 1].substr(0, 2) == "--")
            return juce::Result::fail("Expected a value after " + key);
        if (!options.values.emplace(key, args[++i]).second)
            return juce::Result::fail("Option was supplied more than once: " + key);
    }
    return juce::Result::ok();
}

std::string option(const Options& options, const std::string& name,
                   const std::string& fallback = {}) {
    const auto found = options.values.find(name);
    return found == options.values.end() ? fallback : found->second;
}

juce::Result readRequest(const Options& options, juce::String& json) {
    const auto filename = option(options, "--input", "-");
    if (filename == "-") {
        std::string text{std::istreambuf_iterator<char>(std::cin),
                         std::istreambuf_iterator<char>()};
        if (text.size() > maxRequestBytes)
            return juce::Result::fail("The JSON request exceeds 16 MiB.");
        json = juce::String::fromUTF8(text.c_str(), static_cast<int>(text.size()));
    } else {
        const auto file = path(filename);
        if (!file.existsAsFile())
            return juce::Result::fail("Input file does not exist: " + file.getFullPathName());
        if (file.getSize() < 0 || file.getSize() > static_cast<juce::int64>(maxRequestBytes))
            return juce::Result::fail("The JSON request exceeds 16 MiB.");
        json = file.loadFileAsString();
    }
    if (juce::JSON::parse(json).isVoid())
        return juce::Result::fail("Input is not valid JSON.");
    return juce::Result::ok();
}

bool write32(juce::FileOutputStream& output, std::uint32_t value) {
    const std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
    return output.write(bytes.data(), bytes.size());
}

bool write16(juce::FileOutputStream& output, std::uint16_t value) {
    const std::array<std::uint8_t, 2> bytes{
        static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8)};
    return output.write(bytes.data(), bytes.size());
}

bool writeFourCC(juce::FileOutputStream& output, const char* value) {
    return output.write(value, 4);
}

juce::Result writeWave(const juce::File& file, const float* samples, int count,
                       double sampleRate) {
    if (samples == nullptr || count <= 0 || !std::isfinite(sampleRate) ||
        sampleRate < 1.0 || sampleRate > static_cast<double>((std::numeric_limits<std::uint32_t>::max)() / sizeof(float)))
        return juce::Result::fail("The render result contains no valid audio samples.");
    const auto dataBytes = static_cast<std::uint64_t>(count) * sizeof(float);
    if (dataBytes > std::numeric_limits<std::uint32_t>::max() - 36)
        return juce::Result::fail("The rendered WAV exceeds the RIFF file-size limit.");
    auto output = file.createOutputStream();
    if (output == nullptr)
        return juce::Result::fail("Cannot open WAV output: " + file.getFullPathName());
    if (!output->setPosition(0) || output->truncate().failed())
        return juce::Result::fail("Cannot replace the existing WAV output: " + file.getFullPathName());
    const auto rate = static_cast<std::uint32_t>(sampleRate);
    const bool headerOk = writeFourCC(*output, "RIFF") &&
        write32(*output, static_cast<std::uint32_t>(dataBytes + 36)) &&
        writeFourCC(*output, "WAVE") && writeFourCC(*output, "fmt ") &&
        write32(*output, 16) && write16(*output, 3) && write16(*output, 1) &&
        write32(*output, rate) && write32(*output, rate * sizeof(float)) &&
        write16(*output, sizeof(float)) && write16(*output, 32) &&
        writeFourCC(*output, "data") && write32(*output, static_cast<std::uint32_t>(dataBytes));
    if (!headerOk) return juce::Result::fail("Cannot write the WAV header.");
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(samples[i])) return juce::Result::fail("Render output contains a non-finite audio sample.");
        const auto bits = std::bit_cast<std::uint32_t>(samples[i]);
        if (!write32(*output, bits)) return juce::Result::fail("Cannot write the WAV audio data.");
    }
    output->flush();
    return output->getStatus();
}

juce::Result writeText(const std::string& destination, const juce::String& text) {
    if (destination.empty() || destination == "-") {
        std::cout << utf8(text) << '\n';
        return std::cout.good() ? juce::Result::ok() : juce::Result::fail("Cannot write JSON to stdout.");
    }
    const auto file = path(destination);
    if (!file.replaceWithText(text + "\n"))
        return juce::Result::fail("Cannot write JSON output: " + file.getFullPathName());
    return juce::Result::ok();
}

juce::Result inspectVoice(const Options& options) {
    const auto voiceName = option(options, "--voice");
    if (voiceName.empty()) return juce::Result::fail("inspect requires --voice <info.nofs>.");
    const auto modelsName = option(options, "--models");
    sv::synthesis::VoiceDatabase database;
    const auto opened = database.open(path(voiceName), modelsName.empty() ? juce::File() : path(modelsName));
    if (opened.failed()) return opened;

    auto* object = new juce::DynamicObject();
    const auto& metadata = database.getMetadata();
    object->setProperty("name", metadata.name);
    object->setProperty("vendor", metadata.vendor);
    object->setProperty("version", metadata.version);
    object->setProperty("language", metadata.language);
    object->setProperty("phoneset", metadata.phoneset);
    auto languages = juce::Array<juce::var>();
    for (const auto& language : metadata.languages) languages.add(language);
    object->setProperty("languages", languages);
    auto styles = juce::Array<juce::var>();
    for (const auto& style : metadata.timbreStyles) styles.add(style);
    object->setProperty("styles", styles);
    object->setProperty("documentFormat", database.hasDocument() ? "json" : "nofs");
    auto models = new juce::DynamicObject();
    for (const auto* field : {"duration", "f0", "acoustic", "vocoder"}) {
        juce::String id;
        if (database.hasDocument()) id = database.getDocument()[field].toString();
        else {
            sv::synthesis::VoiceConfiguration config;
            if (config.load(database).wasOk()) {
                if (std::string(field) == "duration") id = juce::String::fromUTF8(config.getDurationModel().name.c_str());
                else if (std::string(field) == "acoustic") id = juce::String::fromUTF8(config.getAcousticModel().name.c_str());
                else if (std::string(field) == "vocoder") id = juce::String::fromUTF8(config.getVocoderModel().name.c_str());
            }
        }
        if (id.isEmpty()) continue;
        auto value = new juce::DynamicObject();
        value->setProperty("id", id);
        const auto found = std::find_if(database.getEntries().begin(), database.getEntries().end(),
            [&id](const auto& entry) { return entry.name == id; });
        const bool available = found != database.getEntries().end() &&
            (found->external.existsAsFile() || found->valueSize > 0);
        value->setProperty("available", available);
        if (found != database.getEntries().end() && found->external.existsAsFile())
            value->setProperty("path", found->external.getFullPathName());
        models->setProperty(field, juce::var(value));
    }
    object->setProperty("models", juce::var(models));
    object->setProperty("apiVersion", opensv_api_version());
    return writeText(option(options, "--response", "-"), juce::JSON::toString(juce::var(object), true));
}

juce::Result runOperation(const Options& options) {
    const auto voice = option(options, "--voice");
    if (voice.empty()) return juce::Result::fail("run requires --voice <info.nofs>.");
    const auto operation = option(options, "--operation");
    if (operation != "duration" && operation != "pitch" && operation != "render")
        return juce::Result::fail("--operation must be duration, pitch or render.");
    if (!option(options, "--wav").empty() && operation != "render")
        return juce::Result::fail("--wav is available with --operation render.");
    juce::String request;
    if (auto result = readRequest(options, request); result.failed()) return result;

    std::array<char, 4096> error{};
    const auto models = option(options, "--models");
    auto* handle = opensv_create(voice.c_str(), models.empty() ? nullptr : models.c_str(), error.data(), error.size());
    if (handle == nullptr) {
        const auto message = error[0] == '\0' ? "Unable to load the voice." : error.data();
        return juce::Result::fail(juce::String::fromUTF8(message));
    }
    auto* result = opensv_execute(handle, operation.c_str(), utf8(request).c_str(), nullptr, nullptr);
    opensv_destroy(handle);
    if (result == nullptr) return juce::Result::fail("The engine returned an empty response.");
    const juce::String response = juce::String::fromUTF8(opensv_result_json(result));
    const auto document = juce::JSON::parse(response);
    if (document.hasProperty("error")) {
        const auto message = document["error"].toString();
        opensv_result_free(result);
        return juce::Result::fail(message);
    }
    if (operation == "render" && !option(options, "--wav").empty()) {
        const auto output = path(option(options, "--wav"));
        const auto status = writeWave(output, opensv_result_samples(result),
                                      opensv_result_count(result), opensv_result_rate(result));
        if (status.failed()) {
            opensv_result_free(result);
            return status;
        }
    }
    const auto writeStatus = writeText(option(options, "--response", "-"), response);
    opensv_result_free(result);
    return writeStatus;
}

void printHelp() {
    std::cout <<
        "OpenSV+ native singing engine\n"
        "\n"
        "Usage:\n"
        "  opensv-plus --version\n"
        "  opensv-plus --api-version\n"
        "  opensv-plus inspect --voice info.nofs [--models MODEL-DIRECTORY]\n"
        "  opensv-plus run --voice info.nofs [--models MODEL-DIRECTORY]\n"
        "      --operation duration|pitch|render [--input request.json]\n"
        "      [--response result.json] [--wav audio.wav]\n"
        "\n"
        "A JSON request is read from standard input when --input is omitted or is '-'.\n"
        "The duration, pitch and render requests use the OpenSV v3 C ABI JSON schema.\n"
        "Use --wav with render to write 32-bit float mono WAV audio.\n";
}

int run(const std::vector<std::string>& args) {
    if (args.empty()) {
        printHelp();
        return 0;
    }
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        printHelp();
        return 0;
    }
    if (args.size() == 1 && args[0] == "--version") {
        std::cout << "OpenSV+ 1.0.0\n";
        return 0;
    }
    if (args.size() == 1 && args[0] == "--api-version") {
        std::cout << opensv_api_version() << '\n';
        return 0;
    }
    const auto& command = args.front();
    if (command != "inspect" && command != "run") {
        std::cerr << "Command must be inspect or run.\n";
        return 2;
    }
    std::vector<std::string> commandArgs(args.begin() + 1, args.end());
    Options commandOptions;
    const auto commandParse = parseOptions(commandArgs, commandOptions);
    if (commandParse.failed()) {
        std::cerr << commandParse.getErrorMessage() << '\n';
        return 2;
    }
    juce::Result result = command == "inspect"
        ? inspectVoice(commandOptions) : runOperation(commandOptions);
    if (result.failed()) {
        std::cerr << result.getErrorMessage() << '\n';
        return 1;
    }
    return 0;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0) return 2;
        std::string value(static_cast<std::size_t>(length), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), length, nullptr, nullptr) <= 0) return 2;
        value.resize(static_cast<std::size_t>(length - 1));
        args.push_back(std::move(value));
    }
    return run(args);
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return run(args);
}
#endif
