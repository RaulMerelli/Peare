#include "ArModule.h"
#include "Compat.h"
#include "../fs/QFileByteStore.h"

#include <QFile>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace peare {
namespace {

const std::int64_t kArMagicSize = 8;
const std::int64_t kArHeaderSize = 60;
const std::size_t kMaxNameTableSize = 64u * 1024u * 1024u;

struct ArMember {
    std::string nameField;
    std::int64_t dataOffset = 0;
    std::int64_t dataSize = 0;
    bool special = false;
};

fs::ByteStorePtr storeForFile(const QString& path)
{
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    auto source = std::make_shared<fs::QFileByteStore>(path);
    return source->validate() ? source : fs::ByteStorePtr();
}

std::string fieldString(const std::uint8_t* data, std::size_t size)
{
    while (size && data[size - 1] == ' ') --size;
    std::size_t start = 0;
    while (start < size && data[start] == ' ') ++start;
    return std::string(reinterpret_cast<const char*>(data + start), size - start);
}

bool parseNumber(const std::uint8_t* data, std::size_t size, unsigned int base,
                 std::int64_t* value)
{
    if (!value) return false;
    std::size_t start = 0;
    while (start < size && data[start] == ' ') ++start;
    std::size_t end = size;
    while (end > start && data[end - 1] == ' ') --end;
    if (start == end) return false;

    std::int64_t result = 0;
    for (std::size_t i = start; i < end; ++i) {
        const unsigned char c = data[i];
        unsigned int digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        if (digit >= base || result > ((std::numeric_limits<std::int64_t>::max)() - digit) / base)
            return false;
        result = result * base + digit;
    }
    *value = result;
    return true;
}

bool isSymbolTable(const std::string& name)
{
    return name == "/" || name == "/SYM64/" || name == "__.SYMDEF" ||
           name == "__.SYMDEF SORTED" || name == "__.SYMDEF_64" ||
           name == "__.SYMDEF_64 SORTED" || name == "__.GOSYMDEF";
}

QString safeName(const std::string& raw)
{
    QString name = QString::fromUtf8(raw.data(), int(raw.size()));
    if (name.contains(QChar::Null)) return QString();
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    name = parts.isEmpty() ? QString() : parts.last();
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < name.size(); ++i) {
        if (name.at(i).unicode() < 0x20 || forbidden.contains(name.at(i)))
            name[i] = QLatin1Char('_');
    }
    while (name.endsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char('.'))) name.chop(1);
    return name;
}

bool resolveGnuName(const std::string& field, const std::vector<std::uint8_t>& table,
                    std::string* name)
{
    if (field.size() < 2 || field[0] != '/') return false;
    std::int64_t offset = 0;
    if (!parseNumber(reinterpret_cast<const std::uint8_t*>(field.data() + 1),
                     field.size() - 1, 10, &offset) || offset < 0 ||
        std::size_t(offset) >= table.size()) return false;
    std::size_t end = std::size_t(offset);
    while (end < table.size() && table[end] != '\n' && table[end] != 0) ++end;
    if (end == table.size() || end == std::size_t(offset)) return false;
    if (table[end] == '\n' && end > std::size_t(offset) && table[end - 1] == '/') --end;
    *name = std::string(reinterpret_cast<const char*>(table.data() + offset),
                        end - std::size_t(offset));
    return !name->empty();
}

} // namespace

ModulePtr ArModule::open(const QString& filePath)
{
    return open(storeForFile(filePath), filePath);
}

ModulePtr ArModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())), logicalName);
}

ModulePtr ArModule::open(const fs::ByteStorePtr& file, const QString& sourceName)
{
    auto module = peare::makeUnique<ArModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::AR;
    module->info_.description = QStringLiteral("Unix ar archive");
    if (!file || file->capacity() < kArMagicSize) {
        module->info_.error = QStringLiteral("Truncated ar archive");
        return ModulePtr(std::move(module));
    }

    const std::vector<std::uint8_t> magic = file->readRange(0, kArMagicSize);
    if (magic.size() != std::size_t(kArMagicSize)) {
        module->info_.error = QStringLiteral("Cannot read ar archive header");
        return ModulePtr(std::move(module));
    }
    if (std::memcmp(magic.data(), "!<arch>\n", kArMagicSize) != 0) {
        module->info_.error = QStringLiteral("Invalid ar archive signature; thin archives are not supported");
        return ModulePtr(std::move(module));
    }

    std::vector<ArMember> members;
    std::vector<std::uint8_t> longNameTable;
    std::int64_t position = kArMagicSize;
    while (position < file->capacity()) {
        if (file->capacity() - position < kArHeaderSize) {
            module->info_.error = QStringLiteral("Truncated ar member header");
            return ModulePtr(std::move(module));
        }
        const std::vector<std::uint8_t> header = file->readRange(position, kArHeaderSize);
        if (header.size() != std::size_t(kArHeaderSize) || header[58] != '`' || header[59] != '\n') {
            module->info_.error = QStringLiteral("Invalid ar member header");
            return ModulePtr(std::move(module));
        }
        std::int64_t memberSize = 0;
        if (!parseNumber(header.data() + 48, 10, 10, &memberSize) || memberSize < 0) {
            module->info_.error = QStringLiteral("Invalid ar member size");
            return ModulePtr(std::move(module));
        }
        const std::int64_t contentStart = position + kArHeaderSize;
        if (memberSize > file->capacity() - contentStart) {
            module->info_.error = QStringLiteral("ar member exceeds archive bounds");
            return ModulePtr(std::move(module));
        }

        ArMember member;
        member.nameField = fieldString(header.data(), 16);
        member.dataOffset = contentStart;
        member.dataSize = memberSize;
        if (member.nameField == "//") {
            if (std::size_t(memberSize) > kMaxNameTableSize) {
                module->info_.error = QStringLiteral("GNU ar name table is too large");
                return ModulePtr(std::move(module));
            }
            longNameTable = file->readRange(contentStart, memberSize);
            if (longNameTable.size() != std::size_t(memberSize)) {
                module->info_.error = QStringLiteral("Truncated GNU ar name table");
                return ModulePtr(std::move(module));
            }
            member.special = true;
        } else if (member.nameField.size() > 3 && member.nameField.compare(0, 3, "#1/") == 0) {
            std::int64_t nameLength = 0;
            if (!parseNumber(reinterpret_cast<const std::uint8_t*>(member.nameField.data() + 3),
                             member.nameField.size() - 3, 10, &nameLength) ||
                nameLength <= 0 || nameLength > memberSize || nameLength > 65536) {
                module->info_.error = QStringLiteral("Invalid BSD ar extended member name");
                return ModulePtr(std::move(module));
            }
            const std::vector<std::uint8_t> nameBytes = file->readRange(contentStart, nameLength);
            member.nameField.assign(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
            while (!member.nameField.empty() && member.nameField.back() == '\0')
                member.nameField.pop_back();
            member.dataOffset += nameLength;
            member.dataSize -= nameLength;
        }

        const std::int64_t memberEnd = contentStart + memberSize;
        if ((memberSize & 1) != 0 && memberEnd == file->capacity()) {
            module->info_.error = QStringLiteral("Truncated ar member padding");
            return ModulePtr(std::move(module));
        }
        position = memberEnd + (memberSize & 1);
        if (!member.special) members.push_back(std::move(member));
    }

    for (const ArMember& member : members) {
        std::string rawName = member.nameField;
        if (rawName == "/" || rawName == "/SYM64/") continue;
        if (rawName.size() > 1 && rawName[0] == '/') {
            if (!resolveGnuName(rawName, longNameTable, &rawName)) {
                module->info_.error = QStringLiteral("Invalid GNU ar long-name reference");
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
        } else {
            if (rawName.back() == '/') rawName.pop_back();
        }
        if (isSymbolTable(rawName)) continue;
        if (isSymbolTable(rawName)) continue;

        const QString name = safeName(rawName);
        if (name.isEmpty()) {
            module->info_.error = QStringLiteral("Invalid or empty ar member name");
            module->resources_.clear();
            return ModulePtr(std::move(module));
        }
        ResourceEntry entry;
        entry.type = QStringLiteral("AR_FILE");
        entry.name = name;
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = quint64(member.dataOffset);
        entry.dataSize = quint64(member.dataSize);
        entry.format = ModuleFormat::AR;
        entry.isEmbeddedFile = true;
        entry.content = std::make_shared<fs::SubStore>(file, member.dataOffset, member.dataSize);
        module->resources_.push_back(std::move(entry));
    }
    return ModulePtr(std::move(module));
}

} // namespace peare
