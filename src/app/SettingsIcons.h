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
};
