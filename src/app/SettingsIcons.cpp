#include "SettingsIcons.h"

#include "IconFromExt.h"
#include "PeareApi.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QPixmap>
#include <QSet>

namespace {
struct DefaultSection {
    const char* section;
    const char* path;
    const char* ordinal;
    const char* iconExt;
};

const DefaultSection kDefaults[] = {
    {"RT_FOLDER_OPEN",  "C:\\Windows\\System32\\shell32.dll",  "4",     "folder_open"},
    {"RT_FOLDER_CLOSE", "C:\\Windows\\System32\\shell32.dll",  "5",     "folder_close"},
    {"RT_DEFAULT",      "C:\\Windows\\System32\\shell32.dll",  "1",     "default_unknown_icon"},
    {"RT_FONT",         "C:\\Windows\\System32\\shell32.dll",  "155",   "fon"},
    {"RT_FONTDIR",      "C:\\Windows\\System32\\shell32.dll",  "155",   "fon"},
    {"RT_VERSION",      "C:\\Windows\\System32\\shell32.dll",  "151",   "ini"},
    {"RT_MENU",         "C:\\Windows\\System32\\shell32.dll",  "151",   "ini"},
    {"RT_STRING",       "C:\\Windows\\System32\\shell32.dll",  "151",   "ini"},
    {"RT_BITMAP",       "C:\\Windows\\System32\\shell32.dll",  "16823", "bmp"},
    {"RT_POINTER",      "C:\\Windows\\System32\\shell32.dll",  "16823", "bmp"},
    {"RT_ICON",         "C:\\Windows\\System32\\shell32.dll",  "16823", "bmp"},
    {"RT_GROUP_ICON",   "C:\\Windows\\System32\\shell32.dll",  "16823", "bmp"},
    {"RT_CURSOR",       "C:\\Windows\\System32\\shell32.dll",  "16823", "bmp"},
    {"RT_GROUP_CURSOR", "C:\\Windows\\System32\\shell32.dll",  "16823", "bmp"},
    {"RT_MANIFEST",     "C:\\Windows\\System32\\mmcndmgr.dll", "1098",  "xml"}
};

QString normalizedName(QString value)
{
    value = value.trimmed();
    if (value.startsWith(QLatin1Char('#')))
        value.remove(0, 1);
    return value;
}

int visibleColorCount(const QImage& image)
{
    if (image.isNull())
        return 0;

    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qAlpha(pixel) == 0)
                continue;
            colors.insert(qRgb(qRed(pixel), qGreen(pixel), qBlue(pixel)));
        }
    }
    return colors.size();
}
}

SettingsIcons::SettingsIcons()
    // IniFile("Settings.ini") resolves the file against the process current
    // directory. Keep that behavior instead of using the platform settings area.
    : filePath_(QFileInfo(QStringLiteral("Settings.ini")).absoluteFilePath())
{
    writeDefaults();
    load();
}

void SettingsIcons::writeDefaults()
{
    QSettings settings(filePath_, QSettings::IniFormat);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    settings.setIniCodec("UTF-8");
#endif

    for (const DefaultSection& value : kDefaults) {
        settings.beginGroup(QString::fromLatin1(value.section));
        // Equivalent of IniFile.WriteIfKeyNotExists: an existing empty value is
        // considered missing by the C# implementation and is therefore replaced.
        if (settings.value(QStringLiteral("path")).toString().isEmpty())
            settings.setValue(QStringLiteral("path"), QString::fromLatin1(value.path));
        if (settings.value(QStringLiteral("ordinal")).toString().isEmpty())
            settings.setValue(QStringLiteral("ordinal"), QString::fromLatin1(value.ordinal));
        if (settings.value(QStringLiteral("iconExt")).toString().isEmpty())
            settings.setValue(QStringLiteral("iconExt"), QString::fromLatin1(value.iconExt));
        settings.endGroup();
    }
    settings.sync();
}

void SettingsIcons::load()
{
    QSettings settings(filePath_, QSettings::IniFormat);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    settings.setIniCodec("UTF-8");
#endif

    const QStringList sections = settings.childGroups();
    for (const QString& section : sections) {
        settings.beginGroup(section);
        IconSetting value;
        value.path = settings.value(QStringLiteral("path")).toString();
        value.ordinal = settings.value(QStringLiteral("ordinal")).toString();
        value.iconExt = settings.value(QStringLiteral("iconExt")).toString();
        settings.endGroup();

        extensions_.insert(section, value.iconExt);
        QIcon icon = loadConfiguredIcon(section, value);
        if (icon.isNull()) {
            if (value.iconExt.startsWith(QStringLiteral("folder"), Qt::CaseInsensitive))
                icon = IconFromExt::getFolder();
            else
                icon = IconFromExt::get(value.iconExt);
        }
        if (!icon.isNull())
            icons_.insert(section, icon);
    }
}

QIcon SettingsIcons::loadConfiguredIcon(const QString& section,
                                        const IconSetting& setting)
{
    if (setting.path.isEmpty() || setting.ordinal.isEmpty() ||
        !QFileInfo::exists(setting.path))
        return {};

    pearegui::Session session;
    if (!session.openFile(setting.path))
        return {};

    bool isOs2 = false;
    for (size_t folder = 0; folder < session.folderCount() && !isOs2; ++folder) {
        for (size_t item = 0; item < session.resourceCount(folder); ++item) {
            pearegui::Resource resource = session.openResource(folder, item);
            if (resource.isValid() && resource.context().platform == PEARE_PLATFORM_OS2) {
                isOs2 = true;
                break;
            }
        }
    }

    const QString desiredType = isOs2
        ? QStringLiteral("RT_POINTER")
        : QStringLiteral("RT_GROUP_ICON");
    const QString desiredName = normalizedName(setting.ordinal);

    for (size_t folder = 0; folder < session.folderCount(); ++folder) {
        if (session.folderType(folder).compare(desiredType, Qt::CaseInsensitive) != 0)
            continue;
        for (size_t item = 0; item < session.resourceCount(folder); ++item) {
            pearegui::Resource resource = session.openResource(folder, item);
            if (!resource.isValid())
                continue;
            const pearegui::ResourceContext context = resource.context();
            if (normalizedName(context.identifier).compare(desiredName, Qt::CaseInsensitive) != 0)
                continue;
            return selectBest16(pearegui::decode(resource));
        }
    }

    Q_UNUSED(section)
    return {};
}

QIcon SettingsIcons::selectBest16(const pearegui::Preview& preview)
{
    const QImage* selected = nullptr;
    int selectedColors = -1;
    for (const QImage& image : preview.images) {
        if (image.width() != 16 || image.height() != 16)
            continue;
        const int colors = visibleColorCount(image);
        if (!selected || colors > selectedColors) {
            selected = &image;
            selectedColors = colors;
        }
    }
    return selected ? QIcon(QPixmap::fromImage(*selected)) : QIcon();
}

QIcon SettingsIcons::selectBestSmall(const pearegui::Preview& preview)
{
    const QImage* selected = nullptr;
    int selectedColors = -1;
    for (const QImage& image : preview.images) {
        if (image.isNull())
            continue;
        const int colors = visibleColorCount(image);
        if (!selected ||
            image.width() * image.height() < selected->width() * selected->height() ||
            (image.width() * image.height() == selected->width() * selected->height() &&
             colors > selectedColors)) {
            selected = &image;
            selectedColors = colors;
        }
    }
    return selected ? QIcon(QPixmap::fromImage(*selected)) : QIcon();
}

QString SettingsIcons::defaultExtensionForType(const QString& type)
{
    static const QHash<QString, QString> defaults = {
        {QStringLiteral("RT_FONT"), QStringLiteral("fon")},
        {QStringLiteral("RT_FONTDIR"), QStringLiteral("fon")},
        {QStringLiteral("RT_VERSION"), QStringLiteral("ini")},
        {QStringLiteral("RT_MENU"), QStringLiteral("ini")},
        {QStringLiteral("RT_STRING"), QStringLiteral("ini")},
        {QStringLiteral("RT_BITMAP"), QStringLiteral("bmp")},
        {QStringLiteral("RT_POINTER"), QStringLiteral("bmp")},
        {QStringLiteral("RT_ICON"), QStringLiteral("ico")},
        {QStringLiteral("RT_GROUP_ICON"), QStringLiteral("ico")},
        {QStringLiteral("RT_CURSOR"), QStringLiteral("cur")},
        {QStringLiteral("RT_GROUP_CURSOR"), QStringLiteral("cur")},
        {QStringLiteral("RT_MANIFEST"), QStringLiteral("xml")}
    };
    return defaults.value(type, QStringLiteral("bin"));
}

QIcon SettingsIcons::iconFor(const QString& section) const
{
    return icons_.value(section);
}

QIcon SettingsIcons::iconForFileName(const QString& fileName) const
{
    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    const QString ext = dot > 0 ? fileName.mid(dot + 1).toLower() : QString();
    const QString key = QStringLiteral("ext:") + ext;
    const auto cached = resourceIconCache_.constFind(key);
    if (cached != resourceIconCache_.constEnd())
        return cached.value();
    QIcon icon = ext.isEmpty() ? defaultIcon() : IconFromExt::get(ext);
    if (icon.isNull())
        icon = defaultIcon();
    resourceIconCache_.insert(key, icon);
    return icon;
}

QIcon SettingsIcons::iconForEmbeddedFile(const QString& fileName,
                                         const pearegui::Session* session,
                                         size_t folder, size_t resource) const
{
    const QIcon fallback = iconForFileName(fileName);
    if (!session)
        return fallback;

    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    const QString ext = dot > 0 ? fileName.mid(dot + 1).toLower() : QString();
    if (ext != QStringLiteral("exe"))
        return fallback;

    pearegui::Resource sourceResource = session->openResource(folder, resource);
    const pearegui::ResourceContext sourceContext =
        sourceResource.isValid() ? sourceResource.context() : pearegui::ResourceContext{};
    const QString cacheKey =
        QStringLiteral("embedded:%1:%2:%3:%4")
            .arg(sourceContext.sourceName, sourceContext.identifier)
            .arg(sourceContext.dataOffset)
            .arg(sourceContext.dataSize);
    const auto cached = resourceIconCache_.constFind(cacheKey);
    if (cached != resourceIconCache_.constEnd())
        return cached.value();

    QIcon icon;
    std::unique_ptr<pearegui::Session> nested = session->openNested(folder, resource);
    if (nested) {
        // Same intent as Disk Manager's old PeareModule path, but entirely through
        // the unified Qt Peare opener/decoder and without a temporary .exe file.
        // OS/2 NE/LE/LX modules use RT_POINTER for their program icon, while PE
        // and Windows NE use RT_GROUP_ICON.
        bool isOs2 = false;
        for (size_t f = 0; f < nested->folderCount() && !isOs2; ++f) {
            for (size_t r = 0; r < nested->resourceCount(f); ++r) {
                pearegui::Resource candidate = nested->openResource(f, r);
                if (candidate.isValid() && candidate.context().platform == PEARE_PLATFORM_OS2) {
                    isOs2 = true;
                    break;
                }
            }
        }

        const QString primaryType = isOs2 ? QStringLiteral("RT_POINTER")
                                          : QStringLiteral("RT_GROUP_ICON");
        const QString secondaryType = isOs2 ? QStringLiteral("RT_GROUP_ICON")
                                            : QStringLiteral("RT_POINTER");
        const QString types[] = {primaryType, secondaryType};

        for (const QString& desiredType : types) {
            for (size_t f = 0; f < nested->folderCount() && icon.isNull(); ++f) {
                for (size_t r = 0; r < nested->resourceCount(f); ++r) {
                    pearegui::Resource iconResource = nested->openResource(f, r);
                    if (!iconResource.isValid())
                        continue;
                    const pearegui::ResourceContext context = iconResource.context();
                    if (context.type.compare(desiredType, Qt::CaseInsensitive) != 0)
                        continue;
                    const pearegui::Preview preview = pearegui::decode(iconResource, nested.get());
                    icon = selectBest16(preview);
                    if (icon.isNull())
                        icon = selectBestSmall(preview);
                    if (!icon.isNull())
                        break;
                }
            }
            if (!icon.isNull())
                break;
        }
    }

    if (icon.isNull())
        icon = fallback;
    resourceIconCache_.insert(cacheKey, icon);
    return icon;
}

QIcon SettingsIcons::resourceIcon(const QString& type) const
{
    QIcon icon = icons_.value(type);
    if (!icon.isNull())
        return icon;

    const auto cached = resourceIconCache_.constFind(type);
    if (cached != resourceIconCache_.constEnd())
        return cached.value();

    const QString extension = extensions_.value(type, defaultExtensionForType(type));
    icon = IconFromExt::get(extension);
    if (icon.isNull())
        icon = defaultIcon();
    resourceIconCache_.insert(type, icon);
    return icon;
}

QIcon SettingsIcons::folderOpenIcon() const
{
    const QIcon icon = icons_.value(QStringLiteral("RT_FOLDER_OPEN"));
    return icon.isNull() ? IconFromExt::getFolder() : icon;
}

QIcon SettingsIcons::folderCloseIcon() const
{
    const QIcon icon = icons_.value(QStringLiteral("RT_FOLDER_CLOSE"));
    return icon.isNull() ? IconFromExt::getFolder() : icon;
}

QIcon SettingsIcons::defaultIcon() const
{
    const QIcon icon = icons_.value(QStringLiteral("RT_DEFAULT"));
    return icon.isNull() ? IconFromExt::get(QStringLiteral("bin")) : icon;
}
