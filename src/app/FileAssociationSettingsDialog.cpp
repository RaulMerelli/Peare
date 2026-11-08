#include "FileAssociationSettingsDialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLabel>
#include <QList>
#include <QListWidget>
#include <QPair>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>

#if (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)) || defined(Q_OS_MACOS)
#include <QMimeDatabase>
#include <QMimeType>
#include <QSet>
#endif

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#endif

#ifdef Q_OS_MACOS
#include <CoreServices/CoreServices.h>
#endif

namespace {
struct Category {
    const char* id;
    const char* title;
    const char* icon;
    const char* extensions;
};

const Category categories[] = {
    {"archive", "Archives and packages", "archive.ico",
     "zip;zipx;jar;war;ear;aar;apk;apks;aab;epub;docx;xlsx;pptx;odt;ods;odp;ipa;ipsw;xpi;"
     "nupkg;snupkg;whl;vsix;cbz;kmz;zap;tar;tgz;tbz;tbz2;txz;tlz;rar;7z;arj;lha;lzh;"
     "cab;chm;wpi;rpm;deb;udeb;cpio;xar;wad;asar;appx;msix;appxbundle;xap;gz;gzip;bz2;"
     "bzip2;xz;lzma;zst;z;uue;uu;ace;alz;arc;ark;pak;pack;pack2;lz;"},
    {"disk", "Disk images", "disk.ico",
     "vmdk;vhd;vhdx;vdi;qcow;qcow2;hdd;img;ima;vfd;dsk;imz;raw;dd;"},
    {"filesystem", "File systems", "disk.ico",
     "fat;fat12;fat16;fat32;exfat;ntfs;ext;ext2;ext3;ext4;xfs;jfs;hpfs;btrfs;squashfs;"
     "cramfs;hfs;hfsx;udf;swap;lvm;"},
    {"optical", "CD and DVD images", "optical.ico",
     "iso;bin;cue;mdf;mds;dmg;udf;"},
    {"font", "Fonts", "font.ico",
     "fnt;fon;font;ttf;ttc;otf;dfont;woff;woff2;eot;bdf;pcf;snf;dwf;afm;pfm;pfa;pfb;"
     "type1;type3;ofm;shp;shx;sfn;psf;psfu;psf1;psf2;pk;vfont;fb;bmf;"},
    {"image", "Images", "image.ico",
     "png;jpg;jpeg;pcx;pcc;gif;bmp;tif;tiff;ico;cur;wmf;emf;webp;"},
    {"binary", "Executables and binary files", "archive.ico",
     "exe;dll;ocx;sys;efi;xbe;xex;pup;imgfs;fwf;fsf;msi;msm;msp;mst;pcp;cub;msg;"},
    {"text", "Text and source files", "text.ico",
     "txt;log;cfg;ini;xml;json;yaml;yml;csv;md;asm;c;cc;cpp;h;hpp;py;ps1;bat;cmd;sh;"
     "sql;html;css;js;ts;hex;ihex;resx;bcd;hiv;reg;"},
    {"backup", "Backups and deployment images", "disk.ico",
     "bkf;mtf;wim;swm;ffu;sdi;"},
    {"firmware", "Firmware and device images", "floppy.ico",
     "fw;rom;bin;img;ima;vfd;dsk;hex;ihex;fwf;fsf;"}
};

QStringList extensionsFor(const Category& category) {
    QStringList extensions = QString::fromLatin1(category.extensions)
        .split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (QString& extension : extensions)
        extension = extension.trimmed().toLower();
    extensions.removeAll(QString());
    return extensions;
}

QString applicationConfigFile() {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(directory);
    return QDir(directory).filePath(QStringLiteral("file-associations.ini"));
}

#ifdef Q_OS_WIN
bool setRegistryString(const QString& key, const QString& name, const QString& value) {
    HKEY handle = nullptr;
    const std::wstring keyName = key.toStdWString();
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyName.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring valueName = name.toStdWString();
    const std::wstring text = value.toStdWString();
    const DWORD bytes = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
    const LONG result = RegSetValueExW(handle, valueName.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(text.c_str()), bytes);
    RegCloseKey(handle);
    return result == ERROR_SUCCESS;
}

bool deleteRegistryValue(const QString& key, const QString& name) {
    HKEY handle = nullptr;
    const std::wstring keyName = key.toStdWString();
    const LONG opened = RegOpenKeyExW(HKEY_CURRENT_USER, keyName.c_str(), 0,
        KEY_SET_VALUE, &handle);
    if (opened == ERROR_FILE_NOT_FOUND)
        return true;
    if (opened != ERROR_SUCCESS)
        return false;
    const std::wstring valueName = name.toStdWString();
    const LONG result = RegDeleteValueW(handle, valueName.c_str());
    RegCloseKey(handle);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

QString registryString(const QString& key, const QString& name) {
    HKEY handle = nullptr;
    const std::wstring keyName = key.toStdWString();
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyName.c_str(), 0,
            KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS)
        return {};
    const std::wstring valueName = name.toStdWString();
    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegQueryValueExW(handle, valueName.c_str(), nullptr, &type, nullptr, &bytes);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
        bytes < sizeof(wchar_t)) {
        RegCloseKey(handle);
        return {};
    }
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    result = RegQueryValueExW(handle, valueName.c_str(), nullptr, &type,
        reinterpret_cast<BYTE*>(value.data()), &bytes);
    RegCloseKey(handle);
    return result == ERROR_SUCCESS ? QString::fromWCharArray(value.data()) : QString();
}

QString windowsCapabilitiesKey() {
    return QStringLiteral("Software\\Peare\\Capabilities\\FileAssociations");
}

QString windowsProgId(const Category& category) {
    return QStringLiteral("Peare.") + QString::fromLatin1(category.id);
}

bool registerWindowsProgId(const Category& category) {
    const QString progId = windowsProgId(category);
    const QString classKey = QStringLiteral("Software\\Classes\\") + progId;
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString iconPath = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("filetypes/") + QString::fromLatin1(category.icon));
    const QString icon = QFileInfo::exists(iconPath)
        ? QDir::toNativeSeparators(iconPath) + QStringLiteral(",0")
        : executable + QStringLiteral(",0");
    const QString label = QStringLiteral("Peare - ") + QString::fromLatin1(category.title);
    return setRegistryString(classKey, {}, label) &&
        setRegistryString(classKey, QStringLiteral("FriendlyTypeName"), label) &&
        setRegistryString(classKey + QStringLiteral("\\DefaultIcon"), {}, icon) &&
        setRegistryString(classKey + QStringLiteral("\\shell\\open\\command"), {},
            QStringLiteral("\"") + executable + QStringLiteral("\" \"%1\""));
}

bool hasWindowsAssociation(const Category& category, const QString& extension) {
    const std::wstring fileExtension = (QStringLiteral(".") + extension).toStdWString();
    DWORD characterCount = 0;
    const HRESULT sizeResult = AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_PROGID,
        fileExtension.c_str(), nullptr, nullptr, &characterCount);
    if (sizeResult != S_FALSE || characterCount == 0)
        return false;
    std::vector<wchar_t> progId(characterCount, L'\0');
    const HRESULT queryResult = AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_PROGID,
        fileExtension.c_str(), nullptr, progId.data(), &characterCount);
    return SUCCEEDED(queryResult) && QString::fromWCharArray(progId.data()) ==
        windowsProgId(category);
}

int applyWindowsAssociations(const QList<QPair<const Category*, QListWidget*>>& lists) {
    int failures = 0;
    int selected = 0;
    for (const auto& entry : lists) {
        const Category& category = *entry.first;
        QListWidget* list = entry.second;
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem* item = list->item(i);
            const QString extension = item->data(Qt::UserRole).toString();
            const QString keyName = QStringLiteral(".") + extension;
            if (item->checkState() == Qt::Checked) {
                if (!registerWindowsProgId(category) ||
                    !setRegistryString(windowsCapabilitiesKey(), keyName,
                        windowsProgId(category))) {
                    ++failures;
                    continue;
                }
                ++selected;
            } else if (!deleteRegistryValue(windowsCapabilitiesKey(), keyName)) {
                ++failures;
            }
        }
    }
    if (selected) {
        if (!setRegistryString(QStringLiteral("Software\\Peare\\Capabilities"),
                QStringLiteral("ApplicationName"), QStringLiteral("Peare")) ||
            !setRegistryString(QStringLiteral("Software\\Peare\\Capabilities"),
                QStringLiteral("ApplicationDescription"),
                QStringLiteral("Open supported file types with Peare.")) ||
            !setRegistryString(QStringLiteral("Software\\RegisteredApplications"),
                QStringLiteral("Peare"), QStringLiteral("Software\\Peare\\Capabilities")))
            ++failures;
    } else {
        if (!deleteRegistryValue(QStringLiteral("Software\\RegisteredApplications"),
                QStringLiteral("Peare")))
            ++failures;
        const std::wstring capabilities = QStringLiteral("Software\\Peare\\Capabilities")
            .toStdWString();
        const LONG result = RegDeleteTreeW(HKEY_CURRENT_USER, capabilities.c_str());
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
            ++failures;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return failures;
}

void openWindowsDefaultApps() {
    QDesktopServices::openUrl(QUrl(QStringLiteral(
        "ms-settings:defaultapps?registeredAppUser=Peare")));
}
#endif

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
const QString linuxDesktopId = QStringLiteral("peare.desktop");

QString mimeForExtension(const QString& extension) {
    QMimeDatabase database;
    const QMimeType mime = database.mimeTypeForFile(
        QStringLiteral("sample.") + extension, QMimeDatabase::MatchExtension);
    if (!mime.isValid() || mime.name() == QStringLiteral("application/octet-stream"))
        return {};
    return mime.name();
}

QString desktopApplicationsDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("applications"));
}

QString mimeAppsFile() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
        .filePath(QStringLiteral("mimeapps.list"));
}

QString mimePreferenceKey(const QString& mime) {
    QString key = mime;
    key.replace(QLatin1Char('/'), QLatin1Char('_'));
    return key;
}

bool hasLinuxDefault(const QString& extension) {
    const QString mime = mimeForExtension(extension);
    if (mime.isEmpty())
        return false;
    QSettings mimeApps(mimeAppsFile(), QSettings::IniFormat);
    mimeApps.beginGroup(QStringLiteral("Default Applications"));
    return mimeApps.value(mime).toString().split(QLatin1Char(';'), Qt::SkipEmptyParts)
        .value(0) == linuxDesktopId;
}

bool writeDesktopFile(const QStringList& mimeTypes) {
    const QString directory = desktopApplicationsDirectory();
    if (!QDir().mkpath(directory))
        return false;
    const QString path = QDir(directory).filePath(linuxDesktopId);
    if (mimeTypes.isEmpty()) {
        QFile::remove(path);
        return true;
    }
    QString executable = QCoreApplication::applicationFilePath();
    executable.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    executable.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    const QByteArray contents = QStringLiteral(
        "[Desktop Entry]\nType=Application\nName=Peare\nExec=\"%1\" %f\n"
        "Terminal=false\nIcon=application-x-executable\nMimeType=%2;\n")
        .arg(executable, mimeTypes.join(QLatin1Char(';'))).toUtf8();
    return file.write(contents) == contents.size() && file.commit();
}

int applyLinuxAssociations(const QList<QPair<const Category*, QListWidget*>>& lists) {
    QSet<QString> selected;
    for (const auto& entry : lists) {
        for (int i = 0; i < entry.second->count(); ++i) {
            const QListWidgetItem* item = entry.second->item(i);
            if (item->checkState() != Qt::Checked)
                continue;
            const QString mime = mimeForExtension(item->data(Qt::UserRole).toString());
            if (!mime.isEmpty())
                selected.insert(mime);
        }
    }

    QSettings state(applicationConfigFile(), QSettings::IniFormat);
    state.setIniCodec("UTF-8");
    const QStringList previous = state.value(QStringLiteral("selectedMimeTypes")).toStringList();
    QSettings mimeApps(mimeAppsFile(), QSettings::IniFormat);
    mimeApps.setIniCodec("UTF-8");
    mimeApps.beginGroup(QStringLiteral("Default Applications"));
    state.beginGroup(QStringLiteral("previousDefaults"));
    for (const QString& mime : selected) {
        if (!previous.contains(mime))
            state.setValue(mimePreferenceKey(mime), mimeApps.value(mime).toString());
        mimeApps.setValue(mime, linuxDesktopId + QLatin1Char(';'));
    }
    for (const QString& mime : previous) {
        if (selected.contains(mime))
            continue;
        const QString current = mimeApps.value(mime).toString();
        if (current.split(QLatin1Char(';'), Qt::SkipEmptyParts).value(0) == linuxDesktopId) {
            const QString restore = state.value(mimePreferenceKey(mime)).toString();
            if (restore.isEmpty())
                mimeApps.remove(mime);
            else
                mimeApps.setValue(mime, restore);
        }
        state.remove(mimePreferenceKey(mime));
    }
    state.endGroup();
    mimeApps.endGroup();
    QStringList selectedList = selected.values();
    selectedList.sort();
    state.setValue(QStringLiteral("selectedMimeTypes"), selectedList);
    state.sync();
    mimeApps.sync();
    if (state.status() != QSettings::NoError || mimeApps.status() != QSettings::NoError)
        return -1;
    if (!writeDesktopFile(selectedList))
        return -1;
    const QString updater = QStandardPaths::findExecutable(QStringLiteral("update-desktop-database"));
    if (!updater.isEmpty())
        QProcess::execute(updater, {desktopApplicationsDirectory()});
    return selected.size();
}
#endif

#ifdef Q_OS_MACOS
const QString macBundleId = QStringLiteral("org.peare.app");

CFStringRef toCFString(const QString& value) {
    return CFStringCreateWithCharacters(kCFAllocatorDefault,
        reinterpret_cast<const UniChar*>(value.utf16()), CFIndex(value.size()));
}

QString utiForExtension(const QString& extension) {
    CFStringRef tag = toCFString(extension);
    if (!tag)
        return {};
    CFStringRef uti = UTTypeCreatePreferredIdentifierForTag(
        kUTTagClassFilenameExtension, tag, nullptr);
    CFRelease(tag);
    if (!uti)
        return {};
    const CFIndex length = CFStringGetLength(uti);
    QString value(length, Qt::Uninitialized);
    CFStringGetCharacters(uti, CFRangeMake(0, length),
        reinterpret_cast<UniChar*>(value.data()));
    CFRelease(uti);
    return value;
}

QString macDefaultHandler(const QString& uti) {
    CFStringRef contentType = toCFString(uti);
    if (!contentType)
        return {};
    CFStringRef handler = LSCopyDefaultRoleHandlerForContentType(
        contentType, kLSRolesAll);
    CFRelease(contentType);
    if (!handler)
        return {};
    const CFIndex length = CFStringGetLength(handler);
    QString value(length, Qt::Uninitialized);
    CFStringGetCharacters(handler, CFRangeMake(0, length),
        reinterpret_cast<UniChar*>(value.data()));
    CFRelease(handler);
    return value;
}

bool hasMacDefault(const QString& extension) {
    const QString uti = utiForExtension(extension);
    return !uti.isEmpty() && macDefaultHandler(uti) == macBundleId;
}

bool setMacDefault(const QString& uti, const QString& handlerId) {
    CFStringRef contentType = toCFString(uti);
    CFStringRef handler = toCFString(handlerId);
    if (!contentType || !handler) {
        if (contentType) CFRelease(contentType);
        if (handler) CFRelease(handler);
        return false;
    }
    const OSStatus status = LSSetDefaultRoleHandlerForContentType(
        contentType, kLSRolesAll, handler);
    CFRelease(contentType);
    CFRelease(handler);
    return status == noErr;
}

int applyMacAssociations(const QList<QPair<const Category*, QListWidget*>>& lists) {
    QSet<QString> selected;
    for (const auto& entry : lists) {
        for (int i = 0; i < entry.second->count(); ++i) {
            const QListWidgetItem* item = entry.second->item(i);
            if (item->checkState() == Qt::Checked) {
                const QString uti = utiForExtension(item->data(Qt::UserRole).toString());
                if (!uti.isEmpty())
                    selected.insert(uti);
            }
        }
    }

    QSettings state(applicationConfigFile(), QSettings::IniFormat);
    state.beginGroup(QStringLiteral("macPreviousHandlers"));
    const QStringList previousTypes = state.childKeys();
    int failures = 0;
    for (const QString& uti : selected) {
        if (!previousTypes.contains(uti)) {
            const QString current = macDefaultHandler(uti);
            if (!current.isEmpty() && current != macBundleId)
                state.setValue(uti, current);
        }
        if (!setMacDefault(uti, macBundleId))
            ++failures;
    }
    for (const QString& uti : previousTypes) {
        if (selected.contains(uti))
            continue;
        const QString current = macDefaultHandler(uti);
        const QString previous = state.value(uti).toString();
        if (current == macBundleId && !previous.isEmpty() &&
            !setMacDefault(uti, previous))
            ++failures;
        state.remove(uti);
    }
    state.endGroup();
    state.sync();
    return failures ? -1 : selected.size();
}
#endif
}

FileAssociationSettingsDialog::FileAssociationSettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("File associations"));
    resize(820, 520);
    auto* layout = new QVBoxLayout(this);
    auto* body = new QHBoxLayout;
    layout->addLayout(body);
    auto* categoryList = new QListWidget(this);
    categoryList->setObjectName(QStringLiteral("associationCategories"));
    categoryList->setMinimumWidth(190);
    categoryList->setMaximumWidth(240);
    auto* pages = new QStackedWidget(this);
    body->addWidget(categoryList);
    body->addWidget(pages, 1);

    QList<QPair<const Category*, QListWidget*>> lists;
    QStringList seenExtensions;
    for (const Category& category : categories) {
        auto* page = new QWidget(pages);
        auto* pageLayout = new QVBoxLayout(page);
        auto* selectAll = new QCheckBox(QStringLiteral("Select all extensions in this category"), page);
        pageLayout->addWidget(selectAll);
        auto* list = new QListWidget(page);
        list->setObjectName(QStringLiteral("associationExtensions"));
        pageLayout->addWidget(list);
        for (const QString& extension : extensionsFor(category)) {
            if (seenExtensions.contains(extension))
                continue;
            seenExtensions.append(extension);
            auto* item = new QListWidgetItem(QStringLiteral("*.") + extension, list);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
#ifdef Q_OS_WIN
            item->setCheckState(hasWindowsAssociation(category, extension)
                ? Qt::Checked : Qt::Unchecked);
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
            item->setCheckState(hasLinuxDefault(extension)
                ? Qt::Checked : Qt::Unchecked);
            if (mimeForExtension(extension).isEmpty())
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
#elif defined(Q_OS_MACOS)
            item->setCheckState(hasMacDefault(extension)
                ? Qt::Checked : Qt::Unchecked);
#else
            item->setCheckState(Qt::Unchecked);
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
#endif
            item->setData(Qt::UserRole, extension);
        }
        list->sortItems(Qt::AscendingOrder);
        connect(selectAll, &QCheckBox::stateChanged, list, [list](int state) {
            const Qt::CheckState checkState = state == Qt::Checked ? Qt::Checked : Qt::Unchecked;
            for (int i = 0; i < list->count(); ++i)
                list->item(i)->setCheckState(checkState);
        });
        auto* categoryItem = new QListWidgetItem(QString::fromLatin1(category.title), categoryList);
        categoryItem->setIcon(QIcon(QStringLiteral(":/filetypes/") +
            QString::fromLatin1(category.icon)));
        pages->addWidget(page);
        lists.append(qMakePair(&category, list));
    }
    connect(categoryList, &QListWidget::currentRowChanged,
            pages, &QStackedWidget::setCurrentIndex);
    categoryList->setCurrentRow(0);

    auto* status = new QLabel(this);
    status->setObjectName(QStringLiteral("associationStatus"));
    layout->addWidget(status);
#if defined(Q_OS_ANDROID)
    status->setText(QStringLiteral(
        "Android does not provide an API for apps to assign file defaults. Open files with Peare from the system file picker."));
#elif defined(Q_OS_WIN)
    status->setText(QStringLiteral(
        "Windows requires you to confirm defaults in system Settings. Peare does not change Open with lists."));
#elif defined(Q_OS_LINUX)
    status->setText(QStringLiteral(
        "Selected file types become your per-user defaults. Clearing a type restores its previous default."));
#elif defined(Q_OS_MACOS)
    status->setText(QStringLiteral(
        "Selected file types become your defaults. Clearing a type restores its previous handler when known."));
#else
    status->setText(QStringLiteral(
        "This platform does not provide file-default management in Peare."));
#endif

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked,
            this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
        [this, lists, status] {
#ifdef Q_OS_WIN
            const int failures = applyWindowsAssociations(lists);
            status->setText(failures
                ? QStringLiteral("Some file types could not be registered.")
                : QStringLiteral("Peare is registered for the selected types. Confirm each default in Windows Settings. Open with lists were not changed."));
            if (!failures)
                openWindowsDefaultApps();
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
            const int applied = applyLinuxAssociations(lists);
            status->setText(applied < 0
                ? QStringLiteral("Could not update the per-user file defaults.")
                : QStringLiteral("Default applications updated for %1 MIME types.").arg(applied));
#elif defined(Q_OS_ANDROID)
            status->setText(QStringLiteral(
                "Android manages file handlers in the system. Open files with Peare from the system file picker."));
#elif defined(Q_OS_MACOS)
            const int applied = applyMacAssociations(lists);
            status->setText(applied < 0
                ? QStringLiteral("Could not update some macOS file defaults.")
                : QStringLiteral("Default handlers updated for %1 file types.").arg(applied));
#else
            Q_UNUSED(lists)
#endif
        });
}
