#pragma once

#include "PeareApi.h"

#include <QHash>
#include <QImage>
#include <QIcon>
#include <QString>

class SettingsIcons final
{
public:
    using ConfiguredImages = QHash<QString, QImage>;

    SettingsIcons();

    QString settingsFilePath() const;
    static ConfiguredImages loadConfiguredImages(const QString& filePath);
    void applyConfiguredImages(const ConfiguredImages& images);

    QIcon iconFor(const QString& section) const;
    QIcon resourceIcon(const QString& type) const;
    // Icon chosen from a file name's extension (for filesystem file entries).
    QIcon iconForFileName(const QString& fileName) const;
    // For executable files exposed by an opened container, ask Peare itself for
    // the first embedded RT_GROUP_ICON instead of relying on the shell suffix.
    QIcon iconForEmbeddedFile(const QString& fileName, const pearegui::Session* session,
                              size_t folder, size_t resource) const;
    QIcon folderOpenIcon() const;
    QIcon folderCloseIcon() const;

private:
    struct IconSetting {
        QString path;
        QString ordinal;
        QString iconExt;
    };

    void writeDefaults();
    void load();
    static QImage selectBest16(const pearegui::Preview& preview);
    static QIcon selectBestSmall(const pearegui::Preview& preview);
    static QString defaultExtensionForType(const QString& type);

    QString filePath_;
    QHash<QString, QIcon> icons_;
    QHash<QString, QString> extensions_;
    // Memoises resourceIcon() for types not in icons_: IconFromExt::get() makes a
    // shell call per type, so without this a tree of thousands of same-type
    // entries (e.g. a WIM's files) would query the shell thousands of times.
    mutable QHash<QString, QIcon> resourceIconCache_;
};
