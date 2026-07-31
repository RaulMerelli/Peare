#include "IconFromExt.h"

#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>

namespace {
QString normalizedExtension(QString extension)
{
    extension = extension.trimmed();
    if (extension.startsWith(QLatin1Char('.')))
        extension.remove(0, 1);
    return extension;
}
}

QIcon IconFromExt::get(const QString& extension)
{
    QFileIconProvider provider;
    const QString suffix = normalizedExtension(extension);
    if (suffix.isEmpty())
        return provider.icon(QFileIconProvider::File);

    // The file doesn't need to exist. QFileIconProvider uses its suffix to ask
    // the platform for the associated icon, just like SHGFI_USEFILEATTRIBUTES.
    return provider.icon(QFileInfo(QStringLiteral("file.%1").arg(suffix)));
}

QIcon IconFromExt::getFolder()
{
    QFileIconProvider provider;
    return provider.icon(QFileIconProvider::Folder);
}

QIcon IconFromExt::getFile(const QString& filePath)
{
    QFileIconProvider provider;
    return provider.icon(QFileInfo(filePath));
}
