#pragma once

#include <QIcon>
#include <QString>

// Cross-platform equivalent of IconFromExt.cs.
// The C# implementation asks the Windows shell for the small icon associated
// with a file extension or a folder. QFileIconProvider delegates the same task
// to the current platform integration, without shell32.dll/user32.dll.
class IconFromExt final
{
public:
    static QIcon get(const QString& extension);
    static QIcon getFolder();
    static QIcon getFile(const QString& filePath);
};
