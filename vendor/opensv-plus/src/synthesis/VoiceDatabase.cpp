#include "VoiceDatabase.h"
#include "ModelFiles.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>

namespace sv::synthesis
{
namespace
{
constexpr std::uint32_t nofsMagic = 0xf580;
constexpr std::uint32_t supportedVersion = 10;
constexpr std::uint16_t indexBlockType = 0x1000;
constexpr std::uint16_t valueBlockType = 1;
constexpr std::uint64_t fileHeaderSize = 16;
constexpr std::uint32_t maximumEntrySize = 512 * 1024 * 1024;
constexpr std::uint32_t maximumMetadataSize = 1024 * 1024;
constexpr std::uint64_t maximumTotalKeySize = 16 * 1024 * 1024;
constexpr std::uint64_t maximumTotalMetadataSize = 16 * 1024 * 1024;
constexpr std::size_t maximumEntryCount = 100000;

std::uint16_t readUint16(const void* data)
{
    return juce::ByteOrder::littleEndianShort(data);
}

std::uint32_t readUint32(const void* data)
{
    return juce::ByteOrder::littleEndianInt(data);
}

std::uint64_t readUint64(const void* data)
{
    return juce::ByteOrder::littleEndianInt64(data);
}

bool readAt(juce::FileInputStream& stream, std::uint64_t offset, void* destination, int size)
{
    return offset <= static_cast<std::uint64_t>(std::numeric_limits<juce::int64>::max()) && stream.setPosition(static_cast<juce::int64>(offset)) && stream.read(destination, size) == size;
}

juce::Result invalid(const juce::String& detail)
{
    return juce::Result::fail("NOFS: " + detail);
}

bool isText(const juce::MemoryBlock& block, bool allowWhitespace)
{
    if (block.isEmpty())
    {
        return true;
    }
    const auto* data = static_cast<const char*>(block.getData());
    if (block.getSize() > static_cast<std::size_t>(std::numeric_limits<int>::max()) || !juce::CharPointer_UTF8::isValidString(data, static_cast<int>(block.getSize())))
    {
        return false;
    }
    return std::none_of(data, data + block.getSize(), [allowWhitespace](char value)
                        {
        const auto byte = static_cast<unsigned char>(value);
        return byte == 0 || byte == 0x7f || (byte < 0x20 && !(allowWhitespace && (byte == '\n' || byte == '\r' || byte == '\t'))); });
}

juce::String textOf(const juce::MemoryBlock& block)
{
    return block.isEmpty() ? juce::String() : juce::String::fromUTF8(static_cast<const char*>(block.getData()), static_cast<int>(block.getSize()));
}

bool isJsonSeparator(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f';
}
} // namespace

juce::Result VoiceDatabase::open(const juce::File& newFile, const juce::File& modelsRoot)
{
    modelDirectory = modelsRoot;
    file = juce::File();
    stream.reset();
    entries.clear();
    metadata = {};
    document = juce::var();

    auto candidate = newFile.createInputStream();
    if (candidate == nullptr)
    {
        return invalid("cannot open " + newFile.getFullPathName());
    }

    // Current voice packages are a JSON document that names sibling .dnni model
    // files; older packages embed every model inside a binary container.
    std::array<char, 16> probe{};
    const auto probeSize = candidate->read(probe.data(), static_cast<int>(probe.size()));
    candidate->setPosition(0);
    std::size_t probeIndex = 0;
    if (probeSize >= 3 && static_cast<unsigned char>(probe[0]) == 0xef && static_cast<unsigned char>(probe[1]) == 0xbb && static_cast<unsigned char>(probe[2]) == 0xbf)
    {
        probeIndex = 3;
    }
    while (probeIndex < static_cast<std::size_t>(probeSize > 0 ? probeSize : 0) && isJsonSeparator(probe[probeIndex]))
    {
        ++probeIndex;
    }
    const char first = probeIndex < static_cast<std::size_t>(probeSize > 0 ? probeSize : 0) ? probe[probeIndex] : '\0';
    if (first == '{')
    {
        candidate.reset();
        return openDocument(newFile);
    }
    return openContainer(newFile, std::move(candidate));
}

juce::Result VoiceDatabase::openDocument(const juce::File& source)
{
    const auto text = source.loadFileAsString();
    if (text.isEmpty())
    {
        return invalid("voice document is empty");
    }
    auto parsed = juce::JSON::parse(text);
    auto* object = parsed.getDynamicObject();
    if (object == nullptr)
    {
        return invalid("voice document is not a JSON object");
    }

    for (const auto& field : {"duration", "acoustic", "vocoder", "f0"})
    {
        const auto hash = object->getProperty(field).toString();
        if (hash.isEmpty())
        {
            continue;
        }
        if (!object->getProperty(field).isString() || !isSafeModelId(hash))
        {
            return invalid("unsafe model identifier for " + juce::String(field));
        }
        VoiceEntry entry;
        entry.key.replaceAll(hash.toRawUTF8(), hash.getNumBytesAsUTF8());
        entry.name = hash;
        entry.external = locateModel(source, field, hash);
        entry.valueSize = entry.external.existsAsFile() ? static_cast<std::uint32_t>(juce::jmin<juce::int64>(entry.external.getSize(), maximumEntrySize)) : 0;
        entries.push_back(entry);
        if (std::string(field) == "f0")
        {
            // The pitch model is resolved by a fixed name rather than through
            // the voice configuration, which this format does not repeat.
            VoiceEntry alias = entry;
            alias.name = "f0model-dds";
            entries.push_back(std::move(alias));
        }
    }
    if (entries.empty())
    {
        return invalid("voice document names no models");
    }

    metadata.name = object->getProperty("name").toString();
    metadata.vendor = object->getProperty("vendor").toString();
    metadata.language = object->getProperty("language").toString();
    metadata.phoneset = object->getProperty("phoneset").toString();
    const auto version = object->getProperty("version").toString();
    metadata.version = version.isNotEmpty() && version.containsOnly("0123456789") && version.length() <= 9 ? version.getIntValue() : -1;
    if (const auto* languages = object->getProperty("support_languages").getArray())
    {
        for (const auto& language : *languages)
        {
            metadata.languages.add(language.toString());
        }
    }
    if (const auto* styles = object->getProperty("styles").getArray())
    {
        for (const auto& style : *styles)
        {
            if (const auto* styleObject = style.getDynamicObject())
            {
                metadata.timbreStyles.add(styleObject->getProperty("name").toString());
            }
        }
    }
    metadata.properties.set(".name", metadata.name);
    metadata.properties.set(".vendor", metadata.vendor);
    metadata.properties.set(".language", metadata.language);
    metadata.properties.set(".phoneset", metadata.phoneset);
    if (metadata.version >= 0)
    {
        metadata.properties.set(".version", juce::String(metadata.version));
    }
    if (metadata.languages.size() > 0)
    {
        metadata.properties.set(".multi", metadata.languages.joinIntoString(","));
    }
    if (metadata.timbreStyles.size() > 0)
    {
        metadata.properties.set(".timbre_styles", metadata.timbreStyles.joinIntoString(","));
    }

    file = source;
    document = std::move(parsed);
    return juce::Result::ok();
}

juce::File VoiceDatabase::locateModel(const juce::File& source, const juce::String& field, const juce::String& hash) const
{
    if (modelDirectory != juce::File())
    {
        return findModelFile(modelDirectory.getChildFile(field), hash);
    }
    if (const auto override = juce::SystemStats::getEnvironmentVariable("OPENSVMODELS", {}); override.isNotEmpty())
    {
        const auto candidate = findModelFile(juce::File(override).getChildFile(field), hash);
        if (candidate.existsAsFile())
        {
            return candidate;
        }
    }
    // source is <root>/databases/<voice>/info.<version>.nofs
    const auto models = source.getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("models");
    const auto expected = findModelFile(models.getChildFile(field), hash);
    if (expected.existsAsFile())
    {
        return expected;
    }
    for (const auto& directory : models.findChildFiles(juce::File::findDirectories, false))
    {
        const auto sibling = findModelFile(directory, hash);
        if (sibling.existsAsFile())
        {
            return sibling;
        }
    }
    return expected;
}

juce::Result VoiceDatabase::openContainer(const juce::File& source, std::unique_ptr<juce::FileInputStream> candidate)
{
    const auto signedSize = candidate->getTotalLength();
    if (signedSize < static_cast<juce::int64>(fileHeaderSize + 10))
    {
        return invalid("truncated file header");
    }
    const auto fileSize = static_cast<std::uint64_t>(signedSize);
    std::array<std::uint8_t, 16> header{};
    if (!readAt(*candidate, 0, header.data(), static_cast<int>(header.size())))
    {
        return invalid("cannot read file header");
    }
    if (readUint32(header.data()) != nofsMagic)
    {
        return invalid("unrecognised file signature");
    }
    if (readUint32(header.data() + 4) != supportedVersion)
    {
        return invalid("unsupported container version");
    }
    if (readUint64(header.data() + 8) != fileSize)
    {
        return invalid("declared file length does not match the file");
    }

    std::array<std::uint8_t, 8> blockHeader{};
    if (!readAt(*candidate, fileHeaderSize, blockHeader.data(), static_cast<int>(blockHeader.size())))
    {
        return invalid("cannot read index block");
    }
    const auto indexSize = readUint32(blockHeader.data());
    if (indexSize < 10 || indexSize > fileSize - fileHeaderSize || readUint16(blockHeader.data() + 4) != indexBlockType)
    {
        return invalid("invalid or unsupported index block");
    }
    std::array<std::uint8_t, 4> lengthBytes{};
    if (!readAt(*candidate, fileHeaderSize + indexSize - 4, lengthBytes.data(), static_cast<int>(lengthBytes.size())) || readUint32(lengthBytes.data()) != indexSize)
    {
        return invalid("index block trailer mismatch");
    }

    // The index hash algorithm is not required for a checked sequential walk.
    // Both observed files contain contiguous value blocks through the exact EOF.
    std::vector<VoiceEntry> parsed;
    std::unordered_set<std::string> keys;
    std::uint64_t totalKeySize = 0;
    auto offset = fileHeaderSize + indexSize;
    while (offset < fileSize)
    {
        if (parsed.size() >= maximumEntryCount || fileSize - offset < 16 || !readAt(*candidate, offset, blockHeader.data(), static_cast<int>(blockHeader.size())))
        {
            return invalid("too many entries or truncated value block header");
        }
        const auto recordSize = readUint32(blockHeader.data());
        const auto recordType = readUint16(blockHeader.data() + 4);
        const auto keySize = readUint16(blockHeader.data() + 6);
        if (recordType != valueBlockType)
        {
            return invalid("unsupported value block type at " + juce::String(static_cast<juce::int64>(offset)));
        }
        if (keySize == 0 || recordSize < 16u + keySize || recordSize > fileSize - offset)
        {
            return invalid("invalid value block size");
        }
        totalKeySize += keySize;
        if (totalKeySize > maximumTotalKeySize)
        {
            return invalid("entry keys exceed the 16 MiB read limit");
        }

        VoiceEntry entry;
        entry.key.setSize(keySize);
        if (!readAt(*candidate, offset + 8, entry.key.getData(), keySize))
        {
            return invalid("cannot read entry key");
        }
        const std::string rawKey(static_cast<const char*>(entry.key.getData()), entry.key.getSize());
        if (!keys.insert(rawKey).second)
        {
            return invalid("duplicate entry key");
        }
        if (isText(entry.key, false))
        {
            entry.name = textOf(entry.key);
        }
        if (!readAt(*candidate, offset + 8 + keySize, lengthBytes.data(), static_cast<int>(lengthBytes.size())))
        {
            return invalid("cannot read entry value length");
        }
        entry.valueSize = readUint32(lengthBytes.data());
        entry.valueOffset = offset + 12 + keySize;
        if (static_cast<std::uint64_t>(entry.valueSize) + keySize + 16 != recordSize)
        {
            return invalid("value length does not match its block");
        }
        if (!readAt(*candidate, offset + recordSize - 4, lengthBytes.data(), static_cast<int>(lengthBytes.size())) || readUint32(lengthBytes.data()) != recordSize)
        {
            return invalid("value block trailer mismatch");
        }
        parsed.push_back(std::move(entry));
        offset += recordSize;
    }
    if (parsed.empty())
    {
        return invalid("container has no entries");
    }

    file = source;
    stream = std::move(candidate);
    entries = std::move(parsed);
    auto result = readMetadata();
    if (result.failed())
    {
        file = juce::File();
        stream.reset();
        entries.clear();
        metadata = {};
    }
    return result;
}

const std::vector<VoiceEntry>& VoiceDatabase::getEntries() const noexcept
{
    return entries;
}

const VoiceMetadata& VoiceDatabase::getMetadata() const noexcept
{
    return metadata;
}

const juce::File& VoiceDatabase::getFile() const noexcept
{
    return file;
}

const VoiceEntry* VoiceDatabase::findEntry(juce::StringRef name) const
{
    const auto found = std::find_if(entries.begin(), entries.end(), [name](const VoiceEntry& entry)
                                    { return entry.name.isNotEmpty() && entry.name == name; });
    return found == entries.end() ? nullptr : &*found;
}

juce::Result VoiceDatabase::readEntry(const VoiceEntry& entry, juce::MemoryBlock& destination)
{
    destination.reset();
    const auto found = std::find_if(entries.begin(), entries.end(), [&entry](const VoiceEntry& existing)
                                    { return existing.key == entry.key && existing.valueOffset == entry.valueOffset && existing.valueSize == entry.valueSize && existing.external == entry.external; });
    if (found == entries.end())
    {
        return invalid("entry does not belong to the open database");
    }
    if (entry.valueSize > maximumEntrySize)
    {
        return invalid("entry exceeds the 512 MiB read limit");
    }
    if (entry.external != juce::File())
    {
        if (!entry.external.existsAsFile())
        {
            return invalid("model file is missing: " + entry.external.getFullPathName());
        }
        const auto size = entry.external.getSize();
        if (size > static_cast<juce::int64>(maximumEntrySize))
        {
            return invalid("model file exceeds the 512 MiB read limit: " + entry.external.getFullPathName());
        }
        auto input = entry.external.createInputStream();
        if (input == nullptr || size > std::numeric_limits<int>::max())
        {
            return invalid("cannot open model file: " + entry.external.getFullPathName());
        }
        destination.setSize(static_cast<std::size_t>(size));
        if (input->read(destination.getData(), static_cast<int>(size)) != static_cast<int>(size))
        {
            destination.reset();
            return invalid("cannot read model file: " + entry.external.getFullPathName());
        }
        return juce::Result::ok();
    }
    if (stream == nullptr)
    {
        return invalid("no database is open");
    }
    if (entry.valueSize == 0)
    {
        return juce::Result::ok();
    }
    destination.setSize(entry.valueSize);
    if (!readAt(*stream, entry.valueOffset, destination.getData(), static_cast<int>(entry.valueSize)))
    {
        destination.reset();
        return invalid("cannot read entry value");
    }
    return juce::Result::ok();
}

bool VoiceDatabase::hasDocument() const noexcept
{
    return document.isObject();
}

const juce::var& VoiceDatabase::getDocument() const noexcept
{
    return document;
}

juce::Result VoiceDatabase::readMetadata()
{
    std::uint64_t totalMetadataSize = 0;
    for (const auto& entry : entries)
    {
        if (!entry.name.startsWithChar('.'))
        {
            continue;
        }
        if (entry.valueSize > maximumMetadataSize)
        {
            return invalid("metadata entry is too large: " + entry.name);
        }
        totalMetadataSize += entry.valueSize;
        if (totalMetadataSize > maximumTotalMetadataSize)
        {
            return invalid("metadata exceeds the 16 MiB read limit");
        }
        juce::MemoryBlock value;
        if (auto result = readEntry(entry, value); result.failed())
        {
            return result;
        }
        if (!isText(value, true))
        {
            return invalid("metadata is not valid text: " + entry.name);
        }
        metadata.properties.set(entry.name, textOf(value));
    }
    metadata.name = metadata.properties[".name"];
    metadata.vendor = metadata.properties[".vendor"];
    metadata.language = metadata.properties[".language"];
    metadata.phoneset = metadata.properties[".phoneset"];
    metadata.type = metadata.properties[".type"];
    metadata.languages.addTokens(metadata.properties[".multi"], false);
    metadata.timbreStyles.addTokens(metadata.properties[".timbre_styles"], false);
    const auto version = metadata.properties[".version"];
    if (version.isNotEmpty())
    {
        if (!version.containsOnly("0123456789") || version.length() > 9)
        {
            return invalid("invalid database version metadata");
        }
        metadata.version = version.getIntValue();
    }
    return juce::Result::ok();
}
} // namespace sv::synthesis
