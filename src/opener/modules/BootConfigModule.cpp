#include "BootConfigModule.h"

#include "Compat.h"

#include "../fs/RegistryHiveReader.h"

#include <QFile>
#include <QRegExp>
#include <QStringList>

#include <algorithm>
#include <sstream>

namespace peare {
namespace {

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

QString payloadText(const fs::RegistryHiveReader& hive, const std::string& path) {
    fs::ByteStorePtr store = hive.openFile(path);
    if (!store) return QString();
    const std::vector<std::uint8_t> data = store->readAll();
    return QString::fromUtf8(reinterpret_cast<const char*>(data.data()), int(data.size()));
}

QString payloadValue(const QString& payload) {
    const QString marker = QStringLiteral("value=");
    const int pos = payload.indexOf(marker);
    return pos < 0 ? QString() : payload.mid(pos + marker.size()).trimmed();
}

quint64 parseIntegerValue(const QString& payload, bool* ok = nullptr) {
    const QString value = payloadValue(payload);
    return value.toULongLong(ok, 10);
}

std::vector<std::uint8_t> parseHexBytes(const QString& payload) {
    std::vector<std::uint8_t> out;
    const QString value = payloadValue(payload);
    const QStringList parts = value.split(QRegExp(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        bool ok = false;
        const uint v = p.toUInt(&ok, 16);
        if (ok) out.push_back(static_cast<std::uint8_t>(v & 0xff));
    }
    return out;
}

QString formatName(quint32 id) {
    switch ((id >> 24) & 0x0f) {
    case 1: return QStringLiteral("Device");
    case 2: return QStringLiteral("String");
    case 3: return QStringLiteral("Guid");
    case 4: return QStringLiteral("GuidList");
    case 5: return QStringLiteral("Integer");
    case 6: return QStringLiteral("Boolean");
    case 7: return QStringLiteral("IntegerList");
    default: return QStringLiteral("None");
    }
}

QString className(quint32 id) {
    switch ((id >> 28) & 0x0f) {
    case 1: return QStringLiteral("Library");
    case 2: return QStringLiteral("Application");
    case 3: return QStringLiteral("Device");
    case 4: return QStringLiteral("SetupTemplate");
    case 5: return QStringLiteral("Oem");
    case 6: return QStringLiteral("BootApplication");
    default: return QStringLiteral("Unknown");
    }
}

QString objectTypeName(quint32 type) {
    switch ((type >> 28) & 0x0f) {
    case 1: return QStringLiteral("Application");
    case 2: return QStringLiteral("Inherit");
    case 3: return QStringLiteral("Device");
    default: return QStringLiteral("Unknown");
    }
}

QString knownObjectName(const QString& guid) {
    const QString g = guid.toUpper();
    if (g == QStringLiteral("{9DEA862C-5CDD-4E70-ACC1-F32B344D4795}")) return QStringLiteral("{bootmgr}");
    if (g == QStringLiteral("{A5A30FA2-3D06-4E9F-B5F4-A01DF9D1FCBA}")) return QStringLiteral("{fwbootmgr}");
    if (g == QStringLiteral("{1CAE1EB7-A0DF-4D4D-9851-4860E34EF535}")) return QStringLiteral("{default}");
    if (g == QStringLiteral("{FA926493-6F1C-4193-A414-58F0B2456D1E}")) return QStringLiteral("{current}");
    if (g == QStringLiteral("{B2721D73-1DB4-4C62-BF78-C548A880142D}")) return QStringLiteral("{memdiag}");
    if (g == QStringLiteral("{7EA2E1AC-2E61-4728-AAA3-896D9D0A9F0E}")) return QStringLiteral("{globalsettings}");
    if (g == QStringLiteral("{6EFB52BF-1766-41DB-A6B3-0EE5EFF72BD7}")) return QStringLiteral("{bootloadersettings}");
    return guid;
}

QString knownElementName(quint32 id) {
    switch (id) {
    case 0x11000001: return QStringLiteral("device");
    case 0x12000002: return QStringLiteral("path");
    case 0x12000004: return QStringLiteral("description");
    case 0x12000005: return QStringLiteral("locale");
    case 0x14000006: return QStringLiteral("inherit");
    case 0x16000009: return QStringLiteral("recoveryenabled");
    case 0x16000010: return QStringLiteral("bootdebug");
    case 0x12000030: return QStringLiteral("loadoptions");
    case 0x24000001: return QStringLiteral("displayorder");
    case 0x24000002: return QStringLiteral("bootsequence");
    case 0x23000003: return QStringLiteral("default/resumeobject");
    case 0x25000004: return QStringLiteral("timeout");
    case 0x26000005: return QStringLiteral("attemptresume");
    case 0x23000006: return QStringLiteral("resumeobject");
    case 0x26000020: return QStringLiteral("displaybootmenu");
    case 0x21000022: return QStringLiteral("bcddevice");
    case 0x22000023: return QStringLiteral("bcdfilepath");
    case 0x22000002: return QStringLiteral("systemroot");
    case 0x25000020: return QStringLiteral("nx");
    case 0x26000041: return QStringLiteral("disablebootdisplay");
    default: return QStringLiteral("element");
    }
}

QString decodeElementValue(quint32 id, const QString& payload) {
    const quint32 fmt = (id >> 24) & 0x0f;
    if (fmt == 2 || fmt == 3 || fmt == 4) return payloadValue(payload);
    const std::vector<std::uint8_t> bytes = parseHexBytes(payload);
    if (fmt == 6) return !bytes.empty() && bytes[0] != 0 ? QStringLiteral("True") : QStringLiteral("False");
    if (fmt == 5) {
        quint64 v = 0;
        const std::size_t n = std::min<std::size_t>(bytes.size(), 8);
        for (std::size_t i = 0; i < n; ++i) v |= quint64(bytes[i]) << (8 * i);
        return QString::number(v);
    }
    if (fmt == 7) {
        QStringList values;
        for (std::size_t pos = 0; pos + 7 < bytes.size(); pos += 8) {
            quint64 v = 0;
            for (int i = 0; i < 8; ++i) v |= quint64(bytes[pos + i]) << (8 * i);
            values << QString::number(v);
        }
        return values.join(QStringLiteral(","));
    }
    return payloadValue(payload);
}

QByteArray textBytes(const QString& text) { return text.toUtf8(); }

bool isGuidName(const std::string& name) {
    return name.size() == 38 && name.front() == '{' && name.back() == '}';
}

}  // namespace

ModulePtr BootConfigModule::open(const fs::ByteStorePtr& store, const QString& sourceName,
                                 const QString& subPath) {
    auto module = peare::makeUnique<BootConfigModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::BOOTCONFIG;

    fs::RegistryHiveReader hive(store);
    if (!hive.valid()) {
        module->info_.error = QString::fromStdString(hive.error());
        return ModulePtr(std::move(module));
    }
    const std::vector<fs::DiscEntry> objects = hive.list("Objects");
    if (objects.empty()) {
        module->info_.error = QStringLiteral("Registry hive is not a BCD store");
        return ModulePtr(std::move(module));
    }

    if (subPath.isEmpty()) {
        for (const fs::DiscEntry& obj : objects) {
            if (!obj.isDirectory || !isGuidName(obj.name)) continue;
            const std::string descPath = std::string("Objects/") + obj.name + "/Description/@Type";
            bool ok = false;
            const quint64 type = parseIntegerValue(payloadText(hive, descPath), &ok);
            if (!ok) continue;
            ResourceEntry entry;
            const QString guid = QString::fromStdString(obj.name).toUpper();
            entry.type = QStringLiteral("BCD_OBJECT");
            entry.name = knownObjectName(guid);
            if (entry.name != guid) entry.name += QStringLiteral(" ") + guid;
            entry.language = QStringLiteral("neutral");
            entry.format = ModuleFormat::BOOTCONFIG;
            entry.dataSize = 0;
            entry.content = store;
            entry.isDirectory = true;
            entry.containerSubPath = guid;
            module->resources_.push_back(std::move(entry));

            ResourceEntry summary;
            summary.type = QStringLiteral("BCD_SUMMARY");
            summary.name = guid + QStringLiteral(" description");
            summary.language = QStringLiteral("neutral");
            summary.format = ModuleFormat::BOOTCONFIG;
            const QString text = QStringLiteral("object=%1\nname=%2\ntype=0x%3\nobjectType=%4\n")
                                     .arg(guid, knownObjectName(guid))
                                     .arg(qulonglong(type), 8, 16, QLatin1Char('0'))
                                     .arg(objectTypeName(quint32(type)));
            summary.data = textBytes(text);
            summary.dataSize = quint64(summary.data.size());
            module->resources_.push_back(std::move(summary));
        }
    } else {
        const QString guid = subPath.toUpper();
        const std::string objectPath = std::string("Objects/") + guid.toStdString();
        bool ok = false;
        const quint64 type =
            parseIntegerValue(payloadText(hive, objectPath + "/Description/@Type"), &ok);
        if (!ok) {
            module->info_.error = QStringLiteral("BCD object not found");
            return ModulePtr(std::move(module));
        }
        const std::vector<fs::DiscEntry> elements = hive.list(objectPath + "/Elements");
        for (const fs::DiscEntry& elementDir : elements) {
            if (!elementDir.isDirectory) continue;
            bool idOk = false;
            const quint32 id = elementDir.name.empty()
                                   ? 0
                                   : QString::fromStdString(elementDir.name).toUInt(&idOk, 16);
            if (!idOk) continue;
            const QString raw = payloadText(hive, objectPath + "/Elements/" + elementDir.name + "/@Element");
            if (raw.isEmpty()) continue;
            const QString text =
                QStringLiteral("object=%1\nobjectType=%2\ntype=0x%3\nid=%4\nname=%5\nclass=%6\nformat=%7\nvalue=%8\n")
                    .arg(guid, objectTypeName(quint32(type)))
                    .arg(qulonglong(type), 8, 16, QLatin1Char('0'))
                    .arg(QString::fromStdString(elementDir.name).toUpper(), knownElementName(id),
                         className(id), formatName(id), decodeElementValue(id, raw));
            ResourceEntry entry;
            entry.type = QStringLiteral("BCD_ELEMENT");
            entry.name = QStringLiteral("%1 %2").arg(QString::fromStdString(elementDir.name).toUpper(),
                                                     knownElementName(id));
            entry.language = QStringLiteral("neutral");
            entry.format = ModuleFormat::BOOTCONFIG;
            entry.data = textBytes(text);
            entry.dataSize = quint64(entry.data.size());
            module->resources_.push_back(std::move(entry));
        }
    }

    if (module->resources_.isEmpty()) {
        module->info_.error = QStringLiteral("BCD store has no readable objects");
        return ModulePtr(std::move(module));
    }
    module->info_.description = QStringLiteral("Boot Configuration Data (BCD)");
    return ModulePtr(std::move(module));
}

ModulePtr BootConfigModule::open(const QString& filePath) {
    fs::ByteStorePtr store = storeForFile(filePath);
    if (!store) {
        auto module = peare::makeUnique<BootConfigModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::BOOTCONFIG;
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    return open(store, filePath);
}

}  // namespace peare
