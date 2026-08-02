#pragma once

#include "PeareApi.h"

#include <QHash>
#include <QIcon>
#include <QString>

class SettingsIcons final
{
public:
    SettingsIcons();

    QIcon iconFor(const QString& section) const;
    QIcon resourceIcon(const QString& type) const;
    // Icon chosen from a file name's extension (for filesystem file entries).
    QIcon iconForFileName(const QString& fileName) const;
    QIcon folderOpenIcon() const;
    QIcon folderCloseIcon() const;
    QIcon defaultIcon() const;

private:
    struct IconSetting {
        QString path;
        QString ordinal;
        QString iconExt;
    };

    void writeDefaults();
    void load();
    static QIcon loadConfiguredIcon(const QString& section,
                                    const IconSetting& setting);
    static QIcon selectBest16(const pearegui::Preview& preview);
    static QString defaultExtensionForType(const QString& type);

    QString filePath_;
    QHash<QString, QIcon> icons_;
    QHash<QString, QString> extensions_;
    // Memoises resourceIcon() for types not in icons_: IconFromExt::get() makes a
    // shell call per type, so without this a tree of thousands of same-type
    // entries (e.g. a WIM's files) would query the shell thousands of times.
    mutable QHash<QString, QIcon> resourceIconCache_;
};
