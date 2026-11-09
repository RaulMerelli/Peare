#include "MainWindow.h"
#include "FileAssociationSettingsDialog.h"
#include <functional>
#include "IconFromExt.h"
#include "peare/peare_writer.h"

#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QApplication>
#include <QMetaObject>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QDir>
#include <QDirIterator>
#include <QImageReader>
#include <QFrame>
#include <QFontDatabase>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPushButton>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QSizePolicy>
#include <QToolButton>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTimer>
#include <QThread>
#include <QUrl>
#include <QVector>
#include <QVBoxLayout>

#include <cstring>
#include <functional>
#include <limits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace {
enum ItemRole {
    TypeRole = Qt::UserRole + 1,
    NameRole,
    LanguageRole,
    OffsetRole,
    SizeRole,
    CodePageRole,
    FolderIndexRole,
    ResourceIndexRole,
    BaseIdRole,
    SessionRole, // pearegui::Session* that owns this item's folder/resource index
    PendingRole, // true: a lazy container whose children are not populated yet
    PlaceholderRole,
    EmbeddedIconQueuedRole,
    LocalPathRole,
    ChildSessionRole,
    DirectoryLoadedRole,
    DirectResourceRole,
    DirectPayloadIndexRole,
    DirectCollectionRole,
    DetailsSourceRole = Qt::UserRole + 100 // QTreeWidgetItem* backing a Details row
};

const quint64 kMaxHexPreviewBytes = 1024 * 1024;
const quint64 kMaxDecodePreviewBytes = 16 * 1024 * 1024;
constexpr int kFontGlyphMapPageSize = 256;

bool hasVisibleLanguage(const QString& language) {
    QString value = language.trimmed();
    if (value.isEmpty()) return false;

    if (value.startsWith(QLatin1Char('#'))) value.remove(0, 1);

    if (value.compare(QStringLiteral("neutral"), Qt::CaseInsensitive) == 0) return false;

    bool numeric = false;
    const uint languageId = value.toUInt(&numeric, 0);
    return !numeric || languageId != 0;
}

QString formatDetailsSize(qulonglong bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    const int precision = unit == 0 ? 0 : (value < 10.0 ? 2 : (value < 100.0 ? 1 : 0));
    return QStringLiteral("%1 %2").arg(QString::number(value, 'f', precision),
                                       QString::fromLatin1(units[unit]));
}

QString fileNameExtension(QString fileName) {
    const int slash = qMax(fileName.lastIndexOf(QLatin1Char('/')),
                           fileName.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0)
        fileName.remove(0, slash + 1);

    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == fileName.size() - 1)
        return {};
    return fileName.mid(dot + 1).toLower();
}

bool hasFileExtension(const QString& fileName) {
    return !fileNameExtension(fileName).isEmpty();
}

bool hasEmbeddedExecutableIcon(const QString& fileName) {
    return fileNameExtension(fileName) == QStringLiteral("exe");
}

QString iconNameForItem(QTreeWidgetItem* item) {
    if (!item)
        return {};
    const QString resourceName = item->data(0, NameRole).toString();
    return resourceName.isEmpty() ? item->text(0) : resourceName;
}

bool isDirectoryEntryType(const QString& type) {
    return type.endsWith(QStringLiteral("_DIR"), Qt::CaseInsensitive) ||
           type.endsWith(QStringLiteral("_FOLDER"), Qt::CaseInsensitive) ||
           type.endsWith(QStringLiteral("_DIRECTORY"), Qt::CaseInsensitive);
}

bool isFileEntryType(const QString& type) {
    return type.endsWith(QStringLiteral("_FILE"), Qt::CaseInsensitive) ||
           type.endsWith(QStringLiteral("_LINK"), Qt::CaseInsensitive);
}

bool itemIsDirectoryLike(QTreeWidgetItem* item) {
    if (!item)
        return false;
    if (item->data(0, LocalPathRole).isValid())
        return QFileInfo(item->data(0, LocalPathRole).toString()).isDir();

    const QVariant typeValue = item->data(0, TypeRole);
    if (!typeValue.isValid())
        return item->childCount() > 0;
    const QString type = typeValue.toString();
    if (isDirectoryEntryType(type))
        return true;
    if (isFileEntryType(type))
        return false;
    return item->childCount() > 0;
}

bool itemIsFileLike(QTreeWidgetItem* item) {
    if (!item)
        return false;
    if (item->data(0, LocalPathRole).isValid())
        return !QFileInfo(item->data(0, LocalPathRole).toString()).isDir();

    const QVariant typeValue = item->data(0, TypeRole);
    if (!typeValue.isValid())
        return false;
    const QString type = typeValue.toString();
    if (isDirectoryEntryType(type))
        return false;
    return isFileEntryType(type) || hasFileExtension(iconNameForItem(item));
}

QString formatRawDump(const QByteArray& data) {
    if (data.isEmpty()) return QStringLiteral("No data.");

    static const char hexDigits[] = "0123456789ABCDEF";
    const qsizetype lineCount = (data.size() + 15) / 16;
    QString result;
    result.reserve(int(lineCount * 76 + 2));

    for (qsizetype line = 0; line < data.size(); line += 16) {
        const int lineLength = int(qMin<qsizetype>(16, data.size() - line));
        result += QString::number(line, 16).toUpper().rightJustified(4, QLatin1Char('0'));
        result += QStringLiteral(": ");

        for (int column = 0; column < 16; ++column) {
            if (column < lineLength) {
                const uchar value = uchar(data.at(line + column));
                result += QLatin1Char(hexDigits[value >> 4]);
                result += QLatin1Char(hexDigits[value & 0x0F]);
                result += QLatin1Char(' ');
            } else {
                result += QStringLiteral("   ");
            }
        }

        result += QStringLiteral("| ");
        for (int column = 0; column < lineLength; ++column) {
            const uchar value = uchar(data.at(line + column));
            result += (value >= 32 && value <= 126) ? QChar(value) : QChar(u'.');
        }
        result += QStringLiteral("\r\n");
    }

    result += QStringLiteral("\r\n");
    return result;
}

bool localDirectory(QTreeWidgetItem* item) {
    if (!item || !item->data(0, LocalPathRole).isValid()) return false;
    const QFileInfo info(item->data(0, LocalPathRole).toString());
    return info.isDir();
}

#ifdef Q_OS_WIN
bool createWindowsJunction(const QString& linkPath, const QString& targetPath) {
    struct MountPointReparseBuffer {
        ULONG ReparseTag;
        USHORT ReparseDataLength;
        USHORT Reserved;
        USHORT SubstituteNameOffset;
        USHORT SubstituteNameLength;
        USHORT PrintNameOffset;
        USHORT PrintNameLength;
        WCHAR PathBuffer[1];
    };
    if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(linkPath.utf16()), nullptr)) return false;
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(linkPath.utf16()), GENERIC_WRITE, 0,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(reinterpret_cast<LPCWSTR>(linkPath.utf16()));
        return false;
    }

    QString absolute = QDir::toNativeSeparators(QFileInfo(targetPath).absoluteFilePath());
    if (absolute.startsWith(QStringLiteral("\\\\?\\"))) absolute.remove(0, 4);
    const bool unc = absolute.startsWith(QStringLiteral("\\\\"));
    const std::wstring substitute = unc
        ? L"\\??\\UNC\\" + absolute.mid(2).toStdWString()
        : L"\\??\\" + absolute.toStdWString();
    const std::wstring printName = absolute.toStdWString();
    const DWORD substituteBytes = DWORD(substitute.size() * sizeof(wchar_t));
    const DWORD printBytes = DWORD(printName.size() * sizeof(wchar_t));
    const DWORD dataBytes = DWORD(8 + substituteBytes + sizeof(wchar_t) + printBytes);
    std::vector<BYTE> buffer(offsetof(MountPointReparseBuffer, PathBuffer) + dataBytes);
    auto* reparse = reinterpret_cast<MountPointReparseBuffer*>(buffer.data());
    reparse->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->ReparseDataLength = USHORT(dataBytes);
    reparse->Reserved = 0;
    reparse->SubstituteNameOffset = 0;
    reparse->SubstituteNameLength = USHORT(substituteBytes);
    reparse->PrintNameOffset = USHORT(substituteBytes + sizeof(wchar_t));
    reparse->PrintNameLength = USHORT(printBytes);
    wchar_t* names = reparse->PathBuffer;
    std::memcpy(names, substitute.data(), substituteBytes);
    names[substitute.size()] = L'\0';
    std::memcpy(names + substitute.size() + 1, printName.data(), printBytes);

    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, reparse,
                                    DWORD(buffer.size()), nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    if (!ok) RemoveDirectoryW(reinterpret_cast<LPCWSTR>(linkPath.utf16()));
    return ok != FALSE;
}

bool removeFilesystemEntry(const QString& path) {
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY)
            return RemoveDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16())) != FALSE;
        return DeleteFileW(reinterpret_cast<LPCWSTR>(path.utf16())) != FALSE;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return DeleteFileW(reinterpret_cast<LPCWSTR>(path.utf16())) != FALSE;
    const QFileInfoList entries = QDir(path).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries)
        if (!removeFilesystemEntry(entry.absoluteFilePath())) return false;
    return RemoveDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16())) != FALSE;
}

bool renameFilesystemEntry(const QString& from, const QString& to) {
    return MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),
                       reinterpret_cast<LPCWSTR>(to.utf16()), MOVEFILE_WRITE_THROUGH) != FALSE;
}
#else
bool removeFilesystemEntry(const QString& path) {
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isDir()) return QFile::remove(path);
    const QFileInfoList entries = QDir(path).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries)
        if (!removeFilesystemEntry(entry.absoluteFilePath())) return false;
    return QDir().rmdir(path);
}

bool renameFilesystemEntry(const QString& from, const QString& to) {
    return QFile::rename(from, to) || QDir().rename(from, to);
}
#endif
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Peare"));
    resize(1067, 498);
    setMinimumSize(760, 360);
    buildMenus();
    buildCentralUi();
}

MainWindow::~MainWindow() {
    if (iconLoadThread_ && iconLoadThread_->isRunning())
        iconLoadThread_->wait();
}

void MainWindow::startBackgroundIconLoading(
    const QString& settingsPathOverride, std::function<void(size_t)> completion) {
    if (iconLoadThread_)
        return;

    const QString settingsPath = settingsPathOverride.isEmpty()
        ? settingsIcons_.settingsFilePath()
        : settingsPathOverride;
    iconLoadThread_ = QThread::create([this, settingsPath, completion] {
        const SettingsIcons::ConfiguredImages images =
            SettingsIcons::loadConfiguredImages(settingsPath);
        const size_t loadedCount = static_cast<size_t>(images.size());
        QMetaObject::invokeMethod(this, [this, images, completion, loadedCount] {
            settingsIcons_.applyConfiguredImages(images);
            refreshConfiguredIcons();
            if (completion)
                completion(loadedCount);
        }, Qt::QueuedConnection);
    });
    iconLoadThread_->setParent(this);
    iconLoadThread_->start(QThread::LowPriority);
}

void MainWindow::refreshConfiguredIcons() {
    std::function<void(QTreeWidgetItem*)> refreshItem = [&](QTreeWidgetItem* item) {
        if (!item)
            return;

        const QString name = iconNameForItem(item);
        if (item->data(0, LocalPathRole).isValid()) {
            const QFileInfo info(item->data(0, LocalPathRole).toString());
            if (info.isDir())
                item->setIcon(0, item->isExpanded() ? settingsIcons_.folderOpenIcon()
                                                    : settingsIcons_.folderCloseIcon());
            else
                item->setIcon(0, settingsIcons_.iconForFileName(info.fileName()));
        } else if (item->data(0, TypeRole).isValid()) {
            const QString type = item->data(0, TypeRole).toString();
            if (isDirectoryEntryType(type)) {
                item->setIcon(0, item->isExpanded() ? settingsIcons_.folderOpenIcon()
                                                    : settingsIcons_.folderCloseIcon());
            } else if (itemIsFileLike(item)) {
                if (!hasEmbeddedExecutableIcon(name))
                    item->setIcon(0, settingsIcons_.iconForFileName(name));
            } else if (item->childCount() > 0) {
                item->setIcon(0, item->isExpanded() ? settingsIcons_.folderOpenIcon()
                                                    : settingsIcons_.folderCloseIcon());
            } else {
                item->setIcon(0, settingsIcons_.resourceIcon(type));
            }
        } else if (item->childCount() > 0) {
            item->setIcon(0, item->isExpanded() ? settingsIcons_.folderOpenIcon()
                                                : settingsIcons_.folderCloseIcon());
        }

        for (int index = 0; index < item->childCount(); ++index)
            refreshItem(item->child(index));
    };

    for (int index = 0; index < treeView_->topLevelItemCount(); ++index)
        refreshItem(treeView_->topLevelItem(index));
    if (detailsView_) {
        for (int index = 0; index < detailsView_->topLevelItemCount(); ++index) {
            QTreeWidgetItem* row = detailsView_->topLevelItem(index);
            if (QTreeWidgetItem* source = detailsSourceItem(row))
                row->setIcon(0, source->icon(0));
        }
    }
    scheduleTreeIconLoading();
    scheduleDetailsIconLoading();
}

void MainWindow::buildMenus() {
    // Keep one Qt Widgets menu implementation on every platform. Qt 6 does not
    // expose QMenuBar through Android's native options-menu backend, so keep
    // the widget menu bar inside the QMainWindow on all targets.
    QMenuBar* applicationMenuBar = menuBar();
    applicationMenuBar->setNativeMenuBar(false);
    applicationMenuBar->setVisible(true);

    // Create actions first, with the main window as their owner, then insert
    // them into menus. This follows Qt's Main Windows / Menus example and also
    // gives the native platform menu a stable action hierarchy from startup.
    QAction* openAction = new QAction(QStringLiteral("Open &file..."), this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, [this] { openFile(); });
    QAction* openFolderAction = new QAction(QStringLiteral("Open &folder..."), this);
    connect(openFolderAction, &QAction::triggered, this, [this] { openFolder(); });
    QAction* createArchiveAction = new QAction(QStringLiteral("Add files &to..."), this);
    connect(createArchiveAction, &QAction::triggered, this,
            [this] { addFilesToNewContainer(); });

    QAction* exitAction = new QAction(QStringLiteral("E&xit"), this);
    connect(exitAction, &QAction::triggered, qApp, &QApplication::quit);

    exportOriginalAction_ = new QAction(QStringLiteral("Export &original"), this);
    exportConvertedAction_ = new QAction(QStringLiteral("Export &converted"), this);
    exportAllAction_ = new QAction(QStringLiteral("Export &all resources..."), this);
    connect(exportOriginalAction_, &QAction::triggered, this, [this] { exportSelectedOriginal(); });
    connect(exportConvertedAction_, &QAction::triggered, this, [this] {
        if (!directConvertedExtension_.isEmpty())
            exportSelectedConverted(directConvertedExtension_);
    });
    connect(exportAllAction_, &QAction::triggered, this, [this] { exportAllResources(); });

    QAction* expandAction = new QAction(QStringLiteral("&Expand treeview"), this);
    QAction* collapseAction = new QAction(QStringLiteral("&Collapse treeview"), this);
    connect(expandAction, &QAction::triggered, this, [this] { treeView_->expandAll(); });
    connect(collapseAction, &QAction::triggered, this, [this] { treeView_->collapseAll(); });

    QAction* issueAction = new QAction(QStringLiteral("Open an &issue"), this);
    connect(issueAction, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/RaulMerelli/Peare/issues")));
    });

    QAction* aboutAction = new QAction(QStringLiteral("&About"), this);
    connect(aboutAction, &QAction::triggered, this, [this] { showAbout(); });

    newFolderAction_ = new QAction(QStringLiteral("New &folder..."), this);
    newFileAction_ = new QAction(QStringLiteral("New &file..."), this);
    newSymbolicLinkAction_ = new QAction(QStringLiteral("New symbolic &link..."), this);
    newJunctionAction_ = new QAction(QStringLiteral("New &junction..."), this);
    renameAction_ = new QAction(QStringLiteral("&Rename..."), this);
    renameAction_->setShortcut(Qt::Key_F2);
    deleteAction_ = new QAction(QStringLiteral("&Delete"), this);
    deleteAction_->setShortcut(QKeySequence::Delete);
    replaceAction_ = new QAction(QStringLiteral("&Replace file..."), this);
    for (QAction* action : {newFolderAction_, newFileAction_, newSymbolicLinkAction_,
                            newJunctionAction_, renameAction_, deleteAction_, replaceAction_})
        action->setEnabled(false);
    connect(newFolderAction_, &QAction::triggered, this, [this] {
        if (folderMode_) createLocalFile(true); else createWriterEntry(true);
    });
    connect(newFileAction_, &QAction::triggered, this, [this] {
        if (folderMode_) createLocalFile(false); else createWriterEntry(false);
    });
    connect(newSymbolicLinkAction_, &QAction::triggered, this,
            [this] { createLocalLink(false); });
    connect(newJunctionAction_, &QAction::triggered, this, [this] { createLocalLink(true); });
    connect(renameAction_, &QAction::triggered, this, [this] {
        const auto items = treeView_->selectedItems();
        if (!items.isEmpty()) {
            if (folderMode_) renameLocalEntry(items.first());
            else renameWriterEntry(items.first());
        }
    });
    connect(deleteAction_, &QAction::triggered, this, [this] {
        const auto items = treeView_->selectedItems();
        if (!items.isEmpty()) {
            if (folderMode_) deleteLocalEntry(items.first());
            else deleteWriterEntry(items.first());
        }
    });
    connect(replaceAction_, &QAction::triggered, this, [this] {
        const auto items = treeView_->selectedItems();
        if (!items.isEmpty()) replaceWriterEntry(items.first());
    });

    QMenu* fileMenu = applicationMenuBar->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(openAction);
    fileMenu->addAction(openFolderAction);
    fileMenu->addSeparator();
    fileMenu->addAction(createArchiveAction);
    fileMenu->addSeparator();
    fileMenu->addAction(exitAction);

    QMenu* editMenu = applicationMenuBar->addMenu(QStringLiteral("&Edit"));
    editMenu->addAction(newFolderAction_);
    editMenu->addAction(newFileAction_);
    QMenu* linksMenu = editMenu->addMenu(QStringLiteral("New link"));
    linksMenu->addAction(newSymbolicLinkAction_);
    linksMenu->addAction(newJunctionAction_);
    editMenu->addSeparator();
    editMenu->addAction(renameAction_);
    editMenu->addAction(deleteAction_);
    editMenu->addAction(replaceAction_);

    resourceMenu_ = applicationMenuBar->addMenu(QStringLiteral("&Resource"));
    addFilesAction_ = new QAction(QStringLiteral("Add files &to..."), this);
    addFilesAction_->setEnabled(false);
    connect(addFilesAction_, &QAction::triggered, this,
            [this] {
        if (offscreenWriterScenario_ && !offscreenWriterAddSources_.isEmpty())
            addFilesToContainer(offscreenWriterAddSources_, offscreenWriterAddDestination_);
        else
            addFilesToContainer();
    });
    resourceMenu_->addAction(addFilesAction_);
    resourceMenu_->addSeparator();
    resourceMenu_->addAction(exportOriginalAction_);
    resourceMenu_->addAction(exportConvertedAction_);
    exportConvertedMenu_ = new QMenu(QStringLiteral("Export converted"), resourceMenu_);
    resourceMenu_->addSeparator();
    resourceMenu_->addAction(exportAllAction_);
    resourceMenu_->setEnabled(false);

    QMenu* viewMenu = applicationMenuBar->addMenu(QStringLiteral("&View"));
    QActionGroup* viewModeGroup = new QActionGroup(this);
    viewModeGroup->setExclusive(true);
    QAction* treeModeAction = viewMenu->addAction(QStringLiteral("&Tree"));
    QAction* detailsModeAction = viewMenu->addAction(QStringLiteral("&Details"));
    treeModeAction->setCheckable(true);
    detailsModeAction->setCheckable(true);
    treeModeAction->setChecked(true);
    viewModeGroup->addAction(treeModeAction);
    viewModeGroup->addAction(detailsModeAction);
    connect(treeModeAction, &QAction::triggered, this, [this] { setDetailsMode(false); });
    connect(detailsModeAction, &QAction::triggered, this, [this] { setDetailsMode(true); });
    viewMenu->addSeparator();
    showNavigationAction_ = viewMenu->addAction(QStringLiteral("Navigation pane"));
    showNavigationAction_->setCheckable(true);
    showNavigationAction_->setChecked(true);
    showPreviewAction_ = viewMenu->addAction(QStringLiteral("Preview pane"));
    showPreviewAction_->setCheckable(true);
    showPreviewAction_->setChecked(true);
    connect(showNavigationAction_, &QAction::toggled, this,
            [this](bool visible) { if (navigationPane_) navigationPane_->setVisible(visible); });
    connect(showPreviewAction_, &QAction::toggled, this,
            [this](bool visible) { if (previewPane_) previewPane_->setVisible(visible); });
    viewMenu->addSeparator();
    viewMenu->addAction(expandAction);
    viewMenu->addAction(collapseAction);

    QMenu* settingsMenu = applicationMenuBar->addMenu(QStringLiteral("&Settings"));
    QAction* associationsAction = settingsMenu->addAction(
        QStringLiteral("File associations..."));
    associationsAction->setObjectName(QStringLiteral("fileAssociationsAction"));
    connect(associationsAction, &QAction::triggered,
            this, &MainWindow::showFileAssociationSettings);

    QMenu* helpMenu = applicationMenuBar->addMenu(QStringLiteral("&Help"));
    helpMenu->addAction(issueAction);
    helpMenu->addSeparator();
    helpMenu->addAction(aboutAction);

}

void MainWindow::showFileAssociationSettings() {
    FileAssociationSettingsDialog dialog(this);
    dialog.exec();
}

void MainWindow::buildCentralUi() {
    QWidget* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 1, 12, 0);
    root->setSpacing(0);

    splitter_ = new QSplitter(Qt::Horizontal, central);
    splitter_->setChildrenCollapsible(true);
    splitter_->setHandleWidth(5);

    navigationPane_ = new QWidget(splitter_);
    auto* navigationLayout = new QVBoxLayout(navigationPane_);
    navigationLayout->setContentsMargins(0, 0, 0, 0);
    navigationLayout->setSpacing(3);
    navigationPane_->setMinimumWidth(0);

    navigationBar_ = new QWidget(navigationPane_);
    auto* barLayout = new QHBoxLayout(navigationBar_);
    barLayout->setContentsMargins(2, 0, 2, 0);
    barLayout->setSpacing(2);

    backButton_ = new QToolButton(navigationBar_);
    forwardButton_ = new QToolButton(navigationBar_);
    upButton_ = new QToolButton(navigationBar_);
    backButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    forwardButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    upButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    backButton_->setToolTip(QStringLiteral("Back (Alt+Left)"));
    forwardButton_->setToolTip(QStringLiteral("Forward (Alt+Right)"));
    upButton_->setToolTip(QStringLiteral("Up (Alt+Up)"));
    backButton_->setAutoRaise(true);
    forwardButton_->setAutoRaise(true);
    upButton_->setAutoRaise(true);
    barLayout->addWidget(backButton_);
    barLayout->addWidget(forwardButton_);
    barLayout->addWidget(upButton_);

    addressEdit_ = new QLineEdit(navigationBar_);
    addressEdit_->setPlaceholderText(QStringLiteral("\\"));
    addressEdit_->setClearButtonEnabled(false);
    barLayout->addWidget(addressEdit_, 1);
    navigationLayout->addWidget(navigationBar_);

    navigationStack_ = new QStackedWidget(navigationPane_);

    treeView_ = new QTreeWidget(navigationStack_);
    treeView_->setHeaderHidden(true);
    treeView_->setIndentation(18);
    treeView_->setUniformRowHeights(true);
    treeView_->setFrameShape(QFrame::StyledPanel);
    treeView_->setContextMenuPolicy(Qt::CustomContextMenu);
    navigationStack_->addWidget(treeView_);

    detailsView_ = new QTreeWidget(navigationStack_);
    detailsView_->setColumnCount(4);
    detailsView_->setHeaderLabels(QStringList() << QStringLiteral("Name") << QStringLiteral("Type")
                                                << QStringLiteral("Size") << QStringLiteral("Offset"));
    detailsView_->setRootIsDecorated(false);
    detailsView_->setItemsExpandable(false);
    detailsView_->setUniformRowHeights(true);
    detailsView_->setAllColumnsShowFocus(true);
    detailsView_->setSelectionMode(QAbstractItemView::SingleSelection);
    detailsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    detailsView_->setFrameShape(QFrame::StyledPanel);
    detailsView_->setContextMenuPolicy(Qt::CustomContextMenu);
    detailsView_->header()->setStretchLastSection(false);
    detailsView_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    detailsView_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    detailsView_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    detailsView_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    navigationStack_->addWidget(detailsView_);
    navigationLayout->addWidget(navigationStack_, 1);
    navigationBar_->setVisible(false);
    splitter_->addWidget(navigationPane_);
    splitter_->setCollapsible(0, true);

    detailsIconTimer_ = new QTimer(this);
    detailsIconTimer_->setSingleShot(true);
    connect(detailsIconTimer_, &QTimer::timeout, this, [this] { loadNextDetailsIcon(); });

    treeIconTimer_ = new QTimer(this);
    treeIconTimer_->setSingleShot(true);
    connect(treeIconTimer_, &QTimer::timeout, this, [this] { loadNextTreeIcon(); });

    previewPane_ = new QWidget(splitter_);
    previewPane_->setMinimumWidth(0);
    auto* rightLayout = new QVBoxLayout(previewPane_);
    rightLayout->setContentsMargins(0, 0, 0, 12);
    rightLayout->setSpacing(3);

    messageLabel_ = new QLabel(previewPane_);
    messageLabel_->setFixedHeight(23);
    messageLabel_->setFrameShape(QFrame::Box);
    messageLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    messageLabel_->setMargin(4);
    rightLayout->addWidget(messageLabel_);

    contentArea_ = new QScrollArea(previewPane_);
    contentArea_->setWidgetResizable(true);
    contentArea_->setFrameShape(QFrame::NoFrame);
    contentArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    contentArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    clearContent();
    rightLayout->addWidget(contentArea_, 1);

    splitter_->addWidget(previewPane_);
    splitter_->setCollapsible(1, true);
    splitter_->setSizes(QList<int>() << 341 << 700);
    root->addWidget(splitter_);
    setCentralWidget(central);
    setAcceptDrops(true);

    connect(treeView_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::showSelectedResource);
    connect(treeView_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& position) {
                showItemContextMenu(treeView_, position, treeView_->itemAt(position));
            });
    connect(detailsView_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& position) {
                QTreeWidgetItem* row = detailsView_->itemAt(position);
                QTreeWidgetItem* source = detailsSourceItem(row);
                if (source) treeView_->setCurrentItem(source);
                showItemContextMenu(detailsView_, position, row);
            });
    connect(treeView_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item) return;
                if (folderMode_ && item->data(0, LocalPathRole).isValid()) {
                    if (localDirectory(item)) {
                        if (detailsMode_) navigateDetailsTo(item);
                        else if (item->data(0, PendingRole).toBool()) expandContainerItem(item);
                    } else {
                        openFilePath(item->data(0, LocalPathRole).toString());
                    }
                } else if (item->data(0, PendingRole).toBool()) {
                    expandContainerItem(item);
                }
            });
    connect(treeView_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        expandContainerItem(item); // lazily open a nested container
        if (itemIsDirectoryLike(item))
            item->setIcon(0, settingsIcons_.folderOpenIcon());
    });
    connect(treeView_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (itemIsDirectoryLike(item))
            item->setIcon(0, settingsIcons_.folderCloseIcon());
    });

    connect(detailsView_, &QTreeWidget::itemSelectionChanged, this, [this] {
        const QList<QTreeWidgetItem*> rows = detailsView_->selectedItems();
        if (rows.isEmpty())
            return;
        QTreeWidgetItem* source = detailsSourceItem(rows.first());
        if (source)
            treeView_->setCurrentItem(source);
    });
    connect(detailsView_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* row, int) {
                QTreeWidgetItem* source = detailsSourceItem(row);
                if (!source)
                    return;
                if (folderMode_ && source->data(0, LocalPathRole).isValid()) {
                    if (localDirectory(source))
                        navigateDetailsTo(source);
                    else
                        openFilePath(source->data(0, LocalPathRole).toString());
                    return;
                }
                if (source->data(0, PendingRole).toBool() || source->childCount() > 0)
                    navigateDetailsTo(source);
                else
                    treeView_->setCurrentItem(source);
            });

    connect(backButton_, &QToolButton::clicked, this, [this] { navigateDetailsBack(); });
    connect(forwardButton_, &QToolButton::clicked, this, [this] { navigateDetailsForward(); });
    connect(upButton_, &QToolButton::clicked, this, [this] { navigateDetailsUp(); });
    connect(addressEdit_, &QLineEdit::returnPressed, this, [this] { navigateDetailsAddress(); });

    QAction* backShortcut = new QAction(this);
    backShortcut->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
    addAction(backShortcut);
    connect(backShortcut, &QAction::triggered, this, [this] { if (detailsMode_) navigateDetailsBack(); });
    QAction* forwardShortcut = new QAction(this);
    forwardShortcut->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
    addAction(forwardShortcut);
    connect(forwardShortcut, &QAction::triggered, this, [this] { if (detailsMode_) navigateDetailsForward(); });
    QAction* upShortcut = new QAction(this);
    upShortcut->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    addAction(upShortcut);
    connect(upShortcut, &QAction::triggered, this, [this] { if (detailsMode_) navigateDetailsUp(); });
}

void MainWindow::setDetailsMode(bool enabled) {
    if (!navigationStack_ || !treeView_ || !detailsView_)
        return;
    if (detailsMode_ == enabled && navigationStack_->currentWidget() == (enabled ? detailsView_ : treeView_))
        return;

    QTreeWidgetItem* selected = treeView_->currentItem();
    detailsMode_ = enabled;
    if (!enabled && detailsIconTimer_)
        detailsIconTimer_->stop();
    navigationBar_->setVisible(enabled);
    navigationStack_->setCurrentWidget(enabled ? static_cast<QWidget*>(detailsView_)
                                                : static_cast<QWidget*>(treeView_));

    if (enabled) {
        QTreeWidgetItem* location = selected;
        if (location && !location->data(0, PendingRole).toBool() && location->childCount() == 0)
            location = location->parent();
        currentDetailsLocation_ = nullptr;
        previousLocations_.clear();
        nextLocations_.clear();
        navigateDetailsTo(location, false);
        if (selected && selected->parent() == currentDetailsLocation_)
            selectDetailsSource(selected);
    } else if (selected) {
        treeView_->scrollToItem(selected);
    } else if (currentDetailsLocation_) {
        treeView_->setCurrentItem(currentDetailsLocation_);
        treeView_->scrollToItem(currentDetailsLocation_);
    }
    updateNavigationUi();
}

QString MainWindow::detailsPath(QTreeWidgetItem* item) const {
    if (!item)
        return QStringLiteral("\\");
    QStringList parts;
    for (QTreeWidgetItem* current = item; current; current = current->parent())
        parts.prepend(current->text(0));
    return QStringLiteral("\\") + parts.join(QLatin1Char('\\')) + QStringLiteral("\\");
}

QTreeWidgetItem* MainWindow::findDetailsPath(const QString& input) const {
    QString path = input.trimmed();
    path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    while (path.startsWith(QLatin1Char('\\')))
        path.remove(0, 1);
    while (path.endsWith(QLatin1Char('\\')))
        path.chop(1);
    if (path.isEmpty())
        return nullptr;

    const QStringList parts = path.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
    QTreeWidgetItem* parent = nullptr;
    for (const QString& part : parts) {
        QTreeWidgetItem* match = nullptr;
        const int count = parent ? parent->childCount() : treeView_->topLevelItemCount();
        for (int i = 0; i < count; ++i) {
            QTreeWidgetItem* child = parent ? parent->child(i) : treeView_->topLevelItem(i);
            if (child && child->text(0).compare(part, Qt::CaseInsensitive) == 0) {
                match = child;
                break;
            }
        }
        if (!match)
            return nullptr;
        parent = match;
        if (parent->data(0, PendingRole).toBool())
            const_cast<MainWindow*>(this)->expandContainerItem(parent);
    }
    return parent;
}

void MainWindow::selectResourceItem(QTreeWidgetItem* item) {
    for (QTreeWidgetItem* ancestor = item->parent(); ancestor; ancestor = ancestor->parent())
        ancestor->setExpanded(true);
    treeView_->setCurrentItem(item);
    treeView_->scrollToItem(item);
    if (detailsMode_) {
        navigateDetailsTo(item->parent(), false);
        selectDetailsSource(item);
    }
}

bool MainWindow::navigateToResource(const QString& path) {
    QTreeWidgetItem* item = findDetailsPath(path);
    if (!item) return false;
    selectResourceItem(item);
    return true;
}

bool MainWindow::navigateToFirstLeafResource() {
    QVector<QTreeWidgetItem*> pending;
    for (int index = treeView_->topLevelItemCount() - 1; index >= 0; --index)
        pending.push_back(treeView_->topLevelItem(index));
    while (!pending.isEmpty()) {
        QTreeWidgetItem* item = pending.takeLast();
        if (item->data(0, PendingRole).toBool()) {
            expandContainerItem(item);
            QApplication::processEvents();
        }
        if (item->data(0, TypeRole).isValid() && item->childCount() == 0) {
            selectResourceItem(item);
            return true;
        }
        for (int index = item->childCount() - 1; index >= 0; --index)
            pending.push_back(item->child(index));
    }
    return false;
}

bool MainWindow::inspectOffscreenPreview(OffscreenPreviewDiagnostics* diagnostics) {
    if (!diagnostics || !treeView_) return false;
    *diagnostics = {};

    QHash<QString, int> pathCounts;
    struct TreeNode {
        QTreeWidgetItem* item;
        QString parentPath;
    };
    QVector<TreeNode> nodes;
    for (int i = 0; i < treeView_->topLevelItemCount(); ++i)
        nodes.push_back({treeView_->topLevelItem(i), {}});
    for (int index = 0; index < nodes.size(); ++index) {
        const TreeNode node = nodes.at(index);
        if (!node.item || node.item->data(0, PlaceholderRole).toBool()) continue;
        const QString label = node.item->text(0);
        const QString path = node.parentPath.isEmpty()
            ? label : node.parentPath + QLatin1Char('/') + label;
        ++pathCounts[path];
        for (int child = 0; child < node.item->childCount(); ++child)
            nodes.push_back({node.item->child(child), path});
    }
    for (auto it = pathCounts.cbegin(); it != pathCounts.cend(); ++it) {
        if (it.value() > 1) diagnostics->duplicatePaths.push_back(it.key());
    }
    diagnostics->duplicatePaths.sort();

    QTreeWidgetItem* selected = treeView_->currentItem();
    if (!selected || !selected->data(0, TypeRole).isValid()) {
        diagnostics->error = QStringLiteral("The selected GUI item is not a resource.");
        return false;
    }
    diagnostics->selectedType = selected->data(0, TypeRole).toString();
    const QString type = diagnostics->selectedType;
    pearegui::Session* owner = sessionOf(selected);
    pearegui::Resource resource = owner
        ? owner->openResource(selected->data(0, FolderIndexRole).toULongLong(),
                              selected->data(0, ResourceIndexRole).toULongLong())
        : pearegui::Resource();
    if (!resource.isValid()) {
        diagnostics->error = QStringLiteral("The selected GUI resource could not be opened.");
        return false;
    }
    const pearegui::ResourceContext context = resource.context();
    diagnostics->dataSize = context.dataSize;
    diagnostics->leaf = !isDirectoryEntryType(type) && !context.isContainer &&
                        !selected->data(0, PendingRole).toBool();
    if (!diagnostics->leaf) {
        diagnostics->error = QStringLiteral("The selected GUI resource is not a leaf.");
        return false;
    }
    if (context.dataSize > 64 * 1024 * 1024) {
        diagnostics->error = QStringLiteral("The selected leaf exceeds the 64 MiB test limit.");
        return false;
    }
    const QByteArray payload = resource.payload();
    if (quint64(payload.size()) != context.dataSize) {
        diagnostics->error = QStringLiteral("The selected resource payload size does not match its context.");
        return false;
    }
    diagnostics->payloadSha256 = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
    diagnostics->previewImageCount = currentPreview_.images.size();
    for (const QImage& image : currentPreview_.images) {
        if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
            diagnostics->error = QStringLiteral("The GUI preview contains an invalid image.");
            return false;
        }
    }
    diagnostics->previewTextCharacters = currentPreview_.text.trimmed().size();
    if (!currentPreview_.error.isEmpty()) {
        diagnostics->error = currentPreview_.error;
        return false;
    }
    if (payload.isEmpty() && context.dataSize == 0) {
        if (!diagnostics->duplicatePaths.isEmpty())
            diagnostics->error = QStringLiteral("The GUI tree contains duplicate resource paths.");
        return diagnostics->duplicatePaths.isEmpty();
    }
    if (diagnostics->previewImageCount == 0 && diagnostics->previewTextCharacters == 0) {
        diagnostics->error = QStringLiteral("The selected leaf has no decoded preview content.");
        return false;
    }
    if (!diagnostics->duplicatePaths.isEmpty()) {
        diagnostics->error = QStringLiteral("The GUI tree contains duplicate resource paths.");
        return false;
    }
    return true;
}

QTreeWidgetItem* MainWindow::detailsSourceItem(QTreeWidgetItem* row) const {
    if (!row)
        return nullptr;
    const quintptr ptr = static_cast<quintptr>(row->data(0, DetailsSourceRole).toULongLong());
    return reinterpret_cast<QTreeWidgetItem*>(ptr);
}

void MainWindow::selectDetailsSource(QTreeWidgetItem* source) {
    if (!detailsView_ || !source)
        return;
    for (int i = 0; i < detailsView_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* row = detailsView_->topLevelItem(i);
        if (detailsSourceItem(row) == source) {
            detailsView_->setCurrentItem(row);
            detailsView_->scrollToItem(row);
            return;
        }
    }
}

void MainWindow::refreshDetailsView() {
    if (!detailsView_)
        return;

    if (detailsIconTimer_)
        detailsIconTimer_->stop();
    detailsIconIndex_ = 0;

    QTreeWidgetItem* previouslySelected = treeView_->currentItem();
    QSignalBlocker blocker(detailsView_);
    detailsView_->clear();

    const int count = currentDetailsLocation_ ? currentDetailsLocation_->childCount()
                                              : treeView_->topLevelItemCount();
    for (int i = 0; i < count; ++i) {
        QTreeWidgetItem* source = currentDetailsLocation_ ? currentDetailsLocation_->child(i)
                                                         : treeView_->topLevelItem(i);
        if (!source || (!source->data(0, TypeRole).isValid() && source->text(0) == QStringLiteral("...")))
            continue;

        QString typeText;
        QString sizeText;
        QString offsetText;
        const bool hasResource = source->data(0, TypeRole).isValid();
        const QString resourceType = source->data(0, TypeRole).toString();
        if (!hasResource) {
            typeText = QStringLiteral("Folder");
        } else if (resourceType.endsWith(QStringLiteral("_DIR"), Qt::CaseInsensitive)) {
            typeText = QStringLiteral("Folder");
        } else {
            typeText = resourceType;
            sizeText = formatDetailsSize(source->data(0, SizeRole).toULongLong());
            offsetText = QStringLiteral("0x%1")
                             .arg(QString::number(source->data(0, OffsetRole).toULongLong(), 16).toUpper());
        }

        auto* row = new QTreeWidgetItem(detailsView_,
                                        QStringList() << source->text(0) << typeText
                                                      << sizeText << offsetText);
        row->setIcon(0, source->icon(0));
        row->setData(0, DetailsSourceRole,
                     QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(source)));
        row->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        row->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
    }

    const bool selectedIsVisible = previouslySelected &&
        ((currentDetailsLocation_ && previouslySelected->parent() == currentDetailsLocation_) ||
         (!currentDetailsLocation_ && !previouslySelected->parent()));
    if (selectedIsVisible) {
        selectDetailsSource(previouslySelected);
    } else if (currentDetailsLocation_) {
        treeView_->setCurrentItem(currentDetailsLocation_);
    } else if (previouslySelected) {
        treeView_->clearSelection();
    }

    scheduleDetailsIconLoading();
}

void MainWindow::scheduleDetailsIconLoading() {
    if (!detailsMode_ || !detailsIconTimer_ || !detailsView_ || detailsView_->topLevelItemCount() == 0)
        return;
    detailsIconTimer_->start(0);
}

void MainWindow::loadNextDetailsIcon() {
    if (!detailsMode_ || !detailsView_ || !detailsIconTimer_)
        return;

    const int count = detailsView_->topLevelItemCount();
    int scannedRows = 0;
    while (detailsIconIndex_ < count && scannedRows++ < 256) {
        QTreeWidgetItem* row = detailsView_->topLevelItem(detailsIconIndex_++);
        QTreeWidgetItem* source = detailsSourceItem(row);
        if (!source || !source->data(0, TypeRole).isValid())
            continue;

        const QString resourceType = source->data(0, TypeRole).toString();
        const QString resourceName = source->data(0, NameRole).toString();
        if (resourceType.endsWith(QStringLiteral("_DIR"), Qt::CaseInsensitive) ||
            !hasEmbeddedExecutableIcon(resourceName))
            continue;

        pearegui::Session* session = sessionOf(source);
        const size_t folder = source->data(0, FolderIndexRole).toULongLong();
        const size_t resource = source->data(0, ResourceIndexRole).toULongLong();
        const QIcon embedded = settingsIcons_.iconForEmbeddedFile(resourceName, session, folder, resource);
        if (!embedded.isNull()) {
            source->setIcon(0, embedded);
            row->setIcon(0, embedded);
        }

        // One embedded executable per event-loop turn.  Disc images can expose
        // hundreds of EXEs in one directory; decoding them all synchronously made
        // Details appear hung even though Tree navigation itself was fine.
        break;
    }

    if (detailsIconIndex_ < count)
        detailsIconTimer_->start(1);
}

void MainWindow::scheduleTreeIconLoading() {
    if (!treeIconTimer_ || treeIconIndex_ >= pendingEmbeddedIconItems_.size())
        return;
    if (!treeIconTimer_->isActive())
        treeIconTimer_->start(0);
}

void MainWindow::loadNextTreeIcon() {
    if (!treeIconTimer_)
        return;

    if (treeIconIndex_ >= pendingEmbeddedIconItems_.size()) {
        pendingEmbeddedIconItems_.clear();
        treeIconIndex_ = 0;
        return;
    }

    QTreeWidgetItem* source = pendingEmbeddedIconItems_[treeIconIndex_++];
    if (source && source->data(0, TypeRole).isValid()) {
        const QString resourceName = source->data(0, NameRole).toString();
        if (hasEmbeddedExecutableIcon(resourceName)) {
            pearegui::Session* session = sessionOf(source);
            const size_t folder = source->data(0, FolderIndexRole).toULongLong();
            const size_t resource = source->data(0, ResourceIndexRole).toULongLong();
            const QIcon embedded = settingsIcons_.iconForEmbeddedFile(resourceName, session, folder, resource);
            if (!embedded.isNull()) {
                source->setIcon(0, embedded);
                if (detailsView_) {
                    for (int i = 0; i < detailsView_->topLevelItemCount(); ++i) {
                        QTreeWidgetItem* row = detailsView_->topLevelItem(i);
                        if (detailsSourceItem(row) == source) {
                            row->setIcon(0, embedded);
                            break;
                        }
                    }
                }
            }
        }
    }

    if (treeIconIndex_ < pendingEmbeddedIconItems_.size())
        treeIconTimer_->start(1);
    else {
        pendingEmbeddedIconItems_.clear();
        treeIconIndex_ = 0;
    }
}

void MainWindow::navigateDetailsTo(QTreeWidgetItem* location, bool saveHistory) {
    if (location && location->data(0, PendingRole).toBool())
        expandContainerItem(location);

    if (saveHistory && location != currentDetailsLocation_) {
        previousLocations_.push_back(currentDetailsLocation_);
        nextLocations_.clear();
    }
    currentDetailsLocation_ = location;
    refreshDetailsView();
    updateNavigationUi();
}

void MainWindow::navigateDetailsBack() {
    if (previousLocations_.empty())
        return;
    nextLocations_.push_back(currentDetailsLocation_);
    currentDetailsLocation_ = previousLocations_.back();
    previousLocations_.pop_back();
    refreshDetailsView();
    updateNavigationUi();
}

void MainWindow::navigateDetailsForward() {
    if (nextLocations_.empty())
        return;
    previousLocations_.push_back(currentDetailsLocation_);
    currentDetailsLocation_ = nextLocations_.back();
    nextLocations_.pop_back();
    if (currentDetailsLocation_ && currentDetailsLocation_->data(0, PendingRole).toBool())
        expandContainerItem(currentDetailsLocation_);
    refreshDetailsView();
    updateNavigationUi();
}

void MainWindow::navigateDetailsUp() {
    if (!currentDetailsLocation_)
        return;
    navigateDetailsTo(currentDetailsLocation_->parent());
}

void MainWindow::navigateDetailsAddress() {
    if (!addressEdit_)
        return;
    QString path = addressEdit_->text().trimmed();
    path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    QString rootTest = path;
    rootTest.remove(QLatin1Char('\\'));
    if (rootTest.trimmed().isEmpty()) {
        navigateDetailsTo(nullptr);
        return;
    }

    QTreeWidgetItem* target = findDetailsPath(path);
    if (!target || (!target->data(0, PendingRole).toBool() && target->childCount() == 0)) {
        updateNavigationUi();
        if (messageLabel_)
            messageLabel_->setText(QStringLiteral("Address not found: %1").arg(path));
        return;
    }
    navigateDetailsTo(target);
}

void MainWindow::updateNavigationUi() {
    if (!addressEdit_)
        return;
    QSignalBlocker blocker(addressEdit_);
    addressEdit_->setText(detailsPath(currentDetailsLocation_));
    backButton_->setEnabled(detailsMode_ && !previousLocations_.empty());
    forwardButton_->setEnabled(detailsMode_ && !nextLocations_.empty());
    upButton_->setEnabled(detailsMode_ && currentDetailsLocation_ != nullptr);
    addressEdit_->setEnabled(detailsMode_ && (folderMode_ || currentSession_.isOpen()));
}

void MainWindow::clearContent() {
    fontGlyphMapLabel_ = nullptr;
    fontGlyphMapImageLabel_ = nullptr;
    fontGlyphMapPreviousButton_ = nullptr;
    fontGlyphMapNextButton_ = nullptr;
    fontGlyphMapPayload_.clear();
    fontGlyphMapValues_.clear();
    fontGlyphMapResourceType_.clear();
    fontGlyphMapUsesGlyphIndexes_ = false;
    fontGlyphMapPage_ = 0;
    QWidget* old = contentArea_ ? contentArea_->takeWidget() : nullptr;
    if (old) old->deleteLater();
    contentWidget_ = new QWidget;
    contentWidget_->setObjectName(QStringLiteral("previewContent"));
    contentWidget_->setStyleSheet(QStringLiteral("background: white;"));
    auto* layout = new QVBoxLayout(contentWidget_);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(3);
    layout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    if (contentArea_) {
        // Reset scrolling for each preview so text uses the full viewport.
        contentArea_->setWidgetResizable(true);
        contentArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        contentArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        contentArea_->setWidget(contentWidget_);
    }
}

void MainWindow::openFile() {
    QFileDialog dialog(this, QStringLiteral("Open module"));
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);

    if (dialog.exec() != QDialog::Accepted) return;

    const QList<QUrl> urls = dialog.selectedUrls();
    if (urls.isEmpty()) return;

    const QUrl url = urls.first();
    const QString localPath = url.isLocalFile() ? url.toLocalFile() : url.toString();
    openFilePath(localPath);
}

void MainWindow::openFilePath(const QString& path) {
    openFilePath(path, QString());
}

void MainWindow::openFilePath(const QString& path, const QString& password) {
    const QFileInfo info(path);
    if (info.isDir()) {
        openFolderPath(info.absoluteFilePath());
        return;
    }
    currentFilePath_ = info.absoluteFilePath();
    showModuleSummary(currentFilePath_, password);
}

void MainWindow::showWriterError(const QString& title, const QString& message) {
    if (offscreenWriterScenario_) {
        offscreenWriterError_ = title + QStringLiteral(": ") + message;
        return;
    }
    QMessageBox::warning(this, title, message);
}

void MainWindow::reportOffscreenWriterStage(const QString& stage) {
    if (offscreenWriterProgress_) offscreenWriterProgress_(stage);
}

bool MainWindow::runOffscreenWriterScenario(const QString& imagePath,
                                            const QString& scenarioPath,
                                            const QString& reportPath,
                                            const QString& screenshotPath,
                                            QString* error) {
    auto fail = [&](const QString& message) {
        offscreenWriterScenario_ = false;
        offscreenWriterProgress_ = {};
        if (error) *error = message;
        return false;
    };
    QFile scenarioFile(scenarioPath);
    if (!scenarioFile.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Could not open scenario manifest."));
    QJsonParseError parseError{};
    const QJsonDocument scenarioDocument = QJsonDocument::fromJson(
        scenarioFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !scenarioDocument.isObject())
        return fail(QStringLiteral("Invalid scenario JSON: %1").arg(parseError.errorString()));
    const QJsonObject scenario = scenarioDocument.object();
    const QJsonArray operations = scenario.value(QStringLiteral("operations")).toArray();
    if (operations.isEmpty())
        return fail(QStringLiteral("Scenario has no operations."));

    QJsonArray results;
    auto writeProgress = [&](const QString& stage) {
        QJsonObject progress;
        progress.insert(QStringLiteral("image"), QFileInfo(imagePath).absoluteFilePath());
        progress.insert(QStringLiteral("view"), scenario.value(QStringLiteral("view")));
        progress.insert(QStringLiteral("stage"), stage);
        progress.insert(QStringLiteral("operations"), results);
        QFile reportFile(reportPath);
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            reportFile.write(QJsonDocument(progress).toJson(QJsonDocument::Indented));
            reportFile.close();
        }
    };
    offscreenWriterProgress_ = writeProgress;
    writeProgress(QStringLiteral("opening-image"));

    offscreenWriterScenario_ = true;
    offscreenWriterTargetIndex_ = scenario.value(QStringLiteral("target_index")).toInt(-1);
    offscreenWriterTextAnswers_.clear();
    offscreenWriterFileAnswers_.clear();
    offscreenWriterAddSources_.clear();
    offscreenWriterAddDestination_.clear();
    offscreenWriterTextIndex_ = 0;
    offscreenWriterFileIndex_ = 0;
    offscreenWriterError_.clear();

    resize(1280, 800);
    show();
    openFilePath(imagePath);
    QApplication::processEvents();
    if (!currentSession_.isOpen())
        return fail(QStringLiteral("The GUI could not open the test image."));
    writeProgress(QStringLiteral("image-opened"));
    setDetailsMode(scenario.value(QStringLiteral("view")).toString() ==
                   QStringLiteral("details"));
    QApplication::processEvents();

    auto normalizedViewPath = [](QString path) {
        path.replace(QLatin1Char('/'), QLatin1Char('\\'));
        while (path.startsWith(QLatin1Char('\\'))) path.remove(0, 1);
        while (path.endsWith(QLatin1Char('\\'))) path.chop(1);
        return path.isEmpty() ? QStringLiteral("\\")
                              : QStringLiteral("\\") + path + QStringLiteral("\\");
    };
    for (const QJsonValue& value : operations) {
        if (!value.isObject()) return fail(QStringLiteral("Operation must be an object."));
        const QJsonObject operation = value.toObject();
        const QString kind = operation.value(QStringLiteral("type")).toString();
        const QString viewPath = operation.value(QStringLiteral("view_path")).toString();
        QTreeWidgetItem* viewLocation = viewPath.isEmpty()
            ? nullptr : findDetailsPath(viewPath);
        if (!viewPath.isEmpty() && !viewLocation)
            return fail(QStringLiteral("View path was not found: %1").arg(viewPath));
        if (detailsMode_) {
            navigateDetailsTo(viewLocation, false);
        } else if (viewLocation) {
            for (QTreeWidgetItem* parent = viewLocation; parent; parent = parent->parent())
                parent->setExpanded(true);
        }

        const QString parentPath = operation.value(QStringLiteral("parent")).toString();
        const QString entryPath = operation.value(QStringLiteral("path")).toString();
        QTreeWidgetItem* selected = nullptr;
        if (!parentPath.isEmpty()) selected = findDetailsPath(parentPath);
        else if (!entryPath.isEmpty()) selected = findDetailsPath(entryPath);
        if ((!parentPath.isEmpty() || !entryPath.isEmpty()) && !selected)
            return fail(QStringLiteral("Selected path was not found: %1")
                        .arg(parentPath.isEmpty() ? entryPath : parentPath));
        if (selected) {
            treeView_->setCurrentItem(selected, 0, QItemSelectionModel::ClearAndSelect);
        } else {
            treeView_->clearSelection();
            treeView_->setCurrentItem(nullptr);
        }
        QApplication::processEvents();
        offscreenWriterError_.clear();
        writeProgress(QStringLiteral("operation-started: ") + kind);

        if (kind == QStringLiteral("create_directory") ||
            kind == QStringLiteral("create_file")) {
            const QString name = operation.value(QStringLiteral("name")).toString();
            if (name.isEmpty()) return fail(QStringLiteral("Create operation has no name."));
            offscreenWriterTextAnswers_ = QStringList{name};
            offscreenWriterTextIndex_ = 0;
            QAction* action = kind == QStringLiteral("create_directory")
                ? newFolderAction_ : newFileAction_;
            if (!action || !action->isEnabled())
                return fail(QStringLiteral("GUI create action is disabled: %1").arg(kind));
            action->trigger();
        } else if (kind == QStringLiteral("add_files")) {
            offscreenWriterAddSources_.clear();
            for (const QJsonValue& source : operation.value(QStringLiteral("sources")).toArray())
                offscreenWriterAddSources_.push_back(source.toString());
            if (offscreenWriterAddSources_.isEmpty())
                return fail(QStringLiteral("Add-files operation has no sources."));
            if (!addFilesAction_ || !addFilesAction_->isEnabled())
                return fail(QStringLiteral("GUI Add files action is disabled."));
            offscreenWriterAddDestination_ =
                operation.value(QStringLiteral("destination")).toString();
            addFilesAction_->trigger();
        } else if (kind == QStringLiteral("rename")) {
            offscreenWriterTextAnswers_ = QStringList{
                operation.value(QStringLiteral("name")).toString()};
            offscreenWriterTextIndex_ = 0;
            if (!renameAction_ || !renameAction_->isEnabled()) {
                peare_writer_update_capabilities capabilities = 0;
                const QByteArray path = currentFilePath_.toUtf8();
                peare_writer_get_update_capabilities(path.constData(), 0, &capabilities);
                return fail(QStringLiteral("GUI Rename action is disabled: entry=%1 type=%2 writerPath=%3 capabilities=0x%4 selected=%5")
                    .arg(entryPath, selected ? selected->data(0, TypeRole).toString()
                                              : QStringLiteral("<none>"),
                         writerEntryPath(selected), QString::number(capabilities, 16),
                         QString::number(treeView_->selectedItems().size())));
            }
            renameAction_->trigger();
        } else if (kind == QStringLiteral("replace")) {
            offscreenWriterFileAnswers_ = QStringList{
                operation.value(QStringLiteral("source")).toString()};
            offscreenWriterFileIndex_ = 0;
            if (!replaceAction_ || !replaceAction_->isEnabled())
                return fail(QStringLiteral("GUI Replace action is disabled."));
            replaceAction_->trigger();
        } else if (kind == QStringLiteral("delete")) {
            if (!deleteAction_ || !deleteAction_->isEnabled())
                return fail(QStringLiteral("GUI Delete action is disabled."));
            deleteAction_->trigger();
        } else {
            return fail(QStringLiteral("Unknown GUI writer operation: %1").arg(kind));
        }
        QApplication::processEvents();
        if (!offscreenWriterError_.isEmpty()) return fail(offscreenWriterError_);

        const QString expectPresent = operation.value(QStringLiteral("expect_present")).toString();
        const QString expectAbsent = operation.value(QStringLiteral("expect_absent")).toString();
        if (!expectPresent.isEmpty() && !findDetailsPath(expectPresent))
            return fail(QStringLiteral("Expected GUI path is missing: %1").arg(expectPresent));
        if (!expectAbsent.isEmpty() && findDetailsPath(expectAbsent))
            return fail(QStringLiteral("Deleted GUI path is still present: %1").arg(expectAbsent));
        if (detailsMode_ && detailsPath(currentDetailsLocation_) !=
                normalizedViewPath(viewPath))
            return fail(QStringLiteral("Details view did not preserve location: %1")
                        .arg(viewPath));

        QJsonObject result;
        result.insert(QStringLiteral("type"), kind);
        result.insert(QStringLiteral("passed"), true);
        result.insert(QStringLiteral("view_path"), detailsPath(currentDetailsLocation_));
        result.insert(QStringLiteral("selection"), detailsPath(treeView_->currentItem()));
        results.push_back(result);
        writeProgress(QStringLiteral("operation-complete: ") + kind);
    }

    if (offscreenWriterTextIndex_ != offscreenWriterTextAnswers_.size() ||
        offscreenWriterFileIndex_ != offscreenWriterFileAnswers_.size())
        return fail(QStringLiteral("Scenario did not consume all dialog answers."));
    const QPixmap screenshot = grab();
    if (screenshot.isNull() || !screenshot.save(screenshotPath, "PNG"))
        return fail(QStringLiteral("Could not save GUI writer screenshot."));
    QJsonObject report;
    report.insert(QStringLiteral("image"), QFileInfo(imagePath).absoluteFilePath());
    report.insert(QStringLiteral("view"), scenario.value(QStringLiteral("view")));
    report.insert(QStringLiteral("stage"), QStringLiteral("complete"));
    report.insert(QStringLiteral("operations"), results);
    QFile reportFile(reportPath);
    if (!reportFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("Could not write GUI writer report."));
    reportFile.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    reportFile.close();
    offscreenWriterScenario_ = false;
    offscreenWriterProgress_ = {};
    if (error) error->clear();
    return true;
}

void MainWindow::openFolder() {
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open folder"), currentDirectory_.isEmpty()
            ? QDir::homePath() : currentDirectory_);
    if (!path.isEmpty()) openFolderPath(path);
}

void MainWindow::addFilesToNewContainer() {
    const QStringList sources = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Select files to add"), currentDirectory_.isEmpty()
            ? QDir::homePath() : currentDirectory_);
    if (sources.isEmpty()) return;

    QVector<peare_writer_format> formats;
    for (size_t i = 0; i < peare_writer_create_format_count(); ++i) {
        peare_writer_format format{};
        if (peare_writer_get_create_format(i, &format) != PEARE_WRITER_OK) continue;
        formats.push_back(format);
    }
    if (formats.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Add files to"),
                             QStringLiteral("No destination format is available."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add files to"));
    auto* formatSelector = new QComboBox(&dialog);
    auto* variantSelector = new QComboBox(&dialog);
    auto* filesystemSelector = new QComboBox(&dialog);
    auto* outputSelector = new QLineEdit(&dialog);
    auto* browseButton = new QPushButton(QStringLiteral("Browse..."), &dialog);
    auto* outputRow = new QWidget(&dialog);
    auto* outputLayout = new QHBoxLayout(outputRow);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->addWidget(outputSelector, 1);
    outputLayout->addWidget(browseButton);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("Format"), formatSelector);
    form->addRow(QStringLiteral("Variant"), variantSelector);
    form->addRow(QStringLiteral("Filesystem"), filesystemSelector);
    QWidget* filesystemLabel = form->labelForField(filesystemSelector);
    form->addRow(QStringLiteral("Destination"), outputRow);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save |
                                         QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("Add files"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);

    for (const peare_writer_format& format : formats)
        formatSelector->addItem(QString::fromUtf8(format.display_name),
                                QString::fromUtf8(format.id));
    auto updateVariants = [&]() {
        variantSelector->clear();
        filesystemSelector->clear();
        const QByteArray formatId = formatSelector->currentData().toString().toUtf8();
        for (size_t i = 0; i < peare_writer_create_variant_count(formatId.constData()); ++i) {
            peare_writer_variant variant{};
            if (peare_writer_get_create_variant(formatId.constData(), i, &variant) ==
                PEARE_WRITER_OK)
                variantSelector->addItem(QString::fromUtf8(variant.display_name),
                                         QString::fromUtf8(variant.id));
        }
        variantSelector->setEnabled(variantSelector->count() > 1);
        for (size_t i = 0; i < peare_writer_create_filesystem_count(formatId.constData()); ++i) {
            peare_writer_variant filesystem{};
            if (peare_writer_get_create_filesystem(formatId.constData(), i, &filesystem) ==
                PEARE_WRITER_OK)
                filesystemSelector->addItem(QString::fromUtf8(filesystem.display_name),
                                            QString::fromUtf8(filesystem.id));
        }
        const bool hasFilesystemChoice = filesystemSelector->count() > 0;
        filesystemSelector->setEnabled(hasFilesystemChoice);
        filesystemSelector->setVisible(hasFilesystemChoice);
        if (filesystemLabel) filesystemLabel->setVisible(hasFilesystemChoice);
    };
    auto updateSuggestedPath = [&]() {
        const int index = formatSelector->currentIndex();
        if (index < 0 || index >= formats.size()) return;
        const QString extension = QString::fromUtf8(formats.at(index).extension);
        const QString directory = currentDirectory_.isEmpty()
            ? QFileInfo(sources.first()).absolutePath() : currentDirectory_;
        outputSelector->setText(QDir(directory).filePath(
            QStringLiteral("Archive.") + extension));
    };
    connect(formatSelector,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), &dialog,
            [&](int) { updateVariants(); updateSuggestedPath(); });
    connect(browseButton, &QPushButton::clicked, &dialog, [&]() {
        const int index = formatSelector->currentIndex();
        if (index < 0 || index >= formats.size()) return;
        const peare_writer_format format = formats.at(index);
        const QString filter = QStringLiteral("%1 (*.%2)")
            .arg(QString::fromUtf8(format.display_name), QString::fromUtf8(format.extension));
        const QString path = QFileDialog::getSaveFileName(
            &dialog, QStringLiteral("Add files to"), outputSelector->text(), filter);
        if (!path.isEmpty()) outputSelector->setText(path);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    updateVariants();
    updateSuggestedPath();
    if (dialog.exec() != QDialog::Accepted) return;

    const int formatIndex = formatSelector->currentIndex();
    if (formatIndex < 0 || formatIndex >= formats.size()) return;
    const peare_writer_format format = formats.at(formatIndex);
    const QString extension = QString::fromUtf8(format.extension);
    QString outputPath = outputSelector->text().trimmed();
    if (outputPath.isEmpty()) return;
    if (QFileInfo(outputPath).suffix().compare(extension, Qt::CaseInsensitive) != 0) {
        const QFileInfo outputInfo(outputPath);
        outputPath = QDir(outputInfo.absolutePath()).filePath(
            outputInfo.completeBaseName() + QLatin1Char('.') + extension);
    }
    if (QFileInfo::exists(outputPath) &&
        QMessageBox::question(this, QStringLiteral("Replace destination"),
            QStringLiteral("The destination already exists. Replace it?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    std::vector<QByteArray> sourcePaths;
    std::vector<QByteArray> entryNames;
    std::vector<peare_writer_input_file> inputFiles;
    sourcePaths.reserve(size_t(sources.size()));
    entryNames.reserve(size_t(sources.size()));
    inputFiles.reserve(size_t(sources.size()));
    for (const QString& source : sources) {
        sourcePaths.push_back(source.toUtf8());
        entryNames.push_back(QFileInfo(source).fileName().toUtf8());
    }
    for (size_t i = 0; i < sourcePaths.size(); ++i)
        inputFiles.push_back({sourcePaths[i].constData(), entryNames[i].constData()});

    peare_writer_handle writer = nullptr;
    peare_writer_status status = peare_writer_create(&writer);
    const QByteArray formatUtf8(format.id);
    const QByteArray variantUtf8 = variantSelector->currentData().toString().toUtf8();
    const QByteArray filesystemUtf8 = filesystemSelector->isEnabled()
        ? filesystemSelector->currentData().toString().toUtf8() : QByteArray();
    const QByteArray outputUtf8 = outputPath.toUtf8();
    const peare_writer_create_options createOptions = {
        formatUtf8.constData(), variantUtf8.constData(),
        filesystemUtf8.isEmpty() ? nullptr : filesystemUtf8.constData()
    };
    if (status == PEARE_WRITER_OK)
        status = peare_writer_add_files_to_ex(
            writer, outputUtf8.constData(), &createOptions, 0,
            inputFiles.data(), inputFiles.size());
    QString error;
    if (status != PEARE_WRITER_OK) {
        const char* detail = writer ? peare_writer_last_error(writer) : nullptr;
        error = QString::fromUtf8(detail && *detail
            ? detail : peare_writer_status_message(status));
    }
    peare_writer_destroy(writer);
    if (status != PEARE_WRITER_OK) {
        QMessageBox::warning(this, QStringLiteral("Add files to"), error);
        return;
    }
    openFilePath(outputPath);
}

void MainWindow::addFilesToContainer(const QStringList& requestedSources,
                                    const QString& requestedDestination) {
    if (currentFilePath_.isEmpty() || folderMode_) return;
    const QStringList sources = requestedSources.isEmpty()
        ? QFileDialog::getOpenFileNames(
              this, QStringLiteral("Add files"), currentDirectory_.isEmpty()
                  ? QFileInfo(currentFilePath_).absolutePath() : currentDirectory_)
        : requestedSources;
    if (sources.isEmpty()) return;
    const QString destination = requestedSources.isEmpty()
        ? writerDestinationPath(treeView_->currentItem()) : requestedDestination;

    const QString containerPath = currentFilePath_;
    const QByteArray containerUtf8 = containerPath.toUtf8();
    const size_t targetCount = peare_writer_update_target_count(containerUtf8.constData());
    size_t targetIndex = 0;
    if (targetCount > 1) {
        if (offscreenWriterScenario_) {
            if (offscreenWriterTargetIndex_ < 0 ||
                size_t(offscreenWriterTargetIndex_) >= targetCount) {
                offscreenWriterError_ = QStringLiteral("Invalid writer target index.");
                return;
            }
            targetIndex = size_t(offscreenWriterTargetIndex_);
        } else {
        QStringList targetNames;
        for (size_t i = 0; i < targetCount; ++i) {
            char name[256] = {};
            const peare_writer_status targetStatus = peare_writer_get_update_target_name(
                containerUtf8.constData(), i, name, sizeof(name));
            if (targetStatus != PEARE_WRITER_OK) {
                QMessageBox::warning(this, QStringLiteral("Add files"),
                    QString::fromUtf8(peare_writer_status_message(targetStatus)));
                return;
            }
            targetNames.push_back(QString::fromUtf8(name));
        }
        bool accepted = false;
        const QString selected = QInputDialog::getItem(
            this, QStringLiteral("Select destination volume"),
            QStringLiteral("Choose a destination:"), targetNames, 0, false, &accepted);
        if (!accepted) return;
        targetIndex = size_t(targetNames.indexOf(selected));
        }
    }

    std::vector<QByteArray> sourcePaths;
    std::vector<QByteArray> entryNames;
    std::vector<peare_writer_input_file> inputFiles;
    sourcePaths.reserve(size_t(sources.size()));
    entryNames.reserve(size_t(sources.size()));
    inputFiles.reserve(size_t(sources.size()));
    for (const QString& source : sources) {
        sourcePaths.push_back(source.toUtf8());
        const QString baseName = QFileInfo(source).fileName();
        const QString entryName = destination.isEmpty() ? baseName
            : destination + QLatin1Char('/') + baseName;
        entryNames.push_back(entryName.toUtf8());
    }
    for (size_t i = 0; i < sourcePaths.size(); ++i)
        inputFiles.push_back({sourcePaths[i].constData(), entryNames[i].constData()});

    const WriterViewState viewState = captureWriterViewState();
    currentSession_.close();
    childSessions_.clear();
    directResourcePayloads_.clear();
    directResourceType_.clear();
    peare_writer_handle writer = nullptr;
    peare_writer_status status = peare_writer_create(&writer);
    if (status == PEARE_WRITER_OK)
        reportOffscreenWriterStage(QStringLiteral("writer-update-started"));
    if (status == PEARE_WRITER_OK)
        status = peare_writer_add_files_to_ex(
            writer, containerUtf8.constData(), nullptr, targetIndex,
            inputFiles.data(), inputFiles.size());
    if (status == PEARE_WRITER_OK)
        reportOffscreenWriterStage(QStringLiteral("writer-update-finished"));
    QString error;
    if (status != PEARE_WRITER_OK) {
        const char* detail = writer ? peare_writer_last_error(writer) : nullptr;
        error = QString::fromUtf8(detail && *detail
            ? detail : peare_writer_status_message(status));
    }
    peare_writer_destroy(writer);
    if (status != PEARE_WRITER_OK) {
        refreshWriterView(viewState);
        showWriterError(QStringLiteral("Add files"), error);
        return;
    }
    refreshWriterView(viewState);
}

void MainWindow::openFolderPath(const QString& path) {
    const QFileInfo info(path);
    if (!info.isDir()) return;
    folderMode_ = true;
    currentDirectory_ = info.absoluteFilePath();
    currentFilePath_ = currentDirectory_;
    currentSession_.close();
    childSessions_.clear();
    if (detailsIconTimer_) detailsIconTimer_->stop();
    if (treeIconTimer_) treeIconTimer_->stop();
    pendingEmbeddedIconItems_.clear();
    treeIconIndex_ = 0;
    currentDetailsLocation_ = nullptr;
    previousLocations_.clear();
    nextLocations_.clear();
    treeView_->clear();
    detailsView_->clear();
    clearContent();
    messageLabel_->clear();
    resourceMenu_->setEnabled(false);
    addFilesAction_->setEnabled(false);
    exportOriginalAction_->setEnabled(false);
    exportConvertedAction_->setEnabled(false);
    exportAllAction_->setEnabled(false);
    populateLocalDirectory(currentDirectory_, nullptr);
    if (detailsMode_) navigateDetailsTo(nullptr, false);
    updateNavigationUi();
    setWindowTitle(QStringLiteral("Folder: %1").arg(QDir::toNativeSeparators(currentDirectory_)));
    messageLabel_->setText(QStringLiteral("%1").arg(currentDirectory_));
    newFolderAction_->setEnabled(true);
    newFileAction_->setEnabled(true);
    newSymbolicLinkAction_->setEnabled(true);
    newJunctionAction_->setEnabled(true);
    renameAction_->setEnabled(false);
    deleteAction_->setEnabled(false);
    replaceAction_->setEnabled(false);

}

bool MainWindow::runOffscreenFolderOpenScenario(const QString& directoryPath,
                                                const QString& relativeFilePath,
                                                const QString& screenshotPath,
                                                QString* error) {
    auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QFileInfo directoryInfo(directoryPath);
    if (!directoryInfo.isDir() || relativeFilePath.trimmed().isEmpty())
        return fail(QStringLiteral("A folder and relative file path are required."));

    resize(1280, 800);
    show();
    openFolderPath(directoryInfo.absoluteFilePath());
    setDetailsMode(true);
    QApplication::processEvents();
    QTreeWidgetItem* item = findDetailsPath(relativeFilePath);
    if (!item || localDirectory(item))
        return fail(QStringLiteral("The requested file was not found in the opened folder."));
    const QString expectedPath = QFileInfo(item->data(0, LocalPathRole).toString()).absoluteFilePath();

    selectDetailsSource(item);
    QTreeWidgetItem* row = nullptr;
    for (int i = 0; i < detailsView_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* candidate = detailsView_->topLevelItem(i);
        if (detailsSourceItem(candidate) == item) {
            row = candidate;
            break;
        }
    }
    if (!row)
        return fail(QStringLiteral("The requested file is not visible in Details view."));

    emit detailsView_->itemActivated(row, 0);
    QApplication::processEvents();
    if (folderMode_ || currentFilePath_ != expectedPath || !currentSession_.isOpen())
        return fail(QStringLiteral("Activating the file did not open it through Peare."));

    const QPixmap screenshot = grab();
    if (screenshot.isNull() || !screenshot.save(screenshotPath, "PNG"))
        return fail(QStringLiteral("Could not save the folder-open screenshot."));
    return true;
}

void MainWindow::populateLocalDirectory(const QString& path, QTreeWidgetItem* parent) {
    QDir directory(path);
    if (!directory.exists()) return;
    const QFileInfoList entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& info : entries) {
        const QString absolute = info.absoluteFilePath();
        auto* item = parent ? new QTreeWidgetItem(parent, QStringList(info.fileName()))
                            : new QTreeWidgetItem(treeView_, QStringList(info.fileName()));
        item->setData(0, LocalPathRole, absolute);
        item->setData(0, NameRole, info.fileName());
        item->setData(0, SizeRole, QVariant::fromValue<qulonglong>(info.size()));
        item->setData(0, TypeRole, info.isSymLink()
            ? (info.isDir() ? QStringLiteral("LOCAL_DIRECTORY_LINK")
                            : QStringLiteral("LOCAL_FILE_LINK"))
            : (info.isDir() ? QStringLiteral("LOCAL_DIRECTORY")
                            : QStringLiteral("LOCAL_FILE")));
        item->setData(0, DirectoryLoadedRole, false);
        if (info.isDir()) {
            QString canonical = info.canonicalFilePath();
            bool cycle = false;
            for (QTreeWidgetItem* ancestor = parent; ancestor; ancestor = ancestor->parent()) {
                const QString ancestorPath = ancestor->data(0, LocalPathRole).toString();
                if (!canonical.isEmpty() &&
                    QFileInfo(ancestorPath).canonicalFilePath().compare(canonical,
                        Qt::CaseInsensitive) == 0) {
                    cycle = true;
                    break;
                }
            }
            if (!parent && !canonical.isEmpty() &&
                QFileInfo(currentDirectory_).canonicalFilePath().compare(canonical,
                    Qt::CaseInsensitive) == 0)
                cycle = true;
            item->setIcon(0, settingsIcons_.folderCloseIcon());
            if (!cycle) {
                item->setData(0, PendingRole, true);
                new QTreeWidgetItem(item, QStringList(QStringLiteral("...")));
            }
        } else {
            item->setIcon(0, settingsIcons_.iconForFileName(info.fileName()));
        }
    }
    if (parent) parent->setData(0, DirectoryLoadedRole, true);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (!url.isLocalFile()) continue;
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent* event) {
    QStringList sources;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (QFileInfo(path).isFile()) sources.push_back(path);
    }
    if (sources.isEmpty()) {
        event->ignore();
        return;
    }

    event->setDropAction(Qt::CopyAction);
    event->accept();
    if (folderMode_) {
        const QPoint globalPosition = mapToGlobal(event->pos());
        QTreeWidgetItem* targetItem = nullptr;
        if (detailsMode_) {
            const QPoint detailsPosition = detailsView_->viewport()->mapFromGlobal(globalPosition);
            if (detailsView_->viewport()->rect().contains(detailsPosition))
                targetItem = detailsSourceItem(detailsView_->itemAt(detailsPosition));
        } else {
            const QPoint treePosition = treeView_->viewport()->mapFromGlobal(globalPosition);
            if (treeView_->viewport()->rect().contains(treePosition))
                targetItem = treeView_->itemAt(treePosition);
        }
        if (!targetItem) {
            targetItem = detailsMode_ ? currentDetailsLocation_ : treeView_->currentItem();
        }

        const QString destination = localMutationDirectory(targetItem);
        QStringList failures;
        int copied = 0;
        for (const QString& source : sources) {
            const QString target = QDir(destination).filePath(QFileInfo(source).fileName());
            if (QFileInfo::exists(target) || QFileInfo(target).isSymLink() ||
                !QFile::copy(source, target)) {
                failures.push_back(QFileInfo(source).fileName());
            } else {
                ++copied;
            }
        }

        QTreeWidgetItem* refreshItem = targetItem && localDirectory(targetItem)
            ? targetItem : (targetItem ? targetItem->parent() : nullptr);
        refreshLocalDirectory(refreshItem);
        if (!failures.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Copy files"),
                QStringLiteral("Could not copy: %1").arg(failures.join(QStringLiteral(", "))));
        } else {
            messageLabel_->setText(QStringLiteral("Copied %1 file(s) to %2")
                .arg(copied).arg(destination));
        }
        return;
    }

    const QPoint globalPosition = mapToGlobal(event->pos());
    QTreeWidgetItem* targetItem = nullptr;
    if (detailsMode_) {
        const QPoint detailsPosition = detailsView_->viewport()->mapFromGlobal(globalPosition);
        if (detailsView_->viewport()->rect().contains(detailsPosition))
            targetItem = detailsSourceItem(detailsView_->itemAt(detailsPosition));
    } else {
        const QPoint treePosition = treeView_->viewport()->mapFromGlobal(globalPosition);
        if (treeView_->viewport()->rect().contains(treePosition))
            targetItem = treeView_->itemAt(treePosition);
    }
    if (!targetItem) targetItem = treeView_->currentItem();
    const QString destination = writerDestinationPath(targetItem);
    if (addFilesAction_->isEnabled()) {
        addFilesToContainer(sources, destination);
    } else {
        openFilePath(sources.first());
    }
}

bool MainWindow::showImageFile(const QString& path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead()) return false;
    const QSize size = reader.size();
    const QImage image = reader.read();
    if (image.isNull()) return false;
    clearContent();
    currentPreview_ = {};
    currentPreview_.images.push_back(image);
    auto* layout = qobject_cast<QVBoxLayout*>(contentWidget_->layout());
    auto* info = new QLabel(QStringLiteral("%1 x %2").arg(size.width()).arg(size.height()),
                            contentWidget_);
    layout->addWidget(info);
    auto* picture = new QLabel(contentWidget_);
    picture->setPixmap(QPixmap::fromImage(image));
    picture->setFixedSize(image.size());
    picture->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(picture);
    contentArea_->setWidgetResizable(false);
    contentArea_->setWidget(contentWidget_);
    messageLabel_->setText(QStringLiteral("Image preview / %1").arg(QFileInfo(path).fileName()));
    setWindowTitle(QStringLiteral("Image preview: %1").arg(QFileInfo(path).fileName()));
    resourceMenu_->setEnabled(false);
    exportOriginalAction_->setEnabled(false);
    exportConvertedAction_->setEnabled(false);
    exportAllAction_->setEnabled(false);
    return true;
}

void MainWindow::showLocalFile(const QString& path) {
    if (showImageFile(path)) return;
    clearContent();
    const QFileInfo info(path);
    auto* layout = qobject_cast<QVBoxLayout*>(contentWidget_->layout());
    QString text = QStringLiteral("<b>%1</b><br><br>Size: %2 bytes<br>Modified: %3")
        .arg(info.fileName().toHtmlEscaped(), QString::number(info.size()),
             QLocale().toString(info.lastModified(), QLocale::ShortFormat));
    if (info.isSymLink()) text += QStringLiteral("<br>Target: %1").arg(info.symLinkTarget().toHtmlEscaped());
    auto* details = new QLabel(text, contentWidget_);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(details);
    if (info.size() > qint64(kMaxHexPreviewBytes)) {
        auto* warning = new QLabel(QStringLiteral("Hex preview disabled for files larger than 1 MiB."),
                                   contentWidget_);
        layout->addWidget(warning);
    } else {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.read(kMaxHexPreviewBytes + 1);
            if (!bytes.contains('\0')) {
                auto* textView = new QPlainTextEdit(contentWidget_);
                textView->setReadOnly(true);
                textView->setPlainText(QString::fromUtf8(bytes));
                layout->addWidget(textView, 1);
            } else {
                auto* dump = new QPlainTextEdit(contentWidget_);
                dump->setReadOnly(true);
                dump->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
                dump->setPlainText(formatRawDump(bytes));
                layout->addWidget(dump, 1);
            }
        }
    }
    messageLabel_->setText(QStringLiteral("File / %1").arg(info.fileName()));
    resourceMenu_->setEnabled(false);
    exportOriginalAction_->setEnabled(false);
    exportConvertedAction_->setEnabled(false);
}

void MainWindow::showModuleSummary(const QString& filePath,
                                   const QString& initialPassword) {
    folderMode_ = false;
    directResourcePayloads_.clear();
    directResourceType_.clear();
    newFolderAction_->setEnabled(false);
    newFileAction_->setEnabled(false);
    newSymbolicLinkAction_->setEnabled(false);
    newJunctionAction_->setEnabled(false);
    renameAction_->setEnabled(false);
    deleteAction_->setEnabled(false);
    replaceAction_->setEnabled(false);
    QString error;
    const QFileInfo fi(filePath);
    const QString sourceName = fi.fileName().isEmpty() ? filePath : fi.fileName();
    // Reset all previous state (tree + any nested child sessions) before the new
    // file is opened, so nothing from a prior document can survive.
    currentDetailsLocation_ = nullptr;
    previousLocations_.clear();
    nextLocations_.clear();
    if (detailsIconTimer_) detailsIconTimer_->stop();
    if (treeIconTimer_) treeIconTimer_->stop();
    pendingEmbeddedIconItems_.clear();
    treeIconIndex_ = 0;
    if (detailsView_) detailsView_->clear();
    treeView_->clear();
    childSessions_.clear();
    currentPreview_ = {};
    clearContent();
    messageLabel_->clear();
    resourceMenu_->setEnabled(false);
    exportOriginalAction_->setEnabled(false);
    exportConvertedAction_->setEnabled(false);
    exportAllAction_->setEnabled(false);

    QString password = initialPassword;
    for (;;) {
        if (currentSession_.openFile(filePath, &error, password)) break;
        const peare_status status = currentSession_.lastStatus();
        if (status != PEARE_STATUS_PASSWORD_REQUIRED &&
            status != PEARE_STATUS_PASSWORD_INVALID) break;
        bool accepted = false;
        const QString prompt = status == PEARE_STATUS_PASSWORD_REQUIRED
            ? QStringLiteral("Enter the password for %1").arg(sourceName)
            : QStringLiteral("The password is incorrect or the archive is damaged. Try again for %1")
                  .arg(sourceName);
        password = QInputDialog::getText(this, QStringLiteral("Encrypted archive"), prompt,
                                         QLineEdit::Password, QString(), &accepted);
        if (!accepted) {
            setWindowTitle(QStringLiteral("Peare"));
            return;
        }
        if (password.isNull()) password = QStringLiteral("");
    }

    if (!currentSession_.isOpen() && fi.size() > 0 &&
        quint64(fi.size()) <= kMaxDecodePreviewBytes) {
        QFile directFile(filePath);
        if (directFile.open(QIODevice::ReadOnly)) {
            const QByteArray payload = directFile.readAll();
            pearegui::ResourceContext context;
            context.sourceName = sourceName;
            context.dataSize = quint64(payload.size());
            pearegui::Resource candidate(payload, context);
            const QVector<QByteArray> faces = pearegui::fontCollectionFaces(candidate);
            if (faces.size() > 1) {
                currentSession_.close();
                directResourcePayloads_ = faces;
                directResourceType_ = QStringLiteral("RT_FONT");
                auto* collection = new QTreeWidgetItem(treeView_);
                collection->setText(0, sourceName);
                collection->setData(0, DirectCollectionRole, true);
                for (int i = 0; i < faces.size(); ++i) {
                    auto* face = new QTreeWidgetItem(collection);
                    face->setText(0, QStringLiteral("Face %1").arg(i + 1));
                    face->setData(0, TypeRole, QStringLiteral("RT_FONT"));
                    face->setData(0, NameRole, QStringLiteral("Face %1").arg(i + 1));
                    face->setData(0, DirectResourceRole, true);
                    face->setData(0, DirectPayloadIndexRole, i);
                }
                collection->setExpanded(true);
                treeView_->setCurrentItem(collection->child(0));
                setWindowTitle(QStringLiteral("Decoded content: ") + sourceName);
                messageLabel_->setText(QStringLiteral("%1 font faces").arg(faces.size()));
                resourceMenu_->setEnabled(false);
                exportOriginalAction_->setEnabled(false);
                exportConvertedAction_->setEnabled(false);
                exportAllAction_->setEnabled(false);
                addFilesAction_->setEnabled(false);
                return;
            }
            quint64 glyphCount = 0;
            const bool isFont =
                pearegui::getUnsigned(candidate, PEARE_INFO_FONT_GLYPH_COUNT, &glyphCount,
                                      QStringLiteral("RT_FONT")) && glyphCount > 0;
            if (isFont)
                context.type = QStringLiteral("RT_FONT");
            const pearegui::Preview directPreview = pearegui::decode(
                pearegui::Resource(payload, context));
            if (isFont || !directPreview.images.isEmpty() || !directPreview.text.isEmpty()) {
                currentSession_.close();
                directResourcePayloads_ = {payload};
                directResourceType_ = isFont ? QStringLiteral("RT_FONT") : QString();
                auto* item = new QTreeWidgetItem(treeView_);
                item->setText(0, sourceName);
                item->setData(0, TypeRole, directResourceType_.isEmpty()
                    ? QStringLiteral("DECODED_RESOURCE") : directResourceType_);
                item->setData(0, NameRole, sourceName);
                item->setData(0, DirectResourceRole, true);
                treeView_->setCurrentItem(item);
                setWindowTitle(QStringLiteral("Decoded content: ") + sourceName);
                messageLabel_->setText(QStringLiteral("1 resource found"));
                resourceMenu_->setEnabled(false);
                exportOriginalAction_->setEnabled(false);
                exportConvertedAction_->setEnabled(false);
                exportAllAction_->setEnabled(false);
                addFilesAction_->setEnabled(false);
                auto* summary = new QLabel(
                    QStringLiteral("<b>Decoded content</b><br><br>File: %1<br>Size: %2 bytes")
                        .arg(fi.absoluteFilePath().toHtmlEscaped(), QString::number(fi.size())),
                    contentWidget_);
                summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
                qobject_cast<QVBoxLayout*>(contentWidget_->layout())->addWidget(summary);
                return;
            }
        }
    }

    if (!currentSession_.isOpen()) {
        if (showImageFile(filePath)) return;
        setWindowTitle(QStringLiteral("Peare"));
        QMessageBox::information(this, QStringLiteral("Info file"),
                                 QStringLiteral("Cannot open file: %1\n%2").arg(sourceName, error));
        return;
    }

    const peare_container_format format = currentSession_.containerFormat();
    const QByteArray filePathUtf8 = filePath.toUtf8();
    const bool writerCheckApplicable = format != PEARE_CONTAINER_UFS;
    const bool canAddFiles = writerCheckApplicable &&
        peare_writer_can_add_files(filePathUtf8.constData()) != 0;
    peare_writer_update_capabilities writerCapabilities = 0;
    if (writerCheckApplicable)
        peare_writer_get_update_capabilities(filePathUtf8.constData(), 0,
                                              &writerCapabilities);
    size_t totalResources = 0;
    for (size_t f = 0; f < currentSession_.folderCount(); ++f)
        totalResources += currentSession_.resourceCount(f);
    treeView_->setUpdatesEnabled(false);
    populateFromSession(&currentSession_, nullptr);

    // Keep directory and grouping icons separate from file-backed containers.
    std::function<void(QTreeWidgetItem*)> normalizeExpandableIcons = [&](QTreeWidgetItem* item) {
        if (!item) return;
        if (itemIsDirectoryLike(item))
            item->setIcon(0, settingsIcons_.folderCloseIcon());
        for (int i = 0; i < item->childCount(); ++i) normalizeExpandableIcons(item->child(i));
    };
    for (int i = 0; i < treeView_->topLevelItemCount(); ++i)
        normalizeExpandableIcons(treeView_->topLevelItem(i));
    treeView_->setUpdatesEnabled(true);
    scheduleTreeIconLoading();
    if (detailsMode_)
        navigateDetailsTo(nullptr, false);
    else
        updateNavigationUi();

    const QString formatName = pearegui::containerName(format);
    const QString description = currentSession_.description();
    setWindowTitle(formatName + QStringLiteral(" file open: ") + sourceName);
    auto* layout = qobject_cast<QVBoxLayout*>(contentWidget_->layout());
    auto* summary = new QLabel(contentWidget_);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    QString summaryHtml =
        QStringLiteral("<b>%1</b><br><br>File: %2<br>Size: %3 bytes")
            .arg(formatName.toHtmlEscaped(), fi.absoluteFilePath().toHtmlEscaped(),
                 QString::number(fi.size()));
    if (!description.isEmpty())
        summaryHtml += QStringLiteral("<br>Description: %1").arg(description.toHtmlEscaped());
    summaryHtml +=
        QStringLiteral("<br>Resource groups: %1<br>Resource entries: %2")
            .arg(QString::number(currentSession_.folderCount()), QString::number(totalResources));
    summary->setText(summaryHtml);
    layout->addWidget(summary);
    messageLabel_->setText(totalResources == 0
                               ? QStringLiteral("No resources found")
                               : QStringLiteral("%1 resources found").arg(totalResources));
    addFilesAction_->setEnabled(canAddFiles);
    resourceMenu_->setEnabled(totalResources > 0 || canAddFiles);
    exportOriginalAction_->setEnabled(false);
    exportConvertedAction_->setEnabled(false);
    exportAllAction_->setEnabled(totalResources > 0);
    newFolderAction_->setEnabled((writerCapabilities & PEARE_WRITER_CAP_CREATE_DIRECTORY) != 0);
    newFileAction_->setEnabled((writerCapabilities & PEARE_WRITER_CAP_CREATE_EMPTY_FILE) != 0);
    newSymbolicLinkAction_->setEnabled(false);
    newJunctionAction_->setEnabled(false);
    renameAction_->setEnabled(false);
    deleteAction_->setEnabled(false);
    replaceAction_->setEnabled(false);

    QTreeWidgetItem* onlyResource = nullptr;
    if (totalResources == 1) {
        QVector<QTreeWidgetItem*> pending;
        for (int i = 0; i < treeView_->topLevelItemCount(); ++i)
            pending.push_back(treeView_->topLevelItem(i));
        while (!pending.isEmpty()) {
            QTreeWidgetItem* item = pending.takeLast();
            if (item->data(0, TypeRole).isValid() && item->childCount() == 0) {
                if (onlyResource) {
                    onlyResource = nullptr;
                    break;
                }
                onlyResource = item;
            }
            for (int i = 0; i < item->childCount(); ++i)
                pending.push_back(item->child(i));
        }
    }
    if (onlyResource) {
        pearegui::Session* owner = sessionOf(onlyResource);
        pearegui::Resource resource = owner->openResource(
            onlyResource->data(0, FolderIndexRole).toULongLong(),
            onlyResource->data(0, ResourceIndexRole).toULongLong());
        quint64 glyphCount = 0;
        const QString resourceType = onlyResource->data(0, TypeRole).toString();
        const bool isFont = pearegui::getUnsigned(resource, PEARE_INFO_FONT_GLYPH_COUNT,
                                                  &glyphCount, resourceType) ||
                            pearegui::getUnsigned(resource, PEARE_INFO_FONT_GLYPH_COUNT,
                                                  &glyphCount, {});
        if (isFont && glyphCount > 0) {
            for (QTreeWidgetItem* parent = onlyResource->parent(); parent;
                 parent = parent->parent())
                parent->setExpanded(true);
            treeView_->setCurrentItem(onlyResource);
            onlyResource->setSelected(true);
        }
    }
}

static QImage cropVisibleGlyph(const QImage& source, bool* hasInk = nullptr) {
    if (hasInk) *hasInk = false;
    if (source.isNull()) return {};
    int left = source.width(), top = source.height(), right = -1, bottom = -1;
    for (int y = 0; y < source.height(); ++y)
        for (int x = 0; x < source.width(); ++x) {
            if (qAlpha(source.pixel(x, y)) == 0) continue;
            left = qMin(left, x);
            top = qMin(top, y);
            right = qMax(right, x);
            bottom = qMax(bottom, y);
        }
    if (hasInk) *hasInk = right >= left && bottom >= top;
    return right >= left && bottom >= top
               ? source.copy(left, top, right - left + 1, bottom - top + 1)
               : source;
}

QImage MainWindow::buildFontGlyphMap(const pearegui::Resource& resource,
                                     const QString& resourceType,
                                     const QVector<quint32>& mapValues,
                                     bool usesGlyphIndexes, int pageIndex, int* cellCount,
                                     int* renderedGlyphCount, int* inkedGlyphCount,
                                     int* columnCount,
                                     int* imageWidth, int* imageHeight,
                                     int* viewportWidthOut, int* viewportHeightOut) const {
    if (cellCount) *cellCount = 0;
    if (renderedGlyphCount) *renderedGlyphCount = 0;
    if (inkedGlyphCount) *inkedGlyphCount = 0;
    if (columnCount) *columnCount = 0;
    if (imageWidth) *imageWidth = 0;
    if (imageHeight) *imageHeight = 0;
    if (viewportWidthOut) *viewportWidthOut = 0;
    if (viewportHeightOut) *viewportHeightOut = 0;
    if (pageIndex < 0 || mapValues.isEmpty()) return {};
    const int offset = pageIndex * kFontGlyphMapPageSize;
    if (offset < 0 || offset >= mapValues.size()) return {};
    const QVector<quint32> pageValues = mapValues.mid(
        offset, qMin(kFontGlyphMapPageSize, mapValues.size() - offset));
    if (cellCount) *cellCount = pageValues.size();
    const int scale = mapValues.size() <= 32 ? 2 : 1;
    QVector<QImage> glyphs;
    glyphs.reserve(pageValues.size());
    QVector<QImage> renderedGlyphs;
    if (usesGlyphIndexes) {
        renderedGlyphs = pearegui::renderFontGlyphs(
            resource, pageValues, scale, 0, 0x000000FFu, 0xFFFFFF00u, resourceType);
        if (renderedGlyphs.size() != pageValues.size()) {
            renderedGlyphs.clear();
            renderedGlyphs.reserve(pageValues.size());
            for (quint32 index : pageValues) {
                QImage image = pearegui::renderFontGlyph(
                    resource, index, scale, 0, 0x000000FFu, 0xFFFFFF00u, resourceType);
                if (image.isNull()) {
                    image = QImage(1, 1, QImage::Format_ARGB32);
                    image.fill(0xFFFFFF00u);
                }
                renderedGlyphs.push_back(std::move(image));
            }
        }
    }
    // Size the glyph-map cells from the actual painted glyph bounds.  Vector
    // font pixelHeight is an em/metric value and can be several times larger
    // than the visible outline, so it must not define the cell canvas.
    int imageAreaWidth = 1;
    int imageAreaHeight = 1;
    int successfulRenders = 0;
    int inkedGlyphs = 0;
    for (int i = 0; i < pageValues.size(); ++i) {
        const quint32 value = pageValues.at(i);
        QImage glyph = usesGlyphIndexes ? renderedGlyphs.at(i)
            : pearegui::renderFont(resource, QString::fromUcs4(&value, 1), scale, 0,
                                   0x000000FFu, 0xFFFFFF00u, resourceType);
        bool hasInk = false;
        const QImage visibleGlyph = cropVisibleGlyph(glyph, &hasInk);
        if (usesGlyphIndexes) glyph = visibleGlyph;
        if (!glyph.isNull()) {
            ++successfulRenders;
            if (hasInk) ++inkedGlyphs;
        }
        glyphs.push_back(glyph);
        if (!glyph.isNull()) {
            imageAreaWidth = qMin(96, qMax(imageAreaWidth, glyph.width()));
            imageAreaHeight = qMin(96, qMax(imageAreaHeight, glyph.height()));
        }
    }
    if (renderedGlyphCount) *renderedGlyphCount = successfulRenders;
    if (inkedGlyphCount) *inkedGlyphCount = inkedGlyphs;
    if (successfulRenders == 0) {
        if (cellCount) *cellCount = 0;
        return {};
    }
    imageAreaWidth = qMax(4, imageAreaWidth);
    imageAreaHeight = qMax(4, imageAreaHeight);
    const int cellWidth = imageAreaWidth + 12, cellHeight = imageAreaHeight + 18;
    const int viewportWidth = contentArea_ ? contentArea_->viewport()->width() : 0;
    const int viewportHeight = contentArea_ ? contentArea_->viewport()->height() : 0;
    if (viewportWidthOut) *viewportWidthOut = viewportWidth;
    if (viewportHeightOut) *viewportHeightOut = viewportHeight;
    // Keep the grid within the viewport width so every glyph column remains visible.
    const int availableWidth = qMax(1, viewportWidth - 1);
    const int columns = qBound(1, viewportWidth > 0 ? availableWidth / cellWidth : 1, 64);
    if (columnCount) *columnCount = columns;
    const int rows = (glyphs.size() + columns - 1) / columns;
    QImage result(columns * cellWidth + 1, rows * cellHeight + 1, QImage::Format_ARGB32);
    if (imageWidth) *imageWidth = result.width();
    if (imageHeight) *imageHeight = result.height();
    result.fill(Qt::white);
    QPainter painter(&result);
    QFont codeFont(QStringLiteral("Monospace"), 7);
    codeFont.setStyleHint(QFont::TypeWriter);
    for (int i = 0; i < glyphs.size(); ++i) {
        const int x = (i % columns) * cellWidth, y = (i / columns) * cellHeight;
        painter.setPen(QColor(Qt::lightGray));
        painter.drawRect(x, y, cellWidth, cellHeight);
        QImage glyph = glyphs.at(i);
        if (!glyph.isNull()) {
            if (glyph.width() > imageAreaWidth || glyph.height() > imageAreaHeight)
                glyph = glyph.scaled(imageAreaWidth, imageAreaHeight, Qt::KeepAspectRatio,
                                     Qt::FastTransformation);
            painter.drawImage(x + 6 + qMax(0, (imageAreaWidth - glyph.width()) / 2),
                              y + 2 + qMax(0, (imageAreaHeight - glyph.height()) / 2), glyph);
        }
        painter.setFont(codeFont);
        painter.setPen(QColor(Qt::darkGray));
        const quint32 code = pageValues.at(i);
        const QString label = usesGlyphIndexes
            ? QStringLiteral("GID %1").arg(code, 4, 16, QLatin1Char('0')).toUpper()
            : QString::number(code, 16).toUpper().rightJustified(code <= 0xFF ? 2 : 4,
                                                                  QLatin1Char('0'));
        painter.drawText(x + 2, y + imageAreaHeight + 3, cellWidth - 4, 14,
                         Qt::AlignLeft | Qt::AlignVCenter, label);
    }
    return result;
}

void MainWindow::showFontGlyphMapPage() {
    if (!fontGlyphMapImageLabel_ || fontGlyphMapValues_.isEmpty()) return;
    pearegui::Resource resource(fontGlyphMapPayload_, fontGlyphMapContext_);
    int cellCount = 0;
    int renderedCount = 0;
    const QImage map = buildFontGlyphMap(resource, fontGlyphMapResourceType_,
                                         fontGlyphMapValues_, fontGlyphMapUsesGlyphIndexes_,
                                         fontGlyphMapPage_, &cellCount, &renderedCount);
    if (map.isNull()) return;
    fontGlyphMapImageLabel_->setPixmap(QPixmap::fromImage(map));
    fontGlyphMapImageLabel_->setFixedSize(map.size());
    fontGlyphMapImageLabel_->setProperty("previewGridCellCount", cellCount);
    fontGlyphMapImageLabel_->setProperty("previewGridRenderedCount", renderedCount);
    const int pageCount = (fontGlyphMapValues_.size() + kFontGlyphMapPageSize - 1) /
                          kFontGlyphMapPageSize;
    const int firstItem = fontGlyphMapPage_ * kFontGlyphMapPageSize + 1;
    const int lastItem = qMin(fontGlyphMapValues_.size(),
                              (fontGlyphMapPage_ + 1) * kFontGlyphMapPageSize);
    fontGlyphMapLabel_->setText(
        QStringLiteral("Glyph map (%1 items; page %2 of %3, %4-%5)")
            .arg(fontGlyphMapValues_.size()).arg(fontGlyphMapPage_ + 1)
            .arg(pageCount).arg(firstItem).arg(lastItem));
    fontGlyphMapLabel_->setProperty("previewGridExpectedItems", fontGlyphMapValues_.size());
    fontGlyphMapLabel_->setProperty("previewGridCellCount", cellCount);
    fontGlyphMapLabel_->setProperty("previewGridRenderedCount", renderedCount);
    fontGlyphMapLabel_->setProperty("previewGridPageIndex", fontGlyphMapPage_);
    fontGlyphMapLabel_->setProperty("previewGridPageCount", pageCount);
    fontGlyphMapLabel_->setProperty("previewGridPageSize", kFontGlyphMapPageSize);
    fontGlyphMapPreviousButton_->setEnabled(fontGlyphMapPage_ > 0);
    fontGlyphMapNextButton_->setEnabled(fontGlyphMapPage_ + 1 < pageCount);
}

pearegui::Session* MainWindow::sessionOf(QTreeWidgetItem* item) const {
    for (; item; item = item->parent()) {
        const QVariant v = item->data(0, SessionRole);
        if (v.isValid())
            return reinterpret_cast<pearegui::Session*>(static_cast<quintptr>(v.toULongLong()));
    }
    return const_cast<pearegui::Session*>(&currentSession_);
}

void MainWindow::populateFromSession(pearegui::Session* session, QTreeWidgetItem* parentBase) {
    const quintptr sid = reinterpret_cast<quintptr>(session);
    QHash<QString, QTreeWidgetItem*> hierarchyItems;
    auto ensurePath = [&](const QStringList& path) -> QTreeWidgetItem* {
        QTreeWidgetItem* parent = parentBase;
        QString key;
        for (const QString& segment : path) {
            if (!key.isEmpty()) key += QLatin1Char('/');
            key += segment;
            auto* existing = hierarchyItems.value(key, nullptr);
            if (!existing) {
                const int siblingCount = parent ? parent->childCount()
                                                 : treeView_->topLevelItemCount();
                for (int i = 0; i < siblingCount; ++i) {
                    QTreeWidgetItem* sibling = parent ? parent->child(i)
                                                       : treeView_->topLevelItem(i);
                    if (sibling->text(0) == segment &&
                        sibling->data(0, NameRole).toString() == segment &&
                        (!sibling->data(0, TypeRole).isValid() ||
                         isDirectoryEntryType(sibling->data(0, TypeRole).toString()) ||
                         sibling->data(0, TypeRole).toString() == QStringLiteral("PE_SECTION"))) {
                        existing = sibling;
                        break;
                    }
                }
                if (!existing) {
                    existing = parent ? new QTreeWidgetItem(parent, QStringList(segment))
                                      : new QTreeWidgetItem(treeView_, QStringList(segment));
                    existing->setIcon(0, settingsIcons_.folderCloseIcon());
                    existing->setData(0, NameRole, segment);
                    existing->setData(0, SessionRole, QVariant::fromValue<qulonglong>(sid));
                }
                hierarchyItems.insert(key, existing);
            }
            parent = existing;
        }
        return parent ? parent : parentBase;
    };
    const size_t folders = session->folderCount();
    for (size_t folderIndex = 0; folderIndex < folders; ++folderIndex) {
        const QString folderType = session->folderType(folderIndex);
        const size_t resources = session->resourceCount(folderIndex);
        for (size_t resourceIndex = 0; resourceIndex < resources; ++resourceIndex) {
            pearegui::Resource resource = session->openResource(folderIndex, resourceIndex);
            if (!resource.isValid()) continue;
            const pearegui::ResourceContext context = resource.context();
            QString label = context.identifier;
            if (context.type == QStringLiteral("PE_HEADERS"))
                label = QStringLiteral("PE headers");
            QStringList hierarchyPath = folderType.split(QChar(0x1f), Qt::SkipEmptyParts);
            const bool resourceAtPathLeaf = !hierarchyPath.isEmpty() &&
                hierarchyPath.last() == context.language &&
                !(context.containerFormat == PEARE_CONTAINER_PE &&
                  hierarchyPath.first() == QStringLiteral(".rsrc"));
            if (!resourceAtPathLeaf && hasVisibleLanguage(context.language))
                label += QStringLiteral(" [%1]").arg(context.language);
            auto* typeItem = ensurePath(hierarchyPath);
            const bool directoryLike = isDirectoryEntryType(context.type);
            QTreeWidgetItem* child = nullptr;
            if (resourceAtPathLeaf && !typeItem->data(0, TypeRole).isValid())
                child = typeItem;
            else if (directoryLike) {
                QTreeWidgetItem* directoryParent = typeItem ? typeItem : parentBase;
                const int siblingCount = directoryParent
                    ? directoryParent->childCount() : treeView_->topLevelItemCount();
                for (int i = 0; i < siblingCount; ++i) {
                    QTreeWidgetItem* candidate = directoryParent
                        ? directoryParent->child(i) : treeView_->topLevelItem(i);
                    if (!candidate->data(0, TypeRole).isValid() &&
                        candidate->data(0, NameRole).toString() == context.identifier) {
                        child = candidate;
                        break;
                    }
                }
                if (!child) {
                    child = directoryParent
                        ? new QTreeWidgetItem(directoryParent, QStringList(label))
                        : new QTreeWidgetItem(treeView_, QStringList(label));
                }
            }
            else if (typeItem)
                child = new QTreeWidgetItem(typeItem, QStringList(label));
            else
                child = new QTreeWidgetItem(treeView_, QStringList(label));
            const bool namedWithExtension = hasFileExtension(context.identifier);
            const bool fileLike = isFileEntryType(context.type);
            if (directoryLike || (context.isContainer && !fileLike && !namedWithExtension))
                child->setIcon(0, settingsIcons_.folderCloseIcon());
            else if (fileLike || namedWithExtension)
                child->setIcon(0, settingsIcons_.iconForFileName(context.identifier));
            else
                child->setIcon(0, settingsIcons_.resourceIcon(context.type));
            child->setData(0, TypeRole, context.type);
            child->setData(0, NameRole, context.identifier);
            child->setData(0, LanguageRole, context.language);
            child->setData(0, OffsetRole, QVariant::fromValue<qulonglong>(context.dataOffset));
            child->setData(0, SizeRole, QVariant::fromValue<qulonglong>(context.dataSize));
            child->setData(0, CodePageRole, context.codepage);
            child->setData(0, FolderIndexRole, QVariant::fromValue<qulonglong>(folderIndex));
            child->setData(0, ResourceIndexRole, QVariant::fromValue<qulonglong>(resourceIndex));
            child->setData(0, BaseIdRole, QVariant::fromValue<qlonglong>(context.baseId));
            child->setData(0, SessionRole, QVariant::fromValue<qulonglong>(sid));
            if (context.isContainer) {
                // Nested containers expand on demand; file-like entries keep file icons.
                child->setData(0, PendingRole, true);
                if (!fileLike && !namedWithExtension)
                    child->setIcon(0, settingsIcons_.folderCloseIcon());
                auto* placeholder = new QTreeWidgetItem(child, QStringList(QStringLiteral("...")));
                placeholder->setData(0, PlaceholderRole, true);
            }
            if (hasEmbeddedExecutableIcon(context.identifier) &&
                !child->data(0, EmbeddedIconQueuedRole).toBool()) {
                child->setData(0, EmbeddedIconQueuedRole, true);
                pendingEmbeddedIconItems_.push_back(child);
            }
        }
    }
}

void MainWindow::expandContainerItem(QTreeWidgetItem* item) {
    if (!item || !item->data(0, PendingRole).toBool()) return;
    if (itemIsDirectoryLike(item)) {
        for (int i = 0; i < item->childCount(); ++i) {
            if (item->child(i)->data(0, PlaceholderRole).toBool()) continue;
            item->setData(0, PendingRole, false);
            for (int childIndex = item->childCount() - 1; childIndex >= 0; --childIndex) {
                if (item->child(childIndex)->data(0, PlaceholderRole).toBool())
                    delete item->takeChild(childIndex);
            }
            return;
        }
    }
    item->setData(0, PendingRole, false); // handled (also prevents recursion)
    qDeleteAll(item->takeChildren());     // drop the placeholder

    if (folderMode_ && item->data(0, LocalPathRole).isValid()) {
        populateLocalDirectory(item->data(0, LocalPathRole).toString(), item);
        if (detailsMode_ && currentDetailsLocation_ == item) refreshDetailsView();
        return;
    }

    pearegui::Session* parent = sessionOf(item);
    const size_t folder = item->data(0, FolderIndexRole).toULongLong();
    const size_t index = item->data(0, ResourceIndexRole).toULongLong();
    std::unique_ptr<pearegui::Session> child = parent ? parent->openNested(folder, index) : nullptr;
    if (!child) {
        // Not actually a navigable container after all: leave it as a leaf.
        item->setData(0, SizeRole, item->data(0, SizeRole));
        return;
    }
    pearegui::Session* childPtr = child.get();
    item->setData(0, ChildSessionRole,
                  QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(childPtr)));
    childSessions_.push_back(std::move(child));
    // Building a large subtree (a WIM can hold tens of thousands of entries) with
    // live view updates is what made expansion take a minute; batch it.
    treeView_->setUpdatesEnabled(false);
    populateFromSession(childPtr, item);

    // Reapply semantic folder icons to the lazily-expanded subtree.
    std::function<void(QTreeWidgetItem*)> normalize = [&](QTreeWidgetItem* node) {
        if (!node) return;
        if (itemIsDirectoryLike(node))
            node->setIcon(0, settingsIcons_.folderCloseIcon());
        for (int i = 0; i < node->childCount(); ++i) normalize(node->child(i));
    };
    for (int i = 0; i < item->childCount(); ++i) normalize(item->child(i));
    if (itemIsDirectoryLike(item))
        item->setIcon(0, settingsIcons_.folderOpenIcon());
    treeView_->setUpdatesEnabled(true);
    scheduleTreeIconLoading();
}

void MainWindow::refreshLocalDirectory(QTreeWidgetItem* parent) {
    if (parent) {
        qDeleteAll(parent->takeChildren());
        parent->setData(0, PendingRole, false);
        parent->setData(0, DirectoryLoadedRole, false);
        populateLocalDirectory(parent->data(0, LocalPathRole).toString(), parent);
    } else {
        treeView_->clear();
        populateLocalDirectory(currentDirectory_, nullptr);
    }
    if (detailsMode_) refreshDetailsView();
}

QString MainWindow::localMutationDirectory(QTreeWidgetItem* selected) const {
    if (folderMode_ && selected && selected->data(0, LocalPathRole).isValid()) {
        const QString path = selected->data(0, LocalPathRole).toString();
        return QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath();
    }
    if (detailsMode_ && currentDetailsLocation_ &&
        currentDetailsLocation_->data(0, LocalPathRole).isValid())
        return currentDetailsLocation_->data(0, LocalPathRole).toString();
    return currentDirectory_;
}

void MainWindow::createLocalFile(bool directory) {
    if (!folderMode_) return;
    QTreeWidgetItem* selected = treeView_->currentItem();
    const QString parentPath = localMutationDirectory(selected);
    if (parentPath.isEmpty()) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, directory ? QStringLiteral("New folder") : QStringLiteral("New file"),
        QStringLiteral("Name"), QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty() || name == QStringLiteral(".") ||
        name == QStringLiteral("..") || QFileInfo(name).fileName() != name)
        return;
    const QString path = QDir(parentPath).filePath(name);
    bool ok = false;
    if (directory) {
        ok = QDir().mkdir(path);
    } else {
        QFile file(path);
        ok = file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
        if (ok) file.close();
    }
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Create item"),
                             QStringLiteral("Could not create %1").arg(path));
        return;
    }
    QTreeWidgetItem* parentItem = selected && localDirectory(selected) ? selected
        : (selected ? selected->parent() : (detailsMode_ ? currentDetailsLocation_ : nullptr));
    if (parentItem && !localDirectory(parentItem)) parentItem = parentItem->parent();
    refreshLocalDirectory(parentItem);
}

void MainWindow::createLocalLink(bool junction) {
    if (!folderMode_) return;
#ifndef Q_OS_WIN
    if (junction) {
        QMessageBox::information(this, QStringLiteral("Junction"),
                                 QStringLiteral("Junctions are available only on Windows."));
        return;
    }
#endif
    const QString parentPath = localMutationDirectory(treeView_->currentItem());
    if (parentPath.isEmpty()) return;
    QString target;
    if (junction) {
        target = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Select junction target"), parentPath);
    } else {
        QMessageBox chooser(this);
        chooser.setWindowTitle(QStringLiteral("Select symbolic link target"));
        chooser.setText(QStringLiteral("Choose a file or folder target."));
        QPushButton* fileButton = chooser.addButton(QStringLiteral("File..."), QMessageBox::AcceptRole);
        QPushButton* folderButton = chooser.addButton(QStringLiteral("Folder..."), QMessageBox::AcceptRole);
        chooser.addButton(QMessageBox::Cancel);
        chooser.exec();
        if (chooser.clickedButton() == fileButton) {
            target = QFileDialog::getOpenFileName(
                this, QStringLiteral("Select link target"), parentPath);
        } else if (chooser.clickedButton() == folderButton) {
            target = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Select link target"), parentPath);
        }
    }
    if (target.isEmpty()) return;
    bool accepted = false;
    const QString defaultName = QFileInfo(target).fileName();
    const QString name = QInputDialog::getText(
        this, junction ? QStringLiteral("New junction") : QStringLiteral("New symbolic link"),
        QStringLiteral("Link name"), QLineEdit::Normal, defaultName, &accepted).trimmed();
    if (!accepted || name.isEmpty() || QFileInfo(name).fileName() != name) return;
    const QString linkPath = QDir(parentPath).filePath(name);
    bool ok = false;
#ifdef Q_OS_WIN
    if (junction) {
        ok = createWindowsJunction(linkPath, target);
    } else {
        DWORD flags = QFileInfo(target).isDir() ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
        flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
        ok = CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(linkPath.utf16()),
                                 reinterpret_cast<LPCWSTR>(QFileInfo(target).absoluteFilePath().utf16()),
                                 flags) != FALSE;
    }
#else
    ok = QFile::link(QFileInfo(target).absoluteFilePath(), linkPath);
#endif
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Create link"),
                             QStringLiteral("Could not create link: %1").arg(linkPath));
        return;
    }
    QTreeWidgetItem* selected = treeView_->currentItem();
    QTreeWidgetItem* parentItem = selected && localDirectory(selected) ? selected
        : (selected ? selected->parent() : (detailsMode_ ? currentDetailsLocation_ : nullptr));
    if (parentItem && !localDirectory(parentItem)) parentItem = parentItem->parent();
    refreshLocalDirectory(parentItem);
}

void MainWindow::renameLocalEntry(QTreeWidgetItem* item) {
    if (!folderMode_ || !item || !item->data(0, LocalPathRole).isValid()) return;
    const QString oldPath = item->data(0, LocalPathRole).toString();
    bool accepted = false;
    const QString oldName = QFileInfo(oldPath).fileName();
    const QString newName = QInputDialog::getText(
        this, QStringLiteral("Rename"), QStringLiteral("New name"), QLineEdit::Normal,
        oldName, &accepted).trimmed();
    if (!accepted || newName.isEmpty() || newName == oldName ||
        QFileInfo(newName).fileName() != newName)
        return;
    const QString newPath = QDir(QFileInfo(oldPath).absolutePath()).filePath(newName);
    if (QFileInfo::exists(newPath) || !renameFilesystemEntry(oldPath, newPath)) {
        QMessageBox::warning(this, QStringLiteral("Rename"),
                             QStringLiteral("Could not rename %1").arg(oldPath));
        return;
    }
    refreshLocalDirectory(item->parent());
}

void MainWindow::deleteLocalEntry(QTreeWidgetItem* item) {
    if (!folderMode_ || !item || !item->data(0, LocalPathRole).isValid()) return;
    const QString path = item->data(0, LocalPathRole).toString();
    if (QMessageBox::question(this, QStringLiteral("Delete"),
            QStringLiteral("Delete %1?").arg(QFileInfo(path).fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (!removeFilesystemEntry(path)) {
        QMessageBox::warning(this, QStringLiteral("Delete"),
                             QStringLiteral("Could not delete %1").arg(path));
        return;
    }
    refreshLocalDirectory(item->parent());
}

QString MainWindow::writerEntryPath(QTreeWidgetItem* item) const {
    if (!item || folderMode_ || !currentSession_.isOpen() || currentFilePath_.isEmpty())
        return {};
    pearegui::Session* itemSession = sessionOf(item);
    QVector<QTreeWidgetItem*> transparentOwners;
    if (item->data(0, TypeRole).toString() == QStringLiteral("DISK_PARTITION")) return {};
    while (itemSession && itemSession != &currentSession_) {
        const quintptr sessionId = reinterpret_cast<quintptr>(itemSession);
        QTreeWidgetItem* owner = nullptr;
        for (QTreeWidgetItem* ancestor = item->parent(); ancestor;
             ancestor = ancestor->parent()) {
            if (ancestor->data(0, ChildSessionRole).toULongLong() == sessionId) {
                owner = ancestor;
                break;
            }
        }
        if (!owner) return {};
        const QString ownerType = owner->data(0, TypeRole).toString();
        pearegui::Session* parentSession = sessionOf(owner);
        if (!parentSession || parentSession == itemSession) return {};
        const bool compressedTarWrapper =
            ((ownerType == QStringLiteral("GZIP_FILE") &&
              parentSession->containerFormat() == PEARE_CONTAINER_GZIP) ||
             (ownerType == QStringLiteral("BZIP2_FILE") &&
              parentSession->containerFormat() == PEARE_CONTAINER_BZIP2) ||
             (ownerType == QStringLiteral("ZSTD_FILE") &&
              parentSession->containerFormat() == PEARE_CONTAINER_ZSTD)) &&
            itemSession->containerFormat() == PEARE_CONTAINER_TAR;
        if (compressedTarWrapper) {
            transparentOwners.push_back(owner);
        } else if (ownerType != QStringLiteral("DISK_PARTITION") &&
            !ownerType.endsWith(QStringLiteral("_DIR"), Qt::CaseInsensitive)) {
            return {};
        }
        itemSession = parentSession;
    }
    if (itemSession != &currentSession_) return {};
    QStringList segments;
    for (QTreeWidgetItem* current = item; current; current = current->parent()) {
        if (transparentOwners.contains(current)) continue;
        if (current->data(0, TypeRole).toString() == QStringLiteral("DISK_PARTITION"))
            break;
        QString name = current->data(0, NameRole).toString();
        if (name.isEmpty() && !current->data(0, TypeRole).isValid())
            name = current->text(0);
        if (name.isEmpty()) continue;
        if (name.contains(QLatin1Char('/')) || name == QStringLiteral("..."))
            return {};
        segments.prepend(name);
    }
    return segments.join(QLatin1Char('/'));
}

QString MainWindow::writerDestinationPath(QTreeWidgetItem* item) const {
    if (!item) return {};
    const QString path = writerEntryPath(item);
    if (path.isEmpty()) return {};
    const QString type = item->data(0, TypeRole).toString();
    if (!item->data(0, TypeRole).isValid() ||
        type.endsWith(QStringLiteral("_DIR"), Qt::CaseInsensitive)) return path;
    const int separator = path.lastIndexOf(QLatin1Char('/'));
    return separator < 0 ? QString() : path.left(separator);
}

MainWindow::WriterViewState MainWindow::captureWriterViewState() const {
    WriterViewState state;
    if (detailsMode_ && currentDetailsLocation_)
        state.locationPath = detailsPath(currentDetailsLocation_);
    if (treeView_->currentItem())
        state.selectionPath = detailsPath(treeView_->currentItem());

    std::function<void(QTreeWidgetItem*)> collectExpanded = [&](QTreeWidgetItem* item) {
        if (!item) return;
        if (item->isExpanded()) state.expandedPaths.push_back(detailsPath(item));
        for (int i = 0; i < item->childCount(); ++i)
            collectExpanded(item->child(i));
    };
    for (int i = 0; i < treeView_->topLevelItemCount(); ++i)
        collectExpanded(treeView_->topLevelItem(i));
    return state;
}

void MainWindow::refreshWriterView(const WriterViewState& state,
                                   const QString& selectionPath) {
    reportOffscreenWriterStage(QStringLiteral("view-refresh-started"));
    openFilePath(currentFilePath_);
    if (!currentSession_.isOpen()) return;

    for (const QString& path : state.expandedPaths) {
        QTreeWidgetItem* item = findDetailsPath(path);
        if (item) item->setExpanded(true);
    }
    if (detailsMode_) {
        QTreeWidgetItem* location = state.locationPath.isEmpty()
            ? nullptr : findDetailsPath(state.locationPath);
        navigateDetailsTo(location, false);
    }

    const QString wantedSelection = selectionPath.isEmpty()
        ? state.selectionPath : selectionPath;
    QTreeWidgetItem* selected = wantedSelection.isEmpty()
        ? nullptr : findDetailsPath(wantedSelection);
    if (!selected) return;
    treeView_->setCurrentItem(selected);
    if (detailsMode_ && selected->parent() == currentDetailsLocation_)
        selectDetailsSource(selected);
    reportOffscreenWriterStage(QStringLiteral("view-refreshed"));
}

bool MainWindow::beginWriterUpdate(peare_writer_handle writer) {
    const QByteArray container = currentFilePath_.toUtf8();
    const size_t count = peare_writer_update_target_count(container.constData());
    if (!count) {
        showWriterError(QStringLiteral("Archive update"),
                        QStringLiteral("No writable target is available."));
        return false;
    }
    size_t targetIndex = 0;
    if (count > 1) {
        if (offscreenWriterScenario_) {
            if (offscreenWriterTargetIndex_ < 0 ||
                size_t(offscreenWriterTargetIndex_) >= count) {
                offscreenWriterError_ = QStringLiteral("Invalid writer target index.");
                return false;
            }
            targetIndex = size_t(offscreenWriterTargetIndex_);
        } else {
        QStringList names;
        for (size_t index = 0; index < count; ++index) {
            char buffer[1024] = {};
            if (peare_writer_get_update_target_name(container.constData(), index,
                                                     buffer, sizeof(buffer)) != PEARE_WRITER_OK) {
                showWriterError(QStringLiteral("Archive update"),
                                QStringLiteral("Could not list writable targets."));
                return false;
            }
            names.push_back(QString::fromUtf8(buffer));
        }
        bool accepted = false;
        const QString selected = QInputDialog::getItem(
            this, QStringLiteral("Select writable target"), QStringLiteral("Target"),
            names, 0, false, &accepted);
        if (!accepted) return false;
        targetIndex = size_t(names.indexOf(selected));
        }
    }
    const peare_writer_status status = peare_writer_update_begin_target(
        writer, container.constData(), targetIndex);
    if (status != PEARE_WRITER_OK) {
        showWriterError(QStringLiteral("Archive update"),
                        QString::fromUtf8(peare_writer_last_error(writer)));
        return false;
    }
    reportOffscreenWriterStage(QStringLiteral("writer-update-begun"));
    return true;
}

void MainWindow::createWriterEntry(bool directory) {
    QTreeWidgetItem* selected = treeView_->currentItem();
    QString parent = writerDestinationPath(selected);
    bool accepted = false;
    const QString name = offscreenWriterScenario_
        ? (offscreenWriterTextIndex_ < offscreenWriterTextAnswers_.size()
            ? (accepted = true, offscreenWriterTextAnswers_.at(offscreenWriterTextIndex_++))
            : QString())
        : QInputDialog::getText(
            this, directory ? QStringLiteral("New archive folder") : QStringLiteral("New archive file"),
            QStringLiteral("Name"), QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty() || name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('\\'))) return;
    const QString entry = parent.isEmpty() ? name : parent + QLatin1Char('/') + name;
    const WriterViewState viewState = captureWriterViewState();
    currentSession_.close();
    childSessions_.clear();
    peare_writer_handle writer = nullptr;
    peare_writer_status status = peare_writer_create(&writer);
    if (status == PEARE_WRITER_OK && !beginWriterUpdate(writer)) {
        peare_writer_destroy(writer);
        refreshWriterView(viewState);
        return;
    }
    if (status == PEARE_WRITER_OK)
        status = directory ? peare_writer_create_directory(writer, entry.toUtf8().constData())
                           : peare_writer_create_empty_file(writer, entry.toUtf8().constData());
    if (status == PEARE_WRITER_OK)
        reportOffscreenWriterStage(QStringLiteral("entry-created"));
    if (status == PEARE_WRITER_OK) status = peare_writer_update_finish(writer);
    if (status == PEARE_WRITER_OK)
        reportOffscreenWriterStage(QStringLiteral("writer-update-finished"));
    const QString error = writer ? QString::fromUtf8(peare_writer_last_error(writer))
                                 : QString::fromUtf8(peare_writer_status_message(status));
    if (status != PEARE_WRITER_OK && writer) peare_writer_abort(writer);
    if (writer) peare_writer_destroy(writer);
    if (status != PEARE_WRITER_OK) {
        refreshWriterView(viewState);
        showWriterError(QStringLiteral("Archive update"), error);
        return;
    }
    refreshWriterView(viewState, entry);
}

void MainWindow::renameWriterEntry(QTreeWidgetItem* item) {
    const QString path = writerEntryPath(item);
    if (path.isEmpty()) return;
    const QString oldName = path.section(QLatin1Char('/'), -1);
    bool accepted = false;
    const QString newName = offscreenWriterScenario_
        ? (offscreenWriterTextIndex_ < offscreenWriterTextAnswers_.size()
            ? (accepted = true, offscreenWriterTextAnswers_.at(offscreenWriterTextIndex_++))
            : QString())
        : QInputDialog::getText(
            this, QStringLiteral("Rename archive entry"), QStringLiteral("New name"),
            QLineEdit::Normal, oldName, &accepted).trimmed();
    if (!accepted || newName.isEmpty() || newName.contains(QLatin1Char('/')) ||
        newName.contains(QLatin1Char('\\')) || newName == oldName) return;
    const QString parent = path.section(QLatin1Char('/'), 0, -2);
    const QString target = parent.isEmpty() ? newName : parent + QLatin1Char('/') + newName;
    const WriterViewState viewState = captureWriterViewState();
    currentSession_.close();
    childSessions_.clear();
    peare_writer_handle writer = nullptr;
    peare_writer_status status = peare_writer_create(&writer);
    if (status == PEARE_WRITER_OK && !beginWriterUpdate(writer)) {
        peare_writer_destroy(writer);
        refreshWriterView(viewState);
        return;
    }
    if (status == PEARE_WRITER_OK)
        status = peare_writer_rename_entry_by_id(
            writer, path.toUtf8().constData(), target.toUtf8().constData(),
            item->data(0, BaseIdRole).toLongLong());
    if (status == PEARE_WRITER_OK)
        reportOffscreenWriterStage(QStringLiteral("entry-renamed"));
    if (status == PEARE_WRITER_OK) status = peare_writer_update_finish(writer);
    if (status == PEARE_WRITER_OK)
        reportOffscreenWriterStage(QStringLiteral("writer-update-finished"));
    const QString error = writer ? QString::fromUtf8(peare_writer_last_error(writer))
                                 : QString::fromUtf8(peare_writer_status_message(status));
    if (status != PEARE_WRITER_OK && writer) peare_writer_abort(writer);
    if (writer) peare_writer_destroy(writer);
    if (status != PEARE_WRITER_OK) {
        refreshWriterView(viewState);
        showWriterError(QStringLiteral("Rename archive entry"), error);
        return;
    }
    refreshWriterView(viewState, target);
}

void MainWindow::deleteWriterEntry(QTreeWidgetItem* item) {
    const QString path = writerEntryPath(item);
    if (path.isEmpty()) return;
    if (!offscreenWriterScenario_ && QMessageBox::question(
            this, QStringLiteral("Delete archive entry"),
            QStringLiteral("Delete %1 and its contents?").arg(path),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    const WriterViewState viewState = captureWriterViewState();
    currentSession_.close();
    childSessions_.clear();
    peare_writer_handle writer = nullptr;
    peare_writer_status status = peare_writer_create(&writer);
    if (status == PEARE_WRITER_OK && !beginWriterUpdate(writer)) {
        peare_writer_destroy(writer);
        refreshWriterView(viewState);
        return;
    }
    if (status == PEARE_WRITER_OK)
        status = peare_writer_delete_entry_by_id(
            writer, path.toUtf8().constData(), 1,
            item->data(0, BaseIdRole).toLongLong());
    if (status == PEARE_WRITER_OK)
        reportOffscreenWriterStage(QStringLiteral("entry-deleted"));
    if (status == PEARE_WRITER_OK) status = peare_writer_update_finish(writer);
    if (status == PEARE_WRITER_OK)
        reportOffscreenWriterStage(QStringLiteral("writer-update-finished"));
    const QString error = writer ? QString::fromUtf8(peare_writer_last_error(writer))
                                 : QString::fromUtf8(peare_writer_status_message(status));
    if (status != PEARE_WRITER_OK && writer) peare_writer_abort(writer);
    if (writer) peare_writer_destroy(writer);
    if (status != PEARE_WRITER_OK) {
        refreshWriterView(viewState);
        showWriterError(QStringLiteral("Delete archive entry"), error);
        return;
    }
    refreshWriterView(viewState);
}

void MainWindow::replaceWriterEntry(QTreeWidgetItem* item) {
    const QString path = writerEntryPath(item);
    if (path.isEmpty()) return;
    const QString source = offscreenWriterScenario_
        ? (offscreenWriterFileIndex_ < offscreenWriterFileAnswers_.size()
            ? offscreenWriterFileAnswers_.at(offscreenWriterFileIndex_++) : QString())
        : QFileDialog::getOpenFileName(this, QStringLiteral("Select replacement file"));
    if (source.isEmpty()) return;
    const WriterViewState viewState = captureWriterViewState();
    currentSession_.close();
    childSessions_.clear();
    peare_writer_handle writer = nullptr;
    peare_writer_status status = peare_writer_create(&writer);
    if (status == PEARE_WRITER_OK && !beginWriterUpdate(writer)) {
        peare_writer_destroy(writer);
        refreshWriterView(viewState);
        return;
    }
    if (status == PEARE_WRITER_OK)
        status = peare_writer_replace_entry_by_id(
            writer, source.toUtf8().constData(), path.toUtf8().constData(),
            item->data(0, BaseIdRole).toLongLong());
    if (status == PEARE_WRITER_OK)
        reportOffscreenWriterStage(QStringLiteral("entry-replaced"));
    if (status == PEARE_WRITER_OK) status = peare_writer_update_finish(writer);
    if (status == PEARE_WRITER_OK)
        reportOffscreenWriterStage(QStringLiteral("writer-update-finished"));
    const QString error = writer ? QString::fromUtf8(peare_writer_last_error(writer))
                                 : QString::fromUtf8(peare_writer_status_message(status));
    if (status != PEARE_WRITER_OK && writer) peare_writer_abort(writer);
    if (writer) peare_writer_destroy(writer);
    if (status != PEARE_WRITER_OK) {
        refreshWriterView(viewState);
        showWriterError(QStringLiteral("Replace archive entry"), error);
        return;
    }
    refreshWriterView(viewState);
}

void MainWindow::showItemContextMenu(QWidget* view, const QPoint& position,
                                     QTreeWidgetItem* item) {
    if (view == detailsView_) {
        item = item ? detailsSourceItem(item) : nullptr;
        treeView_->setCurrentItem(item ? item : currentDetailsLocation_);
    }
    if (!item && view == treeView_) {
        treeView_->clearSelection();
        treeView_->setCurrentItem(nullptr);
    }
    if (item && view == treeView_) treeView_->setCurrentItem(item);
    QMenu menu(this);
    if (folderMode_) {
        if (item && item->data(0, LocalPathRole).isValid()) {
            QAction* openAction = menu.addAction(localDirectory(item)
                ? QStringLiteral("Open folder") : QStringLiteral("Open file"));
            connect(openAction, &QAction::triggered, this, [this, item] {
                const QString path = item->data(0, LocalPathRole).toString();
                if (localDirectory(item)) {
                    if (detailsMode_) navigateDetailsTo(item);
                    else if (item->data(0, PendingRole).toBool()) expandContainerItem(item);
                } else openFilePath(path);
            });
            menu.addSeparator();
        }
        menu.addAction(newFolderAction_);
        menu.addAction(newFileAction_);
        QMenu* links = menu.addMenu(QStringLiteral("New link"));
        links->addAction(newSymbolicLinkAction_);
        links->addAction(newJunctionAction_);
        if (item && item->data(0, LocalPathRole).isValid()) {
            menu.addSeparator();
            menu.addAction(renameAction_);
            menu.addAction(deleteAction_);
        }
    } else if (item && item->data(0, TypeRole).isValid()) {
        const QString name = item->data(0, NameRole).toString();
        pearegui::Session* owner = sessionOf(item);
        pearegui::Resource resource = owner->openResource(
            item->data(0, FolderIndexRole).toULongLong(),
            item->data(0, ResourceIndexRole).toULongLong());
        const auto context = resource.context();
        const QByteArray payload = context.dataSize > kMaxDecodePreviewBytes
            ? QByteArray() : resource.payload();
        const QString ext = pearegui::originalExtension(context.type, payload,
                                                         context.identifier.isEmpty() ? name
                                                                                      : context.identifier);
        QAction* original = menu.addAction(QStringLiteral("Export original (%1)").arg(ext));
        connect(original, &QAction::triggered, this, [this] { exportSelectedOriginal(); });
        const QStringList extensions = context.dataSize <= kMaxDecodePreviewBytes
            ? resource.convertedExtensions(owner) : QStringList();
        if (!extensions.isEmpty()) {
            QMenu* converted = menu.addMenu(QStringLiteral("Export converted"));
            for (const QString& extension : extensions) {
                QAction* action = converted->addAction(extension);
                connect(action, &QAction::triggered, this,
                        [this, extension] { exportSelectedConverted(extension); });
            }
        }
        menu.addSeparator();
        QAction* allLeaves = menu.addAction(QStringLiteral("Export all leaves..."));
        connect(allLeaves, &QAction::triggered, this, [this] { exportAllResources(); });
    }
    const bool rootDestination = !item ||
        item->data(0, TypeRole).toString() == QStringLiteral("DISK_PARTITION");
    const bool entryDestination = item && !writerEntryPath(item).isEmpty();
    if (!folderMode_ && (rootDestination || entryDestination)) {
        peare_writer_update_capabilities capabilities = 0;
        const QByteArray encoded = currentFilePath_.toUtf8();
        peare_writer_get_update_capabilities(encoded.constData(), 0, &capabilities);
        menu.addSeparator();
        if (addFilesAction_->isEnabled())
            menu.addAction(addFilesAction_);
        if (capabilities & PEARE_WRITER_CAP_CREATE_DIRECTORY)
            menu.addAction(newFolderAction_);
        if (capabilities & PEARE_WRITER_CAP_CREATE_EMPTY_FILE)
            menu.addAction(newFileAction_);
        if (entryDestination && (capabilities & PEARE_WRITER_CAP_RENAME_ENTRY))
            menu.addAction(renameAction_);
        if (entryDestination && (capabilities & PEARE_WRITER_CAP_DELETE_ENTRY))
            menu.addAction(deleteAction_);
        if (entryDestination && (capabilities & PEARE_WRITER_CAP_UPDATE_FILE) &&
            item->data(0, TypeRole).isValid() &&
            !item->data(0, TypeRole).toString().endsWith(QStringLiteral("_DIR")))
            menu.addAction(replaceAction_);
    }
    if (!menu.isEmpty()) {
        QTreeWidget* tree = qobject_cast<QTreeWidget*>(view);
        if (tree) menu.exec(tree->viewport()->mapToGlobal(position));
    }
}

void MainWindow::showSelectedResource() {
    const auto items = treeView_->selectedItems();
    if (items.isEmpty()) {
        exportOriginalAction_->setEnabled(false);
        exportConvertedAction_->setEnabled(false);
        return;
    }
    QTreeWidgetItem* item = items.first();
    clearContent();
    messageLabel_->clear();
    renameAction_->setEnabled(false);
    deleteAction_->setEnabled(false);
    replaceAction_->setEnabled(false);
    if (folderMode_ && item->data(0, LocalPathRole).isValid()) {
        const QString path = item->data(0, LocalPathRole).toString();
        const QFileInfo info(path);
        if (info.isDir()) {
            auto* label = new QLabel(
                QStringLiteral("<b>%1</b><br><br>Folder: %2")
                    .arg(info.fileName().toHtmlEscaped(), path.toHtmlEscaped()), contentWidget_);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            qobject_cast<QVBoxLayout*>(contentWidget_->layout())->addWidget(label);
        } else {
            showLocalFile(path);
        }
        const bool selected = !info.fileName().isEmpty();
        newFolderAction_->setEnabled(true);
        newFileAction_->setEnabled(true);
        newSymbolicLinkAction_->setEnabled(true);
        newJunctionAction_->setEnabled(true);
        renameAction_->setEnabled(selected);
        deleteAction_->setEnabled(selected);
        exportOriginalAction_->setEnabled(false);
        exportConvertedAction_->setEnabled(false);
        exportAllAction_->setEnabled(info.isDir());
        return;
    }
    if (!item->data(0, TypeRole).isValid()) {
        exportOriginalAction_->setEnabled(false);
        exportConvertedAction_->setEnabled(false);
        if (folderMode_) {
            newFolderAction_->setEnabled(true);
            newFileAction_->setEnabled(true);
            newSymbolicLinkAction_->setEnabled(true);
            newJunctionAction_->setEnabled(true);
            renameAction_->setEnabled(false);
            deleteAction_->setEnabled(false);
        } else {
            const QString path = writerEntryPath(item);
            peare_writer_update_capabilities capabilities = 0;
            const QByteArray encoded = currentFilePath_.toUtf8();
            peare_writer_get_update_capabilities(encoded.constData(), 0, &capabilities);
            newFolderAction_->setEnabled(capabilities & PEARE_WRITER_CAP_CREATE_DIRECTORY);
            newFileAction_->setEnabled(capabilities & PEARE_WRITER_CAP_CREATE_EMPTY_FILE);
            const bool canMutatePath = !path.isEmpty();
            renameAction_->setEnabled(canMutatePath &&
                (capabilities & PEARE_WRITER_CAP_RENAME_ENTRY));
            deleteAction_->setEnabled(canMutatePath &&
                (capabilities & PEARE_WRITER_CAP_DELETE_ENTRY));
        }
        return;
    }
    const QString type = item->data(0, TypeRole).toString(),
                  name = item->data(0, NameRole).toString();
    const bool directResource = item->data(0, DirectResourceRole).toBool();
    const bool directCollection = item->data(0, DirectCollectionRole).toBool();
    if (!folderMode_ && !directResource) {
        const QByteArray encoded = currentFilePath_.toUtf8();
        peare_writer_update_capabilities capabilities = 0;
        peare_writer_get_update_capabilities(encoded.constData(), 0, &capabilities);
        newFolderAction_->setEnabled(capabilities & PEARE_WRITER_CAP_CREATE_DIRECTORY);
        newFileAction_->setEnabled(capabilities & PEARE_WRITER_CAP_CREATE_EMPTY_FILE);
        const bool canMutatePath = !writerEntryPath(item).isEmpty();
        renameAction_->setEnabled(canMutatePath &&
            (capabilities & PEARE_WRITER_CAP_RENAME_ENTRY));
        deleteAction_->setEnabled(canMutatePath &&
            (capabilities & PEARE_WRITER_CAP_DELETE_ENTRY));
        replaceAction_->setEnabled(canMutatePath &&
            (capabilities & PEARE_WRITER_CAP_UPDATE_FILE) &&
            !type.endsWith(QStringLiteral("_DIR")));
    }
    const size_t folder = item->data(0, FolderIndexRole).toULongLong(),
                 index = item->data(0, ResourceIndexRole).toULongLong();
    auto* layout = qobject_cast<QVBoxLayout*>(contentWidget_->layout());
    pearegui::Session* itemSession = sessionOf(item);
    const int directPayloadIndex = item->data(0, DirectPayloadIndexRole).toInt();
    pearegui::Resource resource = directResource && directPayloadIndex >= 0 &&
                                         directPayloadIndex < directResourcePayloads_.size()
        ? pearegui::Resource(directResourcePayloads_.at(directPayloadIndex), pearegui::ResourceContext{
              PEARE_CONTAINER_UNKNOWN, PEARE_PLATFORM_UNKNOWN, currentFilePath_, directResourceType_,
              name, {}, 0, 0,
              quint64(directResourcePayloads_.at(directPayloadIndex).size()), false})
        : itemSession ? itemSession->openResource(folder, index) : pearegui::Resource();
    const bool valid = resource.isValid();
    const pearegui::ResourceContext selectedContext =
        valid ? resource.context() : pearegui::ResourceContext{};
    if (valid && !directResource && !directCollection && item->childCount() == 0) {
        const QVector<QByteArray> faces = pearegui::fontCollectionFaces(resource);
        if (faces.size() > 1) {
            const int start = directResourcePayloads_.size();
            directResourcePayloads_ += faces;
            directResourceType_ = QStringLiteral("RT_FONT");
            item->setData(0, DirectCollectionRole, true);
            for (int i = 0; i < faces.size(); ++i) {
                auto* face = new QTreeWidgetItem(item);
                face->setText(0, QStringLiteral("Face %1").arg(i + 1));
                face->setData(0, TypeRole, QStringLiteral("RT_FONT"));
                face->setData(0, NameRole, QStringLiteral("Face %1").arg(i + 1));
                face->setData(0, DirectResourceRole, true);
                face->setData(0, DirectPayloadIndexRole, start + i);
            }
            item->setExpanded(true);
        }
    }
    const QString displayedType = valid && item->childCount() > 0 &&
                                          selectedContext.containerFormat != PEARE_CONTAINER_UNKNOWN
                                      ? pearegui::containerName(selectedContext.containerFormat)
                                      : type;
    const bool navigationContainer = directCollection ||
        (valid && (selectedContext.isContainer || item->data(0, PendingRole).toBool()));
    // Do not decode lazy container payloads on selection.
    // Some optical tracks map to the full image and would block the GUI thread.
    const bool tooLargeToDecode = valid && selectedContext.dataSize > kMaxDecodePreviewBytes;
    pearegui::Preview preview;
    if (navigationContainer) {
        preview = {};
    } else if (tooLargeToDecode) {
        preview.text = QStringLiteral("Preview disabled for resources larger than 16 MiB.");
    } else {
        preview = pearegui::decode(resource, itemSession);
    }
    quint64 fontGlyphCount = 0;
    pearegui::FontCodepoints fontMappings;
    QString effectiveResourceType = type;
    bool isFontResource = valid && !navigationContainer && !tooLargeToDecode &&
                          pearegui::getUnsigned(resource, PEARE_INFO_FONT_GLYPH_COUNT,
                                                &fontGlyphCount, effectiveResourceType) &&
                          fontGlyphCount > 0;
    if (!isFontResource && valid && !navigationContainer && !tooLargeToDecode &&
        !effectiveResourceType.isEmpty()) {
        // Retry without a declared type so get_info() can classify raw payloads.
        isFontResource =
            pearegui::getUnsigned(resource, PEARE_INFO_FONT_GLYPH_COUNT,
                                  &fontGlyphCount, {}) && fontGlyphCount > 0;
    }
    if (isFontResource) {
        // Recognition is complete.  From here onward use exactly the same RT_FONT
        // get_info/font_render path, regardless of whether the source was an
        // RT_FONT resource or a payload recognized through RawDetect.
        effectiveResourceType = QStringLiteral("RT_FONT");
    }
    if (isFontResource) {
        fontMappings = pearegui::fontCodepoints(resource, effectiveResourceType);
        if (preview.images.isEmpty()) {
            QString sample = QStringLiteral("The quick brown fox jumps over the lazy dog");
            QHash<uint, bool> renderedGlyphs;
            const auto hasCharacter = [&](uint codepoint) {
                if (!fontMappings.usesGlyphIndexes && !fontMappings.values.isEmpty())
                    return fontMappings.values.contains(codepoint);
                const auto cached = renderedGlyphs.constFind(codepoint);
                if (cached != renderedGlyphs.cend()) return cached.value();
                const QString character = QString::fromUcs4(&codepoint, 1);
                const QImage image = pearegui::renderFont(
                    resource, character, 1, 4, 0x000000FFu, 0xFFFFFFFFu,
                    effectiveResourceType);
                bool hasInk = false;
                for (int y = 0; !hasInk && y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        if (image.pixelColor(x, y) != QColor(Qt::white)) {
                            hasInk = true;
                            break;
                        }
                    }
                }
                renderedGlyphs.insert(codepoint, hasInk);
                return hasInk;
            };
            const auto isCovered = [&](const QString& text) {
                if (fontMappings.usesGlyphIndexes) {
                    bool supported = false;
                    if (pearegui::fontSupportsText(resource, text, &supported,
                                                    effectiveResourceType))
                        return supported;
                }
                const QVector<uint> values = text.toUcs4();
                for (uint value : values) {
                    if (QChar::isSpace(value)) continue;
                    if (!hasCharacter(value)) return false;
                }
                return true;
            };
            const QString uppercase = sample.toUpper();
            if (!isCovered(sample) && isCovered(uppercase))
                sample = uppercase;
            if (!fontMappings.usesGlyphIndexes && !fontMappings.values.isEmpty() &&
                !isCovered(sample)) {
                QVector<uint> printable;
                for (quint32 value : fontMappings.values) {
                    if (value < 0x20 || (value >= 0x7f && value <= 0x9f)) continue;
                    printable.push_back(value);
                    if (printable.size() == 48) break;
                }
                if (!printable.isEmpty())
                    sample = QString::fromUcs4(printable.constData(), printable.size());
            }
            for (quint32 scale = 1; scale <= 4; ++scale) {
                const QImage image = pearegui::renderFont(resource, sample, scale, 8, 0x000000FFu,
                                                          0xFFFFFFFFu, effectiveResourceType);
                if (!image.isNull()) preview.images.push_back(image);
            }
        }
        fontGlyphMapPayload_ = resource.payload();
        fontGlyphMapContext_ = resource.context();
        fontGlyphMapResourceType_ = effectiveResourceType;
        fontGlyphMapUsesGlyphIndexes_ = fontMappings.usesGlyphIndexes;
        fontGlyphMapValues_ = fontMappings.values;
        if (fontGlyphMapValues_.isEmpty() && fontGlyphMapUsesGlyphIndexes_ &&
            fontGlyphCount <= quint64(std::numeric_limits<int>::max())) {
            fontGlyphMapValues_.reserve(int(fontGlyphCount));
            for (quint32 index = 0; index < fontGlyphCount; ++index)
                fontGlyphMapValues_.push_back(index);
        } else if (fontGlyphMapValues_.isEmpty()) {
            quint64 first = 0;
            quint64 last = 0;
            if (pearegui::getUnsigned(resource, PEARE_INFO_FONT_FIRST_CHARACTER, &first,
                                      effectiveResourceType) &&
                pearegui::getUnsigned(resource, PEARE_INFO_FONT_LAST_CHARACTER, &last,
                                      effectiveResourceType) && last >= first &&
                last - first < 0x110000) {
                fontGlyphMapValues_.reserve(int(last - first + 1));
                for (quint64 code = first; code <= last; ++code)
                    fontGlyphMapValues_.push_back(quint32(code));
            }
        }
    }
    if (valid && type == QStringLiteral("PE_MODULE") && !tooLargeToDecode) {
        const auto context = resource.context();
        preview.error.clear();
        preview.rawDump = false;
        preview.images.clear();
        preview.pngs.clear();
        preview.texts.clear();
        preview.text =
            QStringLiteral(
                "Loaded PE image\nName: %1\nSize: %2 bytes\n\nExport original preserves the memory "
                "layout.\nExport converted rebuilds a standard PE file layout.")
                .arg(context.identifier)
                .arg(resource.payload().size());
    } else if (valid &&
               (!preview.error.isEmpty() || (preview.images.isEmpty() && preview.text.isEmpty()))) {
        preview.images.clear();
        preview.pngs.clear();
        preview.texts.clear();
        if (navigationContainer) {
            // Expandable container resources are navigation nodes.  Showing their
            // raw bytes as a fallback is misleading when an Opener already
            // exposes the contained payloads below them.
            preview.error.clear();
            preview.text.clear();
            preview.rawDump = false;
        } else if (selectedContext.dataSize > kMaxHexPreviewBytes) {
            preview.text = QStringLiteral("Hex preview disabled for resources larger than 1 MiB.");
            preview.rawDump = false;
        } else {
            preview.text = formatRawDump(resource.payload());
            preview.rawDump = true;
        }
    }
    currentPreview_ = preview;
    if (!preview.images.isEmpty()) contentArea_->setWidgetResizable(false);
    const bool isFontPreview = isFontResource && preview.images.size() >= 4;
    for (int i = 0; i < preview.images.size(); ++i) {
        if (isFontPreview && i < 4) {
            auto* scaleLabel = new QLabel(QStringLiteral("%1x").arg(i + 1), contentWidget_);
            scaleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            QFont labelFont = scaleLabel->font();
            labelFont.setBold(true);
            scaleLabel->setFont(labelFont);
            layout->addWidget(scaleLabel, 0, Qt::AlignLeft);
        }
        auto* picture = new QLabel(contentWidget_);
        picture->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        picture->setPixmap(QPixmap::fromImage(preview.images[i]));
        picture->setFixedSize(preview.images[i].size());
        picture->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        layout->addWidget(picture, 0, Qt::AlignTop | Qt::AlignLeft);
    }
    if (isFontResource && !fontGlyphMapValues_.isEmpty()) {
        fontGlyphMapLabel_ = new QLabel(contentWidget_);
        fontGlyphMapLabel_->setObjectName(QStringLiteral("fontGlyphMapLabel"));
        fontGlyphMapLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        QFont labelFont = fontGlyphMapLabel_->font();
        labelFont.setBold(true);
        fontGlyphMapLabel_->setFont(labelFont);
        layout->addWidget(fontGlyphMapLabel_, 0, Qt::AlignLeft);

        auto* pageControls = new QWidget(contentWidget_);
        auto* pageLayout = new QHBoxLayout(pageControls);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        pageLayout->setSpacing(6);
        fontGlyphMapPreviousButton_ = new QToolButton(pageControls);
        fontGlyphMapPreviousButton_->setObjectName(QStringLiteral("fontGlyphMapPreviousButton"));
        fontGlyphMapPreviousButton_->setText(QStringLiteral("Previous"));
        fontGlyphMapPreviousButton_->setToolTip(QStringLiteral("Show the previous glyph page"));
        fontGlyphMapPreviousButton_->setEnabled(false);
        fontGlyphMapNextButton_ = new QToolButton(pageControls);
        fontGlyphMapNextButton_->setObjectName(QStringLiteral("fontGlyphMapNextButton"));
        fontGlyphMapNextButton_->setText(QStringLiteral("Next"));
        fontGlyphMapNextButton_->setToolTip(QStringLiteral("Show the next glyph page"));
        pageLayout->addWidget(fontGlyphMapPreviousButton_);
        pageLayout->addWidget(fontGlyphMapNextButton_);
        pageLayout->addStretch(1);
        layout->addWidget(pageControls);

        fontGlyphMapImageLabel_ = new QLabel(contentWidget_);
        fontGlyphMapImageLabel_->setObjectName(QStringLiteral("fontGlyphMapImage"));
        fontGlyphMapImageLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        fontGlyphMapImageLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        layout->addWidget(fontGlyphMapImageLabel_, 0, Qt::AlignTop | Qt::AlignLeft);
        connect(fontGlyphMapPreviousButton_, &QToolButton::clicked, this, [this] {
            if (fontGlyphMapPage_ <= 0) return;
            --fontGlyphMapPage_;
            showFontGlyphMapPage();
        });
        connect(fontGlyphMapNextButton_, &QToolButton::clicked, this, [this] {
            const int pageCount = (fontGlyphMapValues_.size() + kFontGlyphMapPageSize - 1) /
                                  kFontGlyphMapPageSize;
            if (fontGlyphMapPage_ + 1 >= pageCount) return;
            ++fontGlyphMapPage_;
            showFontGlyphMapPage();
        });
        showFontGlyphMapPage();
    }
    if (!preview.text.isEmpty()) {
        auto* text = new QPlainTextEdit(contentWidget_);
        text->setReadOnly(true);
        text->setLineWrapMode(QPlainTextEdit::NoWrap);
        text->setFrameShape(QFrame::NoFrame);
        if (preview.rawDump) text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        text->setPlainText(preview.text);
        if (preview.images.isEmpty()) {
            contentArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            contentArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            layout->addWidget(text, 1);
        } else {
            text->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            text->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            text->setFixedHeight(qMax(1, preview.text.count(QLatin1Char('\n')) + 1) *
                                     text->fontMetrics().lineSpacing() +
                                 16);
            layout->addWidget(text);
        }
    }
    if (preview.images.isEmpty() && preview.text.isEmpty()) {
        auto* label = new QLabel(contentWidget_);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        const QString language = item->data(0, LanguageRole).toString();
        QString details = QStringLiteral("<b>%1 / %2</b><br><br>")
                              .arg(displayedType.toHtmlEscaped(), name.toHtmlEscaped());
        if (hasVisibleLanguage(language))
            details += QStringLiteral("Language: %1<br>").arg(language.toHtmlEscaped());
        details += QStringLiteral("Data address: 0x%1<br>Size: %2 bytes<br>Code page: %3")
                       .arg(QString::number(item->data(0, OffsetRole).toULongLong(), 16).toUpper(),
                            QString::number(item->data(0, SizeRole).toULongLong()),
                            QString::number(item->data(0, CodePageRole).toUInt()));
        label->setText(details);
        layout->addWidget(label);
    }
    if (!preview.images.isEmpty()) {
        contentWidget_->adjustSize();
        contentArea_->setWidget(contentWidget_);
    }
    if (valid) {
        const QString ext = pearegui::originalExtension(
            type, navigationContainer || tooLargeToDecode ? QByteArray{} : resource.payload(), name);
        exportOriginalAction_->setText(QStringLiteral("Export original (%1)").arg(ext));
        exportOriginalAction_->setEnabled(true);
    }
    exportConvertedMenu_->clear();
    directConvertedExtension_.clear();
    QStringList extensions = valid ? resource.convertedExtensions(itemSession) : QStringList();
    if (!preview.pngs.isEmpty()) extensions << QStringLiteral(".png");
    if (!preview.texts.isEmpty()) extensions << QStringLiteral(".txt");
    extensions.removeDuplicates();
    exportConvertedAction_->setEnabled(!extensions.isEmpty());
    if (extensions.size() == 1) {
        directConvertedExtension_ = extensions.first();
        exportConvertedAction_->setMenu(nullptr);
        exportConvertedAction_->setText(
            QStringLiteral("Export converted (%1)").arg(directConvertedExtension_));
    } else {
        exportConvertedAction_->setText(QStringLiteral("Export converted"));
        for (const QString& ext : extensions) {
            QAction* action = exportConvertedMenu_->addAction(
                QStringLiteral("%1 (%2)").arg(ext.mid(1).toUpper(), ext));
            connect(action, &QAction::triggered, this,
                    [this, ext] { exportSelectedConverted(ext); });
        }
        exportConvertedAction_->setMenu(extensions.isEmpty() ? nullptr : exportConvertedMenu_);
    }
    messageLabel_->setText(preview.error.isEmpty() ? displayedType + QStringLiteral(" / ") + name
                                                   : preview.error);
    if (directResource) {
        newFolderAction_->setEnabled(false);
        newFileAction_->setEnabled(false);
        newSymbolicLinkAction_->setEnabled(false);
        newJunctionAction_->setEnabled(false);
        renameAction_->setEnabled(false);
        deleteAction_->setEnabled(false);
        replaceAction_->setEnabled(false);
        exportOriginalAction_->setEnabled(false);
        exportConvertedAction_->setEnabled(false);
        exportAllAction_->setEnabled(false);
        resourceMenu_->setEnabled(false);
    }
}

void MainWindow::exportSelectedOriginal() {
    const auto items = treeView_->selectedItems();
    if (items.isEmpty() || !items.first()->data(0, TypeRole).isValid()) return;
    const size_t folder = items.first()->data(0, FolderIndexRole).toULongLong(),
                 index = items.first()->data(0, ResourceIndexRole).toULongLong();
    pearegui::Resource resource = sessionOf(items.first())->openResource(folder, index);
    if (!resource.isValid()) return;
    const pearegui::ResourceContext context = resource.context();
    const QByteArray payload = resource.payload();
    const QString ext = pearegui::originalExtension(context.type, payload, context.identifier);
    QString base = pearegui::sanitizeFileName(QFileInfo(currentFilePath_).completeBaseName() +
                                              QStringLiteral("_") + context.type +
                                              QStringLiteral("_") + context.identifier);
    if (QFileInfo(context.identifier).suffix().isEmpty()) base += ext;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export original resource"), QDir::home().filePath(base),
        QStringLiteral("Resource (*%1);;All files (*.*)").arg(ext));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size()) {
        QMessageBox::warning(this, QStringLiteral("Export resource"),
                             QStringLiteral("Cannot write resource to %1").arg(path));
        return;
    }
    messageLabel_->setText(QStringLiteral("Exported original resource: %1").arg(path));
}

void MainWindow::exportSelectedConverted(const QString& extension) {
    const auto items = treeView_->selectedItems();
    if (items.isEmpty() || !items.first()->data(0, TypeRole).isValid()) return;
    const QString base = pearegui::sanitizeFileName(
        QFileInfo(currentFilePath_).completeBaseName() + QStringLiteral("_") +
        items.first()->data(0, TypeRole).toString() + QStringLiteral("_") +
        items.first()->data(0, NameRole).toString());
    const size_t folder = items.first()->data(0, FolderIndexRole).toULongLong(),
                 index = items.first()->data(0, ResourceIndexRole).toULongLong();
    pearegui::Resource resource = sessionOf(items.first())->openResource(folder, index);
    QVector<QByteArray> files =
        resource.isValid() ? resource.convert(extension, sessionOf(items.first()))
                           : QVector<QByteArray>();
    if (files.isEmpty())
        files = extension == QStringLiteral(".png") ? currentPreview_.pngs : currentPreview_.texts;
    if (files.isEmpty()) return;
    if (files.size() == 1) {
        const QString path =
            QFileDialog::getSaveFileName(this, QStringLiteral("Export converted resource"),
                                         QDir::home().filePath(base + extension),
                                         QStringLiteral("%1 file (*%2);;All files (*.*)")
                                             .arg(extension.mid(1).toUpper(), extension));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(files.first()) != files.first().size()) {
            QMessageBox::warning(this, QStringLiteral("Export resource"),
                                 QStringLiteral("Cannot write converted resource to %1").arg(path));
            return;
        }
        messageLabel_->setText(QStringLiteral("Exported converted resource: %1").arg(path));
        return;
    }
    const QString destination = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select the folder for the converted resource files"),
        QDir::homePath());
    if (destination.isEmpty()) return;
    int exported = 0;
    for (int i = 0; i < files.size(); ++i) {
        const QString path = pearegui::uniquePath(
            QDir(destination)
                .filePath(QStringLiteral("%1_%2%3").arg(base).arg(i + 1).arg(extension)));
        QFile f(path);
        if (f.open(QIODevice::WriteOnly) && f.write(files[i]) == files[i].size()) ++exported;
    }
    messageLabel_->setText(QStringLiteral("Exported %1 converted resource files to %2")
                               .arg(exported)
                               .arg(destination));
}

void MainWindow::exportAllResources() {
    if (!currentSession_.isOpen() || currentFilePath_.isEmpty() || folderMode_) return;
    const QString destination = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select the destination folder for all original resources"),
        QDir::homePath());
    if (destination.isEmpty()) return;
    const QString root =
        QDir(destination)
            .filePath(pearegui::sanitizeFileName(QFileInfo(currentFilePath_).completeBaseName()) +
                      QStringLiteral("_resources"));
    QDir().mkpath(root);
    int exported = 0, failed = 0;
    std::function<void(QTreeWidgetItem*, const QStringList&, int)> exportNode;
    exportNode = [&](QTreeWidgetItem* item, const QStringList& parentPath, int depth) {
        if (!item || depth > 32) return;
        if (item->data(0, PendingRole).toBool()) expandContainerItem(item);
        const bool resourceNode = item->data(0, TypeRole).isValid();
        const QString name = item->data(0, NameRole).toString().isEmpty()
            ? item->text(0) : item->data(0, NameRole).toString();
        const bool nestedSession = item->data(0, ChildSessionRole).isValid();
        if (nestedSession) {
            QStringList nestedPath = parentPath;
            nestedPath << pearegui::sanitizeFileName(name);
            for (int i = 0; i < item->childCount(); ++i)
                exportNode(item->child(i), nestedPath, depth + 1);
            return;
        }
        if (resourceNode) {
            pearegui::Session* owner = sessionOf(item);
            const size_t folder = item->data(0, FolderIndexRole).toULongLong();
            const size_t index = item->data(0, ResourceIndexRole).toULongLong();
            pearegui::Resource resource = owner ? owner->openResource(folder, index)
                                                 : pearegui::Resource();
            if (!resource.isValid()) {
                ++failed;
                return;
            }
            const pearegui::ResourceContext context = resource.context();
            const QByteArray payload = resource.payload();
            QStringList relative = parentPath;
            QString fileName = pearegui::sanitizeFileName(context.identifier);
            if (QFileInfo(context.identifier).suffix().isEmpty())
                fileName += pearegui::originalExtension(context.type, payload,
                                                        context.identifier);
            relative << fileName;
            QString destinationPath = root;
            for (const QString& part : relative)
                destinationPath = QDir(destinationPath).filePath(pearegui::sanitizeFileName(part));
            const QFileInfo targetInfo(destinationPath);
            QDir().mkpath(targetInfo.absolutePath());
            destinationPath = pearegui::uniquePath(destinationPath);
            QFile file(destinationPath);
            if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size()) {
                ++failed;
                return;
            }
            ++exported;
            return;
        }
        QStringList nestedPath = parentPath;
        nestedPath << pearegui::sanitizeFileName(item->text(0));
        for (int i = 0; i < item->childCount(); ++i)
            exportNode(item->child(i), nestedPath, depth + 1);
    };

    QTreeWidgetItem* scope = detailsMode_ ? currentDetailsLocation_ : treeView_->currentItem();
    if (scope) {
        if (scope->data(0, PendingRole).toBool()) expandContainerItem(scope);
        if (scope->data(0, ChildSessionRole).isValid()) {
            const QString scopeName = scope->data(0, NameRole).toString().isEmpty()
                ? scope->text(0) : scope->data(0, NameRole).toString();
            for (int i = 0; i < scope->childCount(); ++i)
                exportNode(scope->child(i), {pearegui::sanitizeFileName(scopeName)}, 0);
        } else if (scope->childCount() > 0) {
            for (int i = 0; i < scope->childCount(); ++i)
                exportNode(scope->child(i), {}, 0);
        } else {
            exportNode(scope, {}, 0);
        }
    } else {
        for (int i = 0; i < treeView_->topLevelItemCount(); ++i)
            exportNode(treeView_->topLevelItem(i), {}, 0);
    }
    QString summary =
        QStringLiteral("Exported %1 original resources to %2").arg(exported).arg(root);
    if (failed) summary += QStringLiteral(". Failed: %1").arg(failed);
    messageLabel_->setText(summary);
    QMessageBox::information(this, QStringLiteral("Export all resources"), summary);
}

void MainWindow::showAbout() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("About"));
    dialog.setWindowFlags((dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint) |
                          Qt::MSWindowsFixedSizeDialogHint);
    dialog.setFixedSize(297, 157);

    auto* authorLabel = new QLabel(QStringLiteral("Peare - Raul Merelli"), &dialog);
    authorLabel->setAlignment(Qt::AlignCenter);
    authorLabel->setGeometry(0, 6, 297, 18);

    auto* logoLabel = new QLabel(&dialog);
    logoLabel->setAlignment(Qt::AlignCenter);
    logoLabel->setGeometry(0, 25, 297, 107);
    const QPixmap logo(QStringLiteral(":/peare/logo.png"));
    logoLabel->setPixmap(logo.scaled(297, 107, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    auto* projectLink = new QLabel(
        QStringLiteral(
            "<a href=\"https://github.com/RaulMerelli/Peare\">github.com/RaulMerelli/Peare</a>"),
        &dialog);
    projectLink->setTextFormat(Qt::RichText);
    projectLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
    projectLink->setOpenExternalLinks(true);
    projectLink->setAlignment(Qt::AlignCenter);
    projectLink->setGeometry(0, 133, 297, 18);

    dialog.exec();
}
