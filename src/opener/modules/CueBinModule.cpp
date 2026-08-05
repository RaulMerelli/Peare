#include "CueBinModule.h"
#include "Compat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace peare {
namespace {

struct CueFile {
    QString declaredName;
    QString fileType;
    QString resolvedPath;
    fs::ByteStorePtr store;
    qint64 sectorSize = 0;
};

struct CueTrack {
    int number = 0;
    QString mode;
    int fileIndex = -1;
    qint64 index00 = -1;
    qint64 index01 = -1;
    qint64 pregap = 0;
    qint64 postgap = 0;
    QString title;
    QString performer;
    QString isrc;
    QStringList flags;
};

struct ParsedCue {
    QVector<CueFile> files;
    QVector<CueTrack> tracks;
    QString title;
    QString performer;
};

class SectorPayloadStore final : public fs::IByteStore {
public:
    SectorPayloadStore(fs::ByteStorePtr parent, qint64 rawSectorSize,
                       qint64 payloadOffset, qint64 payloadSize)
        : parent_(std::move(parent)), rawSectorSize_(rawSectorSize),
          payloadOffset_(payloadOffset), payloadSize_(payloadSize)
    {
    }

    std::int64_t capacity() const override
    {
        if (!parent_ || rawSectorSize_ <= 0 || payloadSize_ <= 0) return 0;
        return (parent_->capacity() / rawSectorSize_) * payloadSize_;
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override
    {
        const std::int64_t total = capacity();
        if (!parent_ || pos < 0 || count <= 0 || pos >= total) return 0;
        const std::int64_t available = total - pos;
        int wanted = count < available ? count : static_cast<int>(available);
        int produced = 0;
        while (wanted > 0) {
            const std::int64_t logical = pos + produced;
            const std::int64_t sector = logical / payloadSize_;
            const std::int64_t within = logical % payloadSize_;
            const int chunk = static_cast<int>(std::min<std::int64_t>(
                payloadSize_ - within, wanted));
            const int got = parent_->read(sector * rawSectorSize_ + payloadOffset_ + within,
                                          dst + produced, chunk);
            if (got <= 0) break;
            produced += got;
            wanted -= got;
        }
        return produced;
    }

private:
    fs::ByteStorePtr parent_;
    qint64 rawSectorSize_ = 0;
    qint64 payloadOffset_ = 0;
    qint64 payloadSize_ = 0;
};

QString decodeCueText(const QByteArray& bytes)
{
    if (bytes.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(bytes.constData() + 3, bytes.size() - 3);
    if (bytes.size() >= 2 && quint8(bytes[0]) == 0xff && quint8(bytes[1]) == 0xfe) {
        const int units = (bytes.size() - 2) / 2;
        QVector<ushort> text(units);
        for (int i = 0; i < units; ++i)
            text[i] = ushort(quint8(bytes[2 + i * 2])) |
                      (ushort(quint8(bytes[3 + i * 2])) << 8);
        return QString::fromUtf16(text.constData(), units);
    }
    if (bytes.size() >= 2 && quint8(bytes[0]) == 0xfe && quint8(bytes[1]) == 0xff) {
        const int units = (bytes.size() - 2) / 2;
        QVector<ushort> text(units);
        for (int i = 0; i < units; ++i)
            text[i] = (ushort(quint8(bytes[2 + i * 2])) << 8) |
                      ushort(quint8(bytes[3 + i * 2]));
        return QString::fromUtf16(text.constData(), units);
    }

    const QString utf8 = QString::fromUtf8(bytes);
    if (!utf8.contains(QChar::ReplacementCharacter)) return utf8;
    return QString::fromLocal8Bit(bytes);
}

QStringList tokenizeCueLine(const QString& line, QString* error)
{
    QStringList tokens;
    QString current;
    bool quoted = false;
    bool tokenStarted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (quoted) {
            if (ch == QLatin1Char('"')) {
                quoted = false;
                tokenStarted = true;
            } else {
                current += ch;
                tokenStarted = true;
            }
            continue;
        }
        if (ch == QLatin1Char('"')) {
            quoted = true;
            tokenStarted = true;
            continue;
        }
        if (ch.isSpace()) {
            if (tokenStarted) {
                tokens.push_back(current);
                current.clear();
                tokenStarted = false;
            }
            continue;
        }
        current += ch;
        tokenStarted = true;
    }
    if (quoted) {
        if (error) *error = QStringLiteral("Unterminated quoted string");
        return {};
    }
    if (tokenStarted) tokens.push_back(current);
    return tokens;
}

bool parseInteger(const QString& text, int* value)
{
    bool ok = false;
    const int parsed = text.toInt(&ok, 10);
    if (!ok || parsed < 0) return false;
    if (value) *value = parsed;
    return true;
}

bool parseMsf(const QString& text, qint64* sectors)
{
    const QStringList parts = text.split(QLatin1Char(':'));
    if (parts.size() != 3) return false;
    int minute = 0, second = 0, frame = 0;
    if (!parseInteger(parts[0], &minute) || !parseInteger(parts[1], &second) ||
        !parseInteger(parts[2], &frame) || second >= 60 || frame >= 75)
        return false;
    const qint64 result = qint64(minute) * 60 * 75 + qint64(second) * 75 + frame;
    if (sectors) *sectors = result;
    return true;
}

qint64 sectorSizeForMode(const QString& mode)
{
    const QString upper = mode.toUpper();
    if (upper == QStringLiteral("AUDIO") || upper == QStringLiteral("MODE1/2352") ||
        upper == QStringLiteral("MODE2/2352") || upper == QStringLiteral("CDI/2352"))
        return 2352;
    if (upper == QStringLiteral("MODE1/2048")) return 2048;
    if (upper == QStringLiteral("MODE2/2336") || upper == QStringLiteral("CDI/2336"))
        return 2336;
    if (upper == QStringLiteral("CDG")) return 2448;
    return 0;
}

bool dataPayloadForMode(const QString& mode, qint64* offset, qint64* size)
{
    const QString upper = mode.toUpper();
    if (upper == QStringLiteral("MODE1/2048")) {
        *offset = 0; *size = 2048; return true;
    }
    if (upper == QStringLiteral("MODE1/2352")) {
        *offset = 16; *size = 2048; return true;
    }
    if (upper == QStringLiteral("MODE2/2336") || upper == QStringLiteral("CDI/2336")) {
        *offset = 8; *size = 2048; return true;
    }
    if (upper == QStringLiteral("MODE2/2352") || upper == QStringLiteral("CDI/2352")) {
        *offset = 24; *size = 2048; return true;
    }
    return false;
}

bool looksLikeFileSystemPayload(const fs::ByteStorePtr& rawTrack, qint64 rawSectorSize,
                                qint64 payloadOffset)
{
    if (!rawTrack || rawSectorSize <= 0 || payloadOffset < 0) return false;
    std::uint8_t id[5];
    const qint64 isoPos = 16 * rawSectorSize + payloadOffset + 1;
    if (rawTrack->read(isoPos, id, 5) == 5 && std::memcmp(id, "CD001", 5) == 0)
        return true;
    std::uint8_t descriptor[6];
    for (int i = 0; i < 64; ++i) {
        const qint64 pos = (16 + i) * rawSectorSize + payloadOffset;
        if (pos + 6 > rawTrack->capacity()) break;
        if (rawTrack->read(pos, descriptor, 6) != 6) break;
        const char* marker = reinterpret_cast<const char*>(descriptor + 1);
        if (std::memcmp(marker, "NSR02", 5) == 0 || std::memcmp(marker, "NSR03", 5) == 0)
            return true;
        if (std::memcmp(marker, "BEA01", 5) != 0 &&
            std::memcmp(marker, "BOOT2", 5) != 0 &&
            std::memcmp(marker, "CD001", 5) != 0 &&
            std::memcmp(marker, "CDW02", 5) != 0 &&
            std::memcmp(marker, "TEA01", 5) != 0)
            break;
    }
    return false;
}

qint64 selectedPayloadOffset(const QString& mode, const fs::ByteStorePtr& rawTrack,
                             qint64 rawSectorSize, qint64 defaultOffset)
{
    const QString upper = mode.toUpper();
    qint64 alternate = -1;
    if (upper == QStringLiteral("MODE2/2336")) alternate = 0;
    else if (upper == QStringLiteral("MODE2/2352")) alternate = 16;
    // CD-I sectors use the XA-style subheader, so CDI modes deliberately keep
    // the default 8/24-byte offset.
    if (alternate >= 0 && !looksLikeFileSystemPayload(rawTrack, rawSectorSize, defaultOffset) &&
        looksLikeFileSystemPayload(rawTrack, rawSectorSize, alternate))
        return alternate;
    return defaultOffset;
}

bool parseCue(const QString& text, ParsedCue* parsed, QString* error)
{
    if (!parsed) return false;
    ParsedCue result;
    int currentFile = -1;
    int currentTrack = -1;
    QString normalized = text;
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = normalized.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const QString line = lines.at(lineIndex).trimmed();
        if (line.isEmpty()) continue;
        QString tokenError;
        const QStringList tokens = tokenizeCueLine(line, &tokenError);
        if (!tokenError.isEmpty()) {
            if (error) *error = QStringLiteral("Line %1: %2").arg(lineIndex + 1).arg(tokenError);
            return false;
        }
        if (tokens.isEmpty()) continue;
        const QString command = tokens[0].toUpper();
        if (command == QStringLiteral("REM") || command == QStringLiteral("CATALOG") ||
            command == QStringLiteral("CDTEXTFILE") || command == QStringLiteral("SONGWRITER"))
            continue;
        if (command == QStringLiteral("FILE")) {
            if (tokens.size() < 3) {
                if (error) *error = QStringLiteral("Line %1: malformed FILE command").arg(lineIndex + 1);
                return false;
            }
            CueFile file;
            file.declaredName = tokens[1];
            file.fileType = tokens[2].toUpper();
            result.files.push_back(file);
            currentFile = result.files.size() - 1;
            currentTrack = -1;
            continue;
        }
        if (command == QStringLiteral("TRACK")) {
            if (tokens.size() < 3 || currentFile < 0) {
                if (error) *error = QStringLiteral("Line %1: TRACK without FILE").arg(lineIndex + 1);
                return false;
            }
            int number = 0;
            if (!parseInteger(tokens[1], &number) || number < 1 || number > 99) {
                if (error) *error = QStringLiteral("Line %1: invalid track number").arg(lineIndex + 1);
                return false;
            }
            CueTrack track;
            track.number = number;
            track.mode = tokens[2].toUpper();
            track.fileIndex = currentFile;
            if (sectorSizeForMode(track.mode) == 0) {
                if (error) *error = QStringLiteral("Line %1: unsupported track mode %2")
                    .arg(lineIndex + 1).arg(track.mode);
                return false;
            }
            result.tracks.push_back(track);
            currentTrack = result.tracks.size() - 1;
            continue;
        }
        if (command == QStringLiteral("INDEX")) {
            if (tokens.size() < 3 || currentTrack < 0) {
                if (error) *error = QStringLiteral("Line %1: INDEX without TRACK").arg(lineIndex + 1);
                return false;
            }
            int indexNumber = 0;
            qint64 sectors = 0;
            if (!parseInteger(tokens[1], &indexNumber) || !parseMsf(tokens[2], &sectors)) {
                if (error) *error = QStringLiteral("Line %1: invalid INDEX").arg(lineIndex + 1);
                return false;
            }
            if (indexNumber == 0) result.tracks[currentTrack].index00 = sectors;
            if (indexNumber == 1) result.tracks[currentTrack].index01 = sectors;
            continue;
        }
        if (command == QStringLiteral("PREGAP") || command == QStringLiteral("POSTGAP")) {
            if (tokens.size() < 2 || currentTrack < 0) {
                if (error) *error = QStringLiteral("Line %1: %2 without TRACK")
                    .arg(lineIndex + 1).arg(command);
                return false;
            }
            qint64 sectors = 0;
            if (!parseMsf(tokens[1], &sectors)) {
                if (error) *error = QStringLiteral("Line %1: invalid %2")
                    .arg(lineIndex + 1).arg(command);
                return false;
            }
            if (command == QStringLiteral("PREGAP")) result.tracks[currentTrack].pregap = sectors;
            else result.tracks[currentTrack].postgap = sectors;
            continue;
        }
        if (command == QStringLiteral("FLAGS")) {
            if (currentTrack >= 0) result.tracks[currentTrack].flags = tokens.mid(1);
            continue;
        }
        if (command == QStringLiteral("ISRC")) {
            if (currentTrack >= 0 && tokens.size() >= 2) result.tracks[currentTrack].isrc = tokens[1];
            continue;
        }
        if (command == QStringLiteral("TITLE") || command == QStringLiteral("PERFORMER")) {
            const QString value = tokens.mid(1).join(QStringLiteral(" "));
            if (currentTrack >= 0) {
                if (command == QStringLiteral("TITLE")) result.tracks[currentTrack].title = value;
                else result.tracks[currentTrack].performer = value;
            } else {
                if (command == QStringLiteral("TITLE")) result.title = value;
                else result.performer = value;
            }
            continue;
        }
        // Unknown extensions are ignored, as CUE producers commonly add REM-like
        // metadata commands not required for physical track layout.
    }

    if (result.files.isEmpty() || result.tracks.isEmpty()) {
        if (error) *error = QStringLiteral("CUE sheet contains no FILE/TRACK layout");
        return false;
    }
    QSet<int> numbers;
    for (int i = 0; i < result.tracks.size(); ++i) {
        const CueTrack& track = result.tracks[i];
        if (track.index01 < 0) {
            if (error) *error = QStringLiteral("Track %1 has no INDEX 01").arg(track.number, 2, 10, QLatin1Char('0'));
            return false;
        }
        if (track.index00 >= 0 && track.index00 > track.index01) {
            if (error) *error = QStringLiteral("Track %1 has INDEX 00 after INDEX 01").arg(track.number, 2, 10, QLatin1Char('0'));
            return false;
        }
        if (numbers.contains(track.number)) {
            if (error) *error = QStringLiteral("Duplicate track number %1").arg(track.number);
            return false;
        }
        numbers.insert(track.number);
        if (i > 0 && result.tracks[i - 1].number >= track.number) {
            if (error) *error = QStringLiteral("Track numbers are not strictly increasing");
            return false;
        }
    }
    *parsed = result;
    return true;
}

QString findCueBesideBin(const QString& binPath)
{
    const QFileInfo binInfo(binPath);
    QDir dir = binInfo.dir();
    const QString expected = binInfo.completeBaseName() + QStringLiteral(".cue");
    const QString direct = dir.absoluteFilePath(expected);
    if (QFileInfo::exists(direct)) return direct;
    const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::Readable);
    for (const QFileInfo& candidate : files) {
        if (candidate.suffix().compare(QStringLiteral("cue"), Qt::CaseInsensitive) == 0 &&
            candidate.completeBaseName().compare(binInfo.completeBaseName(), Qt::CaseInsensitive) == 0)
            return candidate.absoluteFilePath();
    }
    return {};
}

QString resolveRelativeFile(const QString& cuePath, QString declared, QString* error)
{
    declared.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (declared.isEmpty() || QDir::isAbsolutePath(declared)) {
        if (error) *error = QStringLiteral("Absolute or empty FILE path is not allowed: %1").arg(declared);
        return {};
    }
    const QFileInfo cueInfo(cuePath);
    const QString base = cueInfo.dir().canonicalPath();
    if (base.isEmpty()) {
        if (error) *error = QStringLiteral("Cannot resolve CUE directory");
        return {};
    }
    const QString cleaned = QDir::cleanPath(declared);
    if (cleaned == QStringLiteral("..") || cleaned.startsWith(QStringLiteral("../"))) {
        if (error) *error = QStringLiteral("FILE path escapes the CUE directory: %1").arg(declared);
        return {};
    }

    QDir current(base);
    const QStringList components = cleaned.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString resolved = base;
    for (const QString& component : components) {
        QDir dir(resolved);
        QString chosen = component;
        if (!QFileInfo(dir.absoluteFilePath(chosen)).exists()) {
            const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
            bool found = false;
            for (const QFileInfo& entry : entries) {
                if (entry.fileName().compare(component, Qt::CaseInsensitive) == 0) {
                    chosen = entry.fileName();
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (error) *error = QStringLiteral("Referenced file not found: %1").arg(declared);
                return {};
            }
        }
        resolved = dir.absoluteFilePath(chosen);
    }

    const QFileInfo target(resolved);
    const QString canonical = target.canonicalFilePath();
    const QString prefix = base.endsWith(QDir::separator()) ? base : base + QDir::separator();
    if (canonical.isEmpty() || (!canonical.startsWith(prefix) && canonical != base)) {
        if (error) *error = QStringLiteral("Referenced file escapes the CUE directory: %1").arg(declared);
        return {};
    }
    if (!target.isFile() || !target.isReadable()) {
        if (error) *error = QStringLiteral("Referenced file is not readable: %1").arg(declared);
        return {};
    }
    return canonical;
}

fs::ByteStorePtr mapFile(const QString& path, QString* error)
{
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) {
        if (error) *error = holder->errorString();
        return nullptr;
    }
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    const QByteArray bytes = holder->readAll();
    if (bytes.size() != size) {
        if (error) *error = holder->errorString().isEmpty()
            ? QStringLiteral("Cannot read referenced binary") : holder->errorString();
        return nullptr;
    }
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

QString trackName(const CueTrack& track, bool data)
{
    QString name = QStringLiteral("Track %1").arg(track.number, 2, 10, QLatin1Char('0'));
    if (!track.title.isEmpty()) name += QStringLiteral(" - ") + track.title;
    name += data ? QStringLiteral(".iso") : QStringLiteral(".raw");
    return name;
}

QByteArray cueSummary(const ParsedCue& cue)
{
    QString text;
    if (!cue.title.isEmpty()) text += QStringLiteral("Title: %1\n").arg(cue.title);
    if (!cue.performer.isEmpty()) text += QStringLiteral("Performer: %1\n").arg(cue.performer);
    text += QStringLiteral("Files: %1\nTracks: %2\n\n").arg(cue.files.size()).arg(cue.tracks.size());
    for (const CueTrack& track : cue.tracks) {
        text += QStringLiteral("Track %1: %2, INDEX 01 %3 sectors")
            .arg(track.number, 2, 10, QLatin1Char('0')).arg(track.mode).arg(track.index01);
        if (track.index00 >= 0) text += QStringLiteral(", INDEX 00 %1").arg(track.index00);
        if (track.pregap > 0) text += QStringLiteral(", PREGAP %1").arg(track.pregap);
        if (track.postgap > 0) text += QStringLiteral(", POSTGAP %1").arg(track.postgap);
        if (!track.title.isEmpty()) text += QStringLiteral(", TITLE %1").arg(track.title);
        if (!track.performer.isEmpty()) text += QStringLiteral(", PERFORMER %1").arg(track.performer);
        if (!track.isrc.isEmpty()) text += QStringLiteral(", ISRC %1").arg(track.isrc);
        if (!track.flags.isEmpty()) text += QStringLiteral(", FLAGS %1").arg(track.flags.join(QLatin1Char(' ')));
        text += QLatin1Char('\n');
    }
    return text.toUtf8();
}

} // namespace

ModulePtr CueBinModule::open(const QString& filePath)
{
    auto module = peare::makeUnique<CueBinModule>();
    module->info_.filePath = filePath;
    module->info_.format = ModuleFormat::CUE_BIN;
    module->info_.description = QStringLiteral("CDRWIN BIN/CUE optical image");

    QString cuePath = filePath;
    if (QFileInfo(filePath).suffix().compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0) {
        cuePath = findCueBesideBin(filePath);
        if (cuePath.isEmpty()) {
            module->info_.error = QStringLiteral("No matching CUE sheet found beside BIN file");
            return ModulePtr(std::move(module));
        }
    }

    QFile cueFile(cuePath);
    if (!cueFile.open(QIODevice::ReadOnly)) {
        module->info_.error = cueFile.errorString();
        return ModulePtr(std::move(module));
    }
    if (cueFile.size() > 16 * 1024 * 1024) {
        module->info_.error = QStringLiteral("CUE sheet is unreasonably large");
        return ModulePtr(std::move(module));
    }
    const QByteArray cueBytes = cueFile.readAll();
    ParsedCue cue;
    QString parseError;
    if (!parseCue(decodeCueText(cueBytes), &cue, &parseError)) {
        module->info_.error = parseError;
        return ModulePtr(std::move(module));
    }

    for (int i = 0; i < cue.files.size(); ++i) {
        CueFile& source = cue.files[i];
        if (source.fileType != QStringLiteral("BINARY")) {
            module->info_.error = QStringLiteral("Unsupported FILE type %1 for %2; BIN/CUE support accepts BINARY sources")
                .arg(source.fileType, source.declaredName);
            return ModulePtr(std::move(module));
        }
        QString pathError;
        source.resolvedPath = resolveRelativeFile(cuePath, source.declaredName, &pathError);
        if (source.resolvedPath.isEmpty()) {
            module->info_.error = pathError;
            return ModulePtr(std::move(module));
        }

        qint64 fileSectorSize = 0;
        for (const CueTrack& track : cue.tracks) {
            if (track.fileIndex != i) continue;
            const qint64 size = sectorSizeForMode(track.mode);
            if (fileSectorSize == 0) fileSectorSize = size;
            else if (fileSectorSize != size) {
                module->info_.error = QStringLiteral("Tracks sharing %1 use incompatible physical sector sizes")
                    .arg(source.declaredName);
                return ModulePtr(std::move(module));
            }
        }
        if (fileSectorSize == 0) {
            module->info_.error = QStringLiteral("FILE %1 has no tracks").arg(source.declaredName);
            return ModulePtr(std::move(module));
        }
        source.sectorSize = fileSectorSize;
        source.store = mapFile(source.resolvedPath, &pathError);
        if (!source.store) {
            module->info_.error = QStringLiteral("Cannot open %1: %2").arg(source.declaredName, pathError);
            return ModulePtr(std::move(module));
        }
        if (source.store->capacity() % source.sectorSize != 0) {
            module->info_.error = QStringLiteral("%1 size is not a multiple of its %2-byte sector size")
                .arg(source.declaredName).arg(source.sectorSize);
            return ModulePtr(std::move(module));
        }
        module->backingStores_.push_back(source.store);
    }

    ResourceEntry sheet;
    sheet.type = QStringLiteral("CUE_SHEET");
    sheet.name = QFileInfo(cuePath).fileName();
    sheet.language = QStringLiteral("neutral");
    sheet.data = cueBytes;
    sheet.dataSize = quint64(cueBytes.size());
    sheet.format = ModuleFormat::CUE_BIN;
    sheet.hierarchyPath = QStringList() << QStringLiteral("Descriptor") << sheet.name;
    module->resources_.push_back(std::move(sheet));

    ResourceEntry summary;
    summary.type = QStringLiteral("CUE_METADATA");
    summary.name = QStringLiteral("disc-info.txt");
    summary.language = QStringLiteral("neutral");
    summary.data = cueSummary(cue);
    summary.dataSize = quint64(summary.data.size());
    summary.format = ModuleFormat::CUE_BIN;
    summary.hierarchyPath = QStringList() << QStringLiteral("Descriptor") << summary.name;
    module->resources_.push_back(std::move(summary));

    for (int i = 0; i < cue.files.size(); ++i) {
        const CueFile& source = cue.files[i];
        ResourceEntry entry;
        entry.type = QStringLiteral("CUE_BINARY_FILE");
        entry.name = QFileInfo(source.declaredName).fileName();
        entry.language = QStringLiteral("neutral");
        entry.dataSize = quint64(source.store->capacity());
        entry.format = ModuleFormat::CUE_BIN;
        entry.isEmbeddedFile = true;
        entry.content = source.store;
        entry.hierarchyPath = QStringList() << QStringLiteral("Source files") << entry.name;
        module->resources_.push_back(std::move(entry));
    }

    for (int i = 0; i < cue.tracks.size(); ++i) {
        const CueTrack& track = cue.tracks[i];
        const CueFile& source = cue.files[track.fileIndex];
        const qint64 fileSectors = source.store->capacity() / source.sectorSize;
        qint64 endSector = fileSectors;
        for (int next = i + 1; next < cue.tracks.size(); ++next) {
            if (cue.tracks[next].fileIndex != track.fileIndex) break;
            endSector = cue.tracks[next].index00 >= 0
                ? cue.tracks[next].index00 : cue.tracks[next].index01;
            break;
        }
        if (track.index01 >= fileSectors || endSector > fileSectors || endSector <= track.index01) {
            module->info_.error = QStringLiteral("Track %1 has offsets outside %2")
                .arg(track.number, 2, 10, QLatin1Char('0')).arg(source.declaredName);
            module->resources_.clear();
            return ModulePtr(std::move(module));
        }
        if (track.index00 >= 0 && track.index00 >= fileSectors) {
            module->info_.error = QStringLiteral("Track %1 INDEX 00 is outside %2")
                .arg(track.number, 2, 10, QLatin1Char('0')).arg(source.declaredName);
            module->resources_.clear();
            return ModulePtr(std::move(module));
        }

        const qint64 byteStart = track.index01 * source.sectorSize;
        const qint64 rawLength = (endSector - track.index01) * source.sectorSize;
        fs::ByteStorePtr rawTrack = std::make_shared<fs::SubStore>(source.store, byteStart, rawLength);
        qint64 payloadOffset = 0, payloadSize = 0;
        const bool dataTrack = dataPayloadForMode(track.mode, &payloadOffset, &payloadSize);

        ResourceEntry entry;
        entry.type = dataTrack ? QStringLiteral("CUE_DATA_TRACK")
                               : QStringLiteral("CUE_AUDIO_TRACK");
        entry.name = trackName(track, dataTrack);
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = quint64(byteStart);
        entry.format = ModuleFormat::CUE_BIN;
        entry.hierarchyPath = QStringList() << QStringLiteral("Tracks") << entry.name;
        if (dataTrack) {
            payloadOffset = selectedPayloadOffset(track.mode, rawTrack, source.sectorSize,
                                                  payloadOffset);
            entry.content = (source.sectorSize == payloadSize && payloadOffset == 0)
                ? rawTrack
                : std::make_shared<SectorPayloadStore>(rawTrack, source.sectorSize,
                                                       payloadOffset, payloadSize);
            entry.dataSize = quint64(entry.content->capacity());
            entry.isEmbeddedFile = true;
        } else {
            entry.content = rawTrack;
            entry.dataSize = quint64(rawLength);
            entry.isEmbeddedFile = false;
        }
        module->resources_.push_back(std::move(entry));
    }

    module->info_.description = QStringLiteral("CDRWIN BIN/CUE optical image — %1 track(s), %2 file(s)")
        .arg(cue.tracks.size()).arg(cue.files.size());
    return ModulePtr(std::move(module));
}

} // namespace peare
