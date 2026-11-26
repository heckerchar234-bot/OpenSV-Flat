#include "DnniDump.h"

#include <mutex>

namespace sv::synthesis::dnnidump
{
namespace
{
thread_local const char* currentScope = nullptr;

struct State
{
    std::mutex mutex;
    bool resolved = false;
    juce::File directory;
    std::uint64_t sequence = 0;
};

State& state()
{
    static State value;
    return value;
}

juce::File resolveDirectory()
{
    const auto path = juce::SystemStats::getEnvironmentVariable("OPENSVDUMP", {});
    if (path.isEmpty())
    {
        return {};
    }
    auto directory = juce::File(path);
    if (!directory.isDirectory())
    {
        if (const auto created = directory.createDirectory(); created.failed())
        {
            return {};
        }
    }
    return directory;
}

juce::String identifier(const juce::String& text)
{
    juce::String result;
    for (const auto character : text)
    {
        const bool allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_';
        result += allowed ? juce::String::charToString(character) : juce::String("_");
    }
    return result.isEmpty() ? juce::String("unknown") : result;
}

void writeTensor(const juce::File& file, const DnniTensor& tensor)
{
    file.replaceWithData(tensor.values.data(), tensor.values.size() * sizeof(float));
}

juce::String shape(const DnniTensor& tensor)
{
    return "{\"frames\":" + juce::String(static_cast<juce::int64>(tensor.frames))
        + ",\"channels\":" + juce::String(static_cast<juce::int64>(tensor.channels)) + "}";
}
} // namespace

Scope::Scope(const char* name)
    : previous(currentScope)
{
    currentScope = name;
}

Scope::~Scope()
{
    currentScope = previous;
}

void record(const DnniTensor& input, const DnniTensor* condition, const DnniTensor& output, std::size_t networkOffset)
{
    auto& shared = state();
    std::lock_guard<std::mutex> lock(shared.mutex);
    if (!shared.resolved)
    {
        shared.resolved = true;
        shared.directory = resolveDirectory();
    }
    if (!shared.directory.isDirectory())
    {
        return;
    }

    const auto scope = identifier(currentScope == nullptr ? juce::String("unknown") : juce::String(currentScope));
    const auto network = juce::String::toHexString(static_cast<juce::int64>(networkOffset));
    const auto stem = juce::String::formatted("%06llu", static_cast<unsigned long long>(shared.sequence)) + "_" + scope + "_" + network;

    const auto inputName = stem + ".in.f32";
    const auto outputName = stem + ".out.f32";
    writeTensor(shared.directory.getChildFile(inputName), input);
    writeTensor(shared.directory.getChildFile(outputName), output);
    juce::String conditionName;
    if (condition != nullptr)
    {
        conditionName = stem + ".cond.f32";
        writeTensor(shared.directory.getChildFile(conditionName), *condition);
    }

    juce::String line = "{\"scope\":" + scope.quoted() + ",\"seq\":" + juce::String(static_cast<juce::int64>(shared.sequence))
        + ",\"network\":\"0x" + network + "\",\"input\":" + shape(input)
        + ",\"condition\":" + (condition == nullptr ? juce::String("null") : shape(*condition))
        + ",\"output\":" + shape(output)
        + ",\"files\":[" + inputName.quoted()
        + (conditionName.isEmpty() ? juce::String() : "," + conditionName.quoted())
        + "," + outputName.quoted() + "]}\n";

    const auto manifest = shared.directory.getChildFile("manifest.jsonl");
    juce::FileOutputStream stream(manifest, 64);
    if (!stream.openedOk())
    {
        return;
    }
    stream.setPosition(manifest.getSize());
    stream.writeText(line, false, false, "\n");
    ++shared.sequence;
}
} // namespace sv::synthesis::dnnidump
