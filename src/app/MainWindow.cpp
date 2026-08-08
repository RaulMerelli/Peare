#include "MainWindow.h"
#include <functional>
#include "IconFromExt.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QFrame>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPushButton>
#include <QPixmap>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

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
    SessionRole, // pearegui::Session* that owns this item's folder/resource index
    PendingRole  // true: a lazy container whose children are not populated yet
};

bool hasVisibleLanguage(const QString& language) {
    QString value = language.trimmed();
    if (value.isEmpty()) return false;

    if (value.startsWith(QLatin1Char('#'))) value.remove(0, 1);

    if (value.compare(QStringLiteral("neutral"), Qt::CaseInsensitive) == 0) return false;

    bool numeric = false;
    const uint languageId = value.toUInt(&numeric, 0);
    return !numeric || languageId != 0;
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
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Peare"));
    resize(1067, 498);
    setMinimumSize(760, 360);
    buildMenus();
    buildCentralUi();
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
    QAction* openAction = new QAction(QStringLiteral("&Open"), this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, [this] { openFile(); });

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

    QMenu* fileMenu = applicationMenuBar->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(openAction);
    fileMenu->addSeparator();
    fileMenu->addAction(exitAction);

    resourceMenu_ = applicationMenuBar->addMenu(QStringLiteral("&Resource"));
    resourceMenu_->addAction(exportOriginalAction_);
    resourceMenu_->addAction(exportConvertedAction_);
    exportConvertedMenu_ = new QMenu(QStringLiteral("Export converted"), resourceMenu_);
    resourceMenu_->addSeparator();
    resourceMenu_->addAction(exportAllAction_);
    resourceMenu_->setEnabled(false);

    QMenu* viewMenu = applicationMenuBar->addMenu(QStringLiteral("&View"));
    viewMenu->addAction(expandAction);
    viewMenu->addAction(collapseAction);

    QMenu* helpMenu = applicationMenuBar->addMenu(QStringLiteral("&Help"));
    helpMenu->addAction(issueAction);
    helpMenu->addSeparator();
    helpMenu->addAction(aboutAction);
}

void MainWindow::buildCentralUi() {
    QWidget* central = new QWidget(this);
    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(0, 1, 12, 0);
    root->setSpacing(7);

    treeView_ = new QTreeWidget(central);
    treeView_->setHeaderHidden(true);
    treeView_->setFixedWidth(341);
    treeView_->setIndentation(18);
    treeView_->setUniformRowHeights(true);
    treeView_->setFrameShape(QFrame::StyledPanel);
    root->addWidget(treeView_);

    QWidget* right = new QWidget(central);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 12);
    rightLayout->setSpacing(3);

    messageLabel_ = new QLabel(right);
    messageLabel_->setFixedHeight(23);
    messageLabel_->setFrameShape(QFrame::Box);
    messageLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    messageLabel_->setMargin(4);
    rightLayout->addWidget(messageLabel_);

    contentArea_ = new QScrollArea(right);
    contentArea_->setWidgetResizable(true);
    contentArea_->setFrameShape(QFrame::NoFrame);
    contentArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    contentArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    clearContent();
    rightLayout->addWidget(contentArea_, 1);

    root->addWidget(right, 1);
    setCentralWidget(central);

    connect(treeView_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::showSelectedResource);
    connect(treeView_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        expandContainerItem(item); // lazily open a nested container
        if (item && item->childCount() > 0) item->setIcon(0, settingsIcons_.folderOpenIcon());
    });
    connect(treeView_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (item && item->childCount() > 0) item->setIcon(0, settingsIcons_.folderCloseIcon());
    });
}

void MainWindow::clearContent() {
    QWidget* old = contentArea_ ? contentArea_->takeWidget() : nullptr;
    if (old) old->deleteLater();
    contentWidget_ = new QWidget;
    contentWidget_->setStyleSheet(QStringLiteral("background: white;"));
    auto* layout = new QVBoxLayout(contentWidget_);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(3);
    layout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    if (contentArea_) {
        // Reset the scrolling mode for every new preview. Image previews may
        // switch widgetResizable off later, but text previews must always fill
        // the viewport; otherwise the previous bitmap selection leaves the
        // text editor inside a minimum-size widget with two tiny scrollbars.
        contentArea_->setWidgetResizable(true);
        contentArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        contentArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        contentArea_->setWidget(contentWidget_);
    }
}

void MainWindow::openFile() {
    const QString filters = QStringLiteral(
        "All supported modules (*.exe *.dll *.sys *.ocx *.cpl *.scr *.drv *.ax *.efi *.mui *.tlb "
        "*.acm *.spl *.sct *.wll *.xll *.fll *.pyd *.bpl *.ifs *.msstyles *.386 *.vxd *.fon *.le "
        "*.lx *.xbe *.xex *.xzp *.xuiz *.live *.pirs *.con *.bin *.nb0 *.nbf *.fim *.img *.ima "
        "*.vfd *.qcow *.qcow2 *.pup *.appx *.msix *.appxbundle *.xap *.eappx *.emsix *.eappxbundle "
        "*.emsixbundle *.wad *.bkf *.mtf *.mdf *.mds *.rom *.msi *.msm *.msp *.mst *.msg);;"
        "DOS, Windows and OS/2 executables (*.exe *.dll *.sys *.ocx *.cpl *.scr *.drv *.ax *.efi "
        "*.mui *.tlb *.acm *.spl *.sct *.wll *.xll *.fll *.pyd *.bpl *.ifs *.msstyles *.386 *.vxd "
        "*.fon *.le *.lx);;"
        "Portable Executable (PE) (*.exe *.dll *.sys *.ocx *.cpl *.scr *.drv *.ax *.efi *.mui "
        "*.tlb *.acm *.spl *.sct *.wll *.xll *.fll *.pyd *.bpl *.ifs *.msstyles);;"
        "New Executable (NE) (*.exe *.dll *.drv *.386 *.vxd *.fon);;"
        "Linear Executable (LE/LX) (*.exe *.dll *.drv *.le *.lx);;"
        "Xbox executables (*.xbe *.xex);;"
        "Xbox 360 archives and packages (*.xzp *.xuiz *.live *.pirs *.con);;"
        "Windows CE ROM and IMGFS (*.bin *.nb0 *.nbf *.fim);;"
        "Raw PC floppy images (*.img *.ima *.vfd);;"
        "QEMU virtual disks (*.qcow *.qcow2);;"
        "PlayStation 3 updates (*.pup);;"
        "PlayStation 2 ROMDIR / IOPRP images (*.rom *.bin *.img);;"
        "Windows Installer databases (*.msi *.msm *.msp *.mst);;"
        "Microsoft Outlook messages (*.msg);;"
        "Windows application packages (*.appx *.msix *.appxbundle *.xap *.eappx *.emsix "
        "*.eappxbundle *.emsixbundle *.wad *.bkf *.mtf *.mdf *.mds);;"
        "All files (*.*)");

    QFileDialog dialog(this, QStringLiteral("Open module"));
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);

    // Deliberate order: keep application/octet-stream available to native
    // MIME-only pickers, then restore Peare's original desktop name filters.
    dialog.setMimeTypeFilters(QStringList()
                              << QStringLiteral("application/octet-stream")
                              << QStringLiteral("application/x-msdownload")
                              << QStringLiteral("application/vnd.microsoft.portable-executable")
                              << QStringLiteral("application/x-dosexec")
                              << QStringLiteral("application/x-msdos-program")
                              << QStringLiteral("application/x-executable")
                              << QStringLiteral("application/x-sharedlib")
                              << QStringLiteral("application/x-object"));
    dialog.setNameFilters(filters.split(QStringLiteral(";;")));

    if (dialog.exec() != QDialog::Accepted) return;

    const QList<QUrl> urls = dialog.selectedUrls();
    if (urls.isEmpty()) return;

    const QUrl url = urls.first();
    const QString localPath = url.isLocalFile() ? url.toLocalFile() : url.toString();
    currentFilePath_ = localPath;
    showModuleSummary(localPath);
}

void MainWindow::showModuleSummary(const QString& filePath) {
    QString error;
    const QFileInfo fi(filePath);
    const QString sourceName = fi.fileName().isEmpty() ? filePath : fi.fileName();
    // Reset all previous state (tree + any nested child sessions) before the new
    // file is opened, so nothing from a prior document can survive.
    treeView_->clear();
    childSessions_.clear();
    currentPreview_ = {};
    clearContent();
    messageLabel_->clear();
    resourceMenu_->setEnabled(false);

    // Open by path: the module memory-maps the file (no whole-file read, no temp
    // copy), so even multi-hundred-MB images open instantly.
    currentSession_.openFile(filePath, &error);

    if (!currentSession_.isOpen()) {
        setWindowTitle(QStringLiteral("Peare"));
        QMessageBox::information(this, QStringLiteral("Info file"),
                                 QStringLiteral("Cannot open file: %1\n%2").arg(sourceName, error));
        return;
    }

    const peare_container_format format = currentSession_.containerFormat();
    size_t totalResources = 0;
    for (size_t f = 0; f < currentSession_.folderCount(); ++f)
        totalResources += currentSession_.resourceCount(f);
    treeView_->setUpdatesEnabled(false);
    populateFromSession(&currentSession_, nullptr);

    // Every expandable native container uses folder icons consistently.
    // Leaf sections/resources keep their file/type icon.
    std::function<void(QTreeWidgetItem*)> normalizeExpandableIcons = [&](QTreeWidgetItem* item) {
        if (!item) return;
        if (item->childCount() > 0) item->setIcon(0, settingsIcons_.folderCloseIcon());
        for (int i = 0; i < item->childCount(); ++i) normalizeExpandableIcons(item->child(i));
    };
    for (int i = 0; i < treeView_->topLevelItemCount(); ++i)
        normalizeExpandableIcons(treeView_->topLevelItem(i));
    treeView_->setUpdatesEnabled(true);

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
        QStringLiteral("<br>Resource types: %1<br>Resource entries: %2")
            .arg(QString::number(currentSession_.folderCount()), QString::number(totalResources));
    summary->setText(summaryHtml);
    layout->addWidget(summary);
    messageLabel_->setText(totalResources == 0
                               ? QStringLiteral("No resources found")
                               : QStringLiteral("%1 resources found").arg(totalResources));
    resourceMenu_->setEnabled(totalResources > 0);
    exportOriginalAction_->setEnabled(false);
    exportConvertedAction_->setEnabled(false);
    exportAllAction_->setEnabled(totalResources > 0);
}

static QImage cropVisibleGlyph(const QImage& source) {
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
    return right >= left && bottom >= top
               ? source.copy(left, top, right - left + 1, bottom - top + 1)
               : source;
}

QImage MainWindow::buildFontGlyphMap(const pearegui::Resource& resource,
                                     const QString& resourceType) const {
    quint64 first = 0, last = 0;
    if (!pearegui::getUnsigned(resource, PEARE_INFO_FONT_FIRST_CHARACTER, &first, resourceType) ||
        !pearegui::getUnsigned(resource, PEARE_INFO_FONT_LAST_CHARACTER, &last, resourceType))
        return {};
    if (last < first || last - first + 1 > 65536) return {};
    const int scale = 1;
    QVector<QImage> glyphs;
    glyphs.reserve(int(last - first + 1));
    // Size the glyph-map cells from the actual painted glyph bounds.  Vector
    // font pixelHeight is an em/metric value and can be several times larger
    // than the visible outline, so it must not define the cell canvas.
    int imageAreaWidth = 1;
    int imageAreaHeight = 1;
    for (quint64 code = first; code <= last; ++code) {
        QImage glyph = pearegui::renderFont(resource, QString(QChar(static_cast<ushort>(code))),
                                            scale, 0, 0x000000FFu, 0xFFFFFF00u, resourceType);
        glyph = cropVisibleGlyph(glyph);
        glyphs.push_back(glyph);
        if (!glyph.isNull()) {
            imageAreaWidth = qMin(96, qMax(imageAreaWidth, glyph.width()));
            imageAreaHeight = qMin(96, qMax(imageAreaHeight, glyph.height()));
        }
    }
    imageAreaWidth = qMax(4, imageAreaWidth);
    imageAreaHeight = qMax(4, imageAreaHeight);
    const int columns = 16, cellWidth = imageAreaWidth + 12, cellHeight = imageAreaHeight + 18;
    const int rows = (glyphs.size() + columns - 1) / columns;
    QImage result(columns * cellWidth + 1, rows * cellHeight + 1, QImage::Format_ARGB32);
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
        const quint64 code = first + quint64(i);
        painter.drawText(x + 2, y + imageAreaHeight + 3, cellWidth - 4, 14,
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(code, 16).toUpper().rightJustified(code <= 0xFF ? 2 : 4,
                                                                            QLatin1Char('0')));
    }
    return result;
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
                existing = parent ? new QTreeWidgetItem(parent, QStringList(segment))
                                  : new QTreeWidgetItem(treeView_, QStringList(segment));
                existing->setIcon(0, settingsIcons_.folderCloseIcon());
                existing->setData(0, SessionRole, QVariant::fromValue<qulonglong>(sid));
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
            if (hasVisibleLanguage(context.language))
                label += QStringLiteral(" [%1]").arg(context.language);
            // Filesystem entries (files and directories from ISO/FAT/NTFS/ext/...
            // listings) are shown flat by name directly under the container, never
            // grouped under a synthetic "<FMT>_FILE"/"<FMT>_DIR" type node.
            const bool filesystemEntry = context.type.endsWith(QStringLiteral("_DIR")) ||
                                         context.type == QStringLiteral("DISK_PARTITION") ||
                                         context.type == QStringLiteral("ISO_FILE") ||
                                         context.type == QStringLiteral("WIM_FILE") ||
                                         context.type == QStringLiteral("WINCE_FILE") ||
                                         context.type == QStringLiteral("OS2_PACK_FILE") ||
                                         context.type == QStringLiteral("FAT_FILE") ||
                                         context.type == QStringLiteral("EXFAT_FILE") ||
                                         context.type == QStringLiteral("NTFS_FILE") ||
                                         context.type == QStringLiteral("EXT_FILE") ||
                                         context.type == QStringLiteral("XFS_FILE") ||
                                         context.type == QStringLiteral("JFS_FILE") ||
                                         context.type == QStringLiteral("HPFS_FILE") ||
                                         context.type == QStringLiteral("SIEMENS_FSF_FILE") ||
                                         context.type == QStringLiteral("SQUASHFS_FILE") ||
                                         context.type == QStringLiteral("HFS_FILE") ||
                                         context.type == QStringLiteral("BTRFS_FILE") ||
                                         context.type == QStringLiteral("CAB_FILE") ||
                                         context.type == QStringLiteral("ZIP_FILE") ||
                                         context.type == QStringLiteral("APP_PACKAGE_FILE") ||
                                         context.type == QStringLiteral("APP_BUNDLE_FILE") ||
                                         context.type == QStringLiteral("XAP_FILE") ||
                                         context.type == QStringLiteral("XAP_METADATA") ||
                                         context.type == QStringLiteral("XAP_ENCRYPTED_ARCHIVE") ||
                                         context.type == QStringLiteral("TAR_FILE") ||
                                         context.type == QStringLiteral("PUP_SEGMENT") ||
                                         context.type == QStringLiteral("WAD_LUMP") ||
                                         context.type == QStringLiteral("MDF_TRACK") ||
                                         context.type == QStringLiteral("PS2_ROM_FILE") ||
                                         context.type == QStringLiteral("MSI_STREAM") ||
                                         context.type == QStringLiteral("MSG_ATTACHMENT") ||
                                         context.type == QStringLiteral("REG_KEY") ||
                                         context.type == QStringLiteral("REG_VALUE") ||
                                         context.type == QStringLiteral("BCD_OBJECT") ||
                                         context.type == QStringLiteral("BCD_SUMMARY") ||
                                         context.type == QStringLiteral("BCD_ELEMENT") ||
                                         context.type == QStringLiteral("UDF_FILE") ||
                                         context.type == QStringLiteral("LVM_LOGICAL_VOLUME") ||
                                         context.type == QStringLiteral("LINUX_RAID_VOLUME") ||
                                         context.type == QStringLiteral("LDM_VOLUME");
            QStringList hierarchyPath = folderType.split(QChar(0x1f), Qt::SkipEmptyParts);
            if (hierarchyPath.isEmpty() && !filesystemEntry) hierarchyPath << context.type;
            auto* typeItem = ensurePath(hierarchyPath);
            QTreeWidgetItem* child = nullptr;
            if (context.type == QStringLiteral("PE_MODULE") ||
                context.type == QStringLiteral("PE_HEADERS") ||
                context.type == QStringLiteral("PE_SECTION") ||
                context.type == QStringLiteral("XBE_SECTION") ||
                context.type == QStringLiteral("NE_HEADERS") ||
                context.type == QStringLiteral("NE_AREA") ||
                context.type == QStringLiteral("LE_HEADERS") ||
                context.type == QStringLiteral("LE_AREA") ||
                context.type == QStringLiteral("LX_HEADERS") ||
                context.type == QStringLiteral("LX_AREA") ||
                context.type == QStringLiteral("XUIZ_CONTAINER") ||
                context.type == QStringLiteral("XUIZ_FILE") ||
                context.type == QStringLiteral("SZDD_FILE") ||
                context.type == QStringLiteral("SIEMENS_IMG_FILE") ||
                context.type == QStringLiteral("SIEMENS_FWF_FILE")) {
                child = typeItem;
                child->setText(0, label);
            } else {
                if (typeItem)
                    child = new QTreeWidgetItem(typeItem, QStringList(label));
                else
                    child = parentBase ? new QTreeWidgetItem(parentBase, QStringList(label))
                                       : new QTreeWidgetItem(treeView_, QStringList(label));
            }
            // Filesystem files get an icon by their real extension; the folder
            // icon for directories is applied by the is-container branch below.
            child->setIcon(0, filesystemEntry ? settingsIcons_.iconForFileName(context.identifier)
                                              : settingsIcons_.resourceIcon(context.type));
            child->setData(0, TypeRole, context.type);
            child->setData(0, NameRole, context.identifier);
            child->setData(0, LanguageRole, context.language);
            child->setData(0, OffsetRole, QVariant::fromValue<qulonglong>(context.dataOffset));
            child->setData(0, SizeRole, QVariant::fromValue<qulonglong>(context.dataSize));
            child->setData(0, CodePageRole, context.codepage);
            child->setData(0, FolderIndexRole, QVariant::fromValue<qulonglong>(folderIndex));
            child->setData(0, ResourceIndexRole, QVariant::fromValue<qulonglong>(resourceIndex));
            child->setData(0, SessionRole, QVariant::fromValue<qulonglong>(sid));
            if (context.isContainer) {
                // Nested container: mark expandable with a placeholder child that
                // is replaced by real content the first time it is expanded.
                child->setData(0, PendingRole, true);
                child->setIcon(0, settingsIcons_.folderCloseIcon());
                new QTreeWidgetItem(child, QStringList(QStringLiteral("...")));
            }
        }
    }
}

void MainWindow::expandContainerItem(QTreeWidgetItem* item) {
    if (!item || !item->data(0, PendingRole).toBool()) return;
    item->setData(0, PendingRole, false); // handled (also prevents recursion)
    qDeleteAll(item->takeChildren());     // drop the placeholder

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
    childSessions_.push_back(std::move(child));
    // Building a large subtree (a WIM can hold tens of thousands of entries) with
    // live view updates is what made expansion take a minute; batch it.
    treeView_->setUpdatesEnabled(false);
    populateFromSession(childPtr, item);

    // Normalise folder icons in the freshly built subtree (the top-level build
    // does this too, but that pass does not reach lazily-expanded children):
    // any node with children is a folder, e.g. the .rsrc / RT_* grouping nodes.
    std::function<void(QTreeWidgetItem*)> normalize = [&](QTreeWidgetItem* node) {
        if (!node) return;
        if (node->childCount() > 0) node->setIcon(0, settingsIcons_.folderCloseIcon());
        for (int i = 0; i < node->childCount(); ++i) normalize(node->child(i));
    };
    for (int i = 0; i < item->childCount(); ++i) normalize(item->child(i));
    item->setIcon(0, settingsIcons_.folderOpenIcon());
    treeView_->setUpdatesEnabled(true);
}

void MainWindow::showSelectedResource() {
    const auto items = treeView_->selectedItems();
    if (items.isEmpty()) return;
    QTreeWidgetItem* item = items.first();
    clearContent();
    messageLabel_->clear();
    if (!item->data(0, TypeRole).isValid()) return;
    const QString type = item->data(0, TypeRole).toString(),
                  name = item->data(0, NameRole).toString();
    const size_t folder = item->data(0, FolderIndexRole).toULongLong(),
                 index = item->data(0, ResourceIndexRole).toULongLong();
    auto* layout = qobject_cast<QVBoxLayout*>(contentWidget_->layout());
    pearegui::Session* itemSession = sessionOf(item);
    pearegui::Resource resource = itemSession->openResource(folder, index);
    const bool valid = resource.isValid();
    const pearegui::ResourceContext selectedContext =
        valid ? resource.context() : pearegui::ResourceContext{};
    const QString displayedType = valid && item->childCount() > 0 &&
                                          selectedContext.containerFormat != PEARE_CONTAINER_UNKNOWN
                                      ? pearegui::containerName(selectedContext.containerFormat)
                                      : type;
    pearegui::Preview preview = pearegui::decode(resource, itemSession);
    quint64 fontFirst = 0;
    QString effectiveResourceType = type;
    bool isFontResource = valid && pearegui::getUnsigned(resource, PEARE_INFO_FONT_FIRST_CHARACTER,
                                                         &fontFirst, effectiveResourceType);
    if (!isFontResource && valid && !effectiveResourceType.isEmpty()) {
        // Retry without a declared type so get_info() can classify raw payloads.
        isFontResource =
            pearegui::getUnsigned(resource, PEARE_INFO_FONT_FIRST_CHARACTER, &fontFirst, {});
    }
    if (isFontResource) {
        // Recognition is complete.  From here onward use exactly the same RT_FONT
        // get_info/font_render path, regardless of whether the source was an
        // RT_FONT resource or a payload recognized through RawDetect.
        effectiveResourceType = QStringLiteral("RT_FONT");
    }
    if (isFontResource) {
        const QString sample = QStringLiteral("The quick brown fox jumps over the lazy dog");
        for (quint32 scale = 1; scale <= 4; ++scale) {
            const QImage image = pearegui::renderFont(resource, sample, scale, 8, 0x000000FFu,
                                                      0xFFFFFFFFu, effectiveResourceType);
            if (!image.isNull()) preview.images.push_back(image);
        }
        const QImage map = buildFontGlyphMap(resource, effectiveResourceType);
        if (!map.isNull()) preview.images.push_back(map);
    }
    if (valid && type == QStringLiteral("PE_MODULE")) {
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
        if (item->childCount() > 0) {
            // Expandable container resources are navigation nodes.  Showing their
            // raw bytes as a fallback is misleading when an Opener already
            // exposes the contained payloads below them.
            preview.error.clear();
            preview.text.clear();
            preview.rawDump = false;
        } else {
            preview.text = formatRawDump(resource.payload());
            preview.rawDump = true;
        }
    }
    currentPreview_ = preview;
    if (!preview.images.isEmpty()) contentArea_->setWidgetResizable(false);
    const bool isFontPreview = isFontResource && preview.images.size() >= 5;
    for (int i = 0; i < preview.images.size(); ++i) {
        if (isFontPreview && i < 4) {
            auto* scaleLabel = new QLabel(QStringLiteral("%1x").arg(i + 1), contentWidget_);
            scaleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            QFont labelFont = scaleLabel->font();
            labelFont.setBold(true);
            scaleLabel->setFont(labelFont);
            layout->addWidget(scaleLabel, 0, Qt::AlignLeft);
        } else if (isFontPreview && i == 4) {
            auto* mapLabel = new QLabel(QStringLiteral("Glyph map"), contentWidget_);
            mapLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            QFont labelFont = mapLabel->font();
            labelFont.setBold(true);
            mapLabel->setFont(labelFont);
            layout->addWidget(mapLabel, 0, Qt::AlignLeft);
        }
        auto* picture = new QLabel(contentWidget_);
        picture->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        picture->setPixmap(QPixmap::fromImage(preview.images[i]));
        picture->setFixedSize(preview.images[i].size());
        picture->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        layout->addWidget(picture, 0, Qt::AlignTop | Qt::AlignLeft);
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
        const QString ext = pearegui::originalExtension(type, resource.payload());
        exportOriginalAction_->setText(QStringLiteral("Export original (%1)").arg(ext));
        exportOriginalAction_->setEnabled(true);
    }
    exportConvertedMenu_->clear();
    directConvertedExtension_.clear();
    QStringList extensions = valid ? resource.convertedExtensions() : QStringList();
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
    const QString ext = pearegui::originalExtension(context.type, payload);
    const QString base = pearegui::sanitizeFileName(QFileInfo(currentFilePath_).completeBaseName() +
                                                    QStringLiteral("_") + context.type +
                                                    QStringLiteral("_") + context.identifier);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export original resource"), QDir::home().filePath(base + ext),
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
        resource.isValid() ? resource.convert(extension) : QVector<QByteArray>();
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
    if (!currentSession_.isOpen() || currentFilePath_.isEmpty()) return;
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
    for (size_t folder = 0; folder < currentSession_.folderCount(); ++folder) {
        const QString folderName = pearegui::sanitizeFileName(currentSession_.folderType(folder));
        const QString target = QDir(root).filePath(folderName);
        QDir().mkpath(target);
        for (size_t index = 0; index < currentSession_.resourceCount(folder); ++index) {
            pearegui::Resource resource = currentSession_.openResource(folder, index);
            if (!resource.isValid()) {
                ++failed;
                continue;
            }
            const auto context = resource.context();
            const QByteArray payload = resource.payload();
            const QString path = pearegui::uniquePath(
                QDir(target).filePath(pearegui::sanitizeFileName(context.identifier) +
                                      pearegui::originalExtension(context.type, payload)));
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly) || f.write(payload) != payload.size()) {
                ++failed;
                continue;
            }
            ++exported;
        }
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
