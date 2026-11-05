#pragma once

#include <QIcon>
#include <QString>

// Get extension and folder icons through Qt's platform integration.
class IconFromExt final
{
public:
    static QIcon get(const QString& extension);
    static QIcon getFolder();
    static QIcon getFile(const QString& filePath);
};
