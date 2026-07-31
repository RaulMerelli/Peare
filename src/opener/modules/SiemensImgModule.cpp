#include "SiemensImgModule.h"
#include "Compat.h"

#include <QFile>
#include <QFileInfo>

#include "SiemensImgParser.h"

namespace peare {
namespace {

std::vector<std::uint8_t> toVector(const QByteArray& data)
{
    const auto* begin = reinterpret_cast<const std::uint8_t*>(data.constData());
    return std::vector<std::uint8_t>(begin, begin + data.size());
}

QByteArray toByteArray(const std::vector<std::uint8_t>& data)
{
    return QByteArray(reinterpret_cast<const char*>(data.data()), int(data.size()));
}

QString safeName(const std::string& name, std::size_t index)
{
    QString value = QString::fromLatin1(name.c_str());
    if (value.isEmpty()) value = QStringLiteral("blob_%1").arg(qulonglong(index));
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (!(c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('-') || c == QLatin1Char('_')))
            value[i] = QLatin1Char('_');
    }
    return value;
}

void addResource(QVector<ResourceEntry>& resources, const QString& type,
                 const QString& name, const QByteArray& data, quint64 offset,
                 const QStringList& path = {})
{
    ResourceEntry entry;
    entry.type = type;
    entry.name = name;
    entry.dataOffset = offset;
    entry.dataSize = quint64(data.size());
    // For container-file types the tree uses the last hierarchyPath segment as
    // the resource leaf, so each resource needs a unique full path ending in its
    // own name (otherwise multiple resources sharing a folder collapse into one
    // node). Mirrors the OS2_PACK_FILE / SZDD_FILE convention.
    entry.hierarchyPath = path;
    entry.hierarchyPath << name;
    entry.data = data;
    resources.push_back(entry);
}

void addBytes(QVector<ResourceEntry>& resources, const QString& type,
              const QStringList& path, const QString& name,
              const std::vector<std::uint8_t>& data, quint64 offset = 0)
{
    addResource(resources, type, name, toByteArray(data), offset, path);
}

void addReconstructedImg(QVector<ResourceEntry>& resources,
                         const std::vector<Section>& sections,
                         const std::vector<std::vector<std::uint8_t>>& payloads)
{
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 5> pairs{{
        {0x00000001U, 0x20000000U}, {0x00000002U, 0x40000000U},
        {0x00000040U, 0x00000080U}, {0x00000200U, 0x00000400U},
        {0x00000800U, 0x10000000U}}};
    std::vector<FlashRegion> regions;
    std::map<std::size_t, std::vector<std::uint8_t>> inflated;
    auto payloadAt = [&](std::size_t i) -> const std::vector<std::uint8_t>& {
        const auto it = inflated.find(i);
        return it == inflated.end() ? payloads.at(i) : it->second;
    };

    for (const auto& pair : pairs) {
        const auto di = find_section_by_mask(sections, pair.first);
        const auto xi = find_section_by_mask(sections, pair.second);
        if (!di && !xi) continue;
        if (!di || !xi) throw Error("incomplete ProSave data/descriptor section pair");
        const auto& descriptor = payloads.at(*xi);
        if (descriptor.size() != 64 || read_u32(descriptor.data()) != 0x19101998U)
            throw Error("invalid 64-byte ProSave image descriptor");
        const std::uint32_t base = read_u32(descriptor.data() + 6);
        const std::uint32_t size = read_u32(descriptor.data() + 14);
        const auto& payload = payloads.at(*di);
        if (size != payload.size()) {
            if (!has_zlib_header(payload)) throw Error("descriptor size does not match fully decoded payload");
            inflated.emplace(*di, inflate_zlib(payload, size));
        }
        regions.push_back(FlashRegion{base, *di});
    }
    if (regions.empty()) return;

    std::sort(regions.begin(), regions.end(), [](const FlashRegion& a, const FlashRegion& b) { return a.base < b.base; });
    const std::uint64_t base = regions.front().base;
    std::uint64_t end = base;
    for (const auto& region : regions)
        end = std::max(end, std::uint64_t(region.base) + payloadAt(region.payload_index).size());
    if (end - base > std::numeric_limits<std::size_t>::max())
        throw Error("reconstructed flash image is too large");

    std::vector<std::uint8_t> flash(std::size_t(end - base), 0xFF);
    std::uint64_t previous = base;
    for (const auto& region : regions) {
        const auto& payload = payloadAt(region.payload_index);
        if (region.base < previous) throw Error("overlapping ProSave flash regions");
        std::copy(payload.begin(), payload.end(), flash.begin() + std::size_t(region.base - base));
        previous = std::uint64_t(region.base) + payload.size();
    }

    std::ostringstream flashNameStream;
    flashNameStream << "flash_base_" << std::uppercase << std::hex << std::setw(8)
                    << std::setfill('0') << base << ".nb0";
    const QString flashName = QString::fromStdString(flashNameStream.str());
    addBytes(resources, QStringLiteral("SIEMENS_IMG_FILE"),
             {}, flashName, flash);

    for (std::size_t off = 0; off + 12 <= flash.size(); ++off) {
        if (read_u32(flash.data() + off) != 0x43454345U || read_u32(flash.data() + off + 8) != 0) continue;
        const std::uint32_t pointer = read_u32(flash.data() + off + 4);
        if (pointer >= base && pointer < end)
            write_u32(flash, off + 8, std::uint32_t(pointer - base));
    }
    addBytes(resources, QStringLiteral("SIEMENS_IMG_FILE"),
             {}, QStringLiteral("NK.bin"), flash);
}

QByteArray readAll(const QString& path, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return {}; }
    return file.readAll();
}

} // namespace

ModulePtr SiemensImgModule::open(const QString& filePath)
{
    QString error;
    const QByteArray bytes = readAll(filePath, error);
    if (!error.isEmpty()) {
        auto module = peare::makeUnique<SiemensImgModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::SIEMENS_IMG;
        module->info_.description = QStringLiteral("Siemens ProSave IMG firmware container");
        module->info_.error = error;
        return module;
    }
    return open(bytes, filePath);
}

ModulePtr SiemensImgModule::open(const QByteArray& bytes, const QString& logicalName)
{
    auto module = peare::makeUnique<SiemensImgModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::SIEMENS_IMG;
    module->info_.description = QStringLiteral("Siemens ProSave IMG firmware container");

    try {
        const auto image = toVector(bytes);
        const Footer footer = parse_footer(image);
        const auto sections = parse_sections(image, footer);
        ExtractContext context;
        context.verify_crc = true;

        if (sections.empty()) {
            const std::size_t end = common_file_payload_end(image, footer);
            const std::vector<std::uint8_t> payload(image.begin(), image.begin() + end);
            addBytes(module->resources_, QStringLiteral("SIEMENS_IMG_FILE"),
                     {}, QStringLiteral("NK.bin"), payload);
            return module;
        }

        std::vector<std::vector<std::uint8_t>> payloads;
        payloads.reserve(sections.size());

        for (const auto& section : sections) {
            auto layer1 = extract_section(image, section, context);
            auto decoded = decode_nested_layers(std::move(layer1), context);
            payloads.push_back(decoded.final_data);
        }

        addReconstructedImg(module->resources_, sections, payloads);
    } catch (const std::exception& ex) {
        module->info_.error = QString::fromLocal8Bit(ex.what());
    }
    return module;
}


} // namespace peare
