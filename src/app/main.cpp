#include "MainWindow.h"

#include <QApplication>
#include <QAction>
#include <QByteArray>
#include <QDialogButtonBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QEventLoop>
#include <QEvent>
#include <QFont>
#include <QFileInfo>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPixmap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QToolBar>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>

#include <cstdio>

static bool isPreviewModeRequested(int argc, char* argv[])
{
    if (argc < 2) return false;
    const QString mode = QString::fromLocal8Bit(argv[1]);
    return mode == QStringLiteral("--preview-resource");
}

static bool isWriterTestRequested(int argc, char* argv[])
{
    return argc >= 2 && QString::fromLocal8Bit(argv[1]) ==
        QStringLiteral("--gui-writer-test");
}

static bool isStartupIconTestRequested(int argc, char* argv[])
{
    return argc >= 2 && QString::fromLocal8Bit(argv[1]) ==
        QStringLiteral("--startup-icons-test");
}

static bool isFolderOpenTestRequested(int argc, char* argv[])
{
    return argc >= 2 && QString::fromLocal8Bit(argv[1]) ==
        QStringLiteral("--folder-open-test");
}

static bool isFileAssociationTestRequested(int argc, char* argv[])
{
    return argc >= 2 && QString::fromLocal8Bit(argv[1]) ==
        QStringLiteral("--file-associations-test");
}

static bool isCredentialTestRequested(int argc, char* argv[])
{
    return argc >= 2 && QString::fromLocal8Bit(argv[1]) ==
        QStringLiteral("--gui-credential-test");
}

class CredentialDialogObserver final : public QObject {
public:
    int choicesShown = 0;
    int warningsShown = 0;
    int keyDialogsShown = 0;
    int passwordDialogsShown = 0;
    int passphraseDialogsShown = 0;

    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() != QEvent::Show) return false;
        if (object->objectName() == QStringLiteral("credentialChoiceDialog")) ++choicesShown;
        else if (object->objectName() == QStringLiteral("credentialWarningDialog")) ++warningsShown;
        else if (qobject_cast<QFileDialog*>(object)) ++keyDialogsShown;
        else if (object->objectName() == QStringLiteral("credentialKeyPassphraseDialog")) ++passphraseDialogsShown;
        else if (qobject_cast<QInputDialog*>(object)) ++passwordDialogsShown;
        return false;
    }
};

static int runCredentialTest(MainWindow& window, QApplication& app, const QString& mode)
{
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
    const QJsonDocument document = QJsonDocument::fromJson(input.read(16384));
    const QJsonObject options = document.object();
    const QString archive = options.value(QStringLiteral("archive")).toString();
    const QString keyPath = options.value(QStringLiteral("key_file")).toString();
    const QString wrongKeyPath = options.value(QStringLiteral("wrong_key_file")).toString();
    const QString resourcePath = options.value(QStringLiteral("resource_path"))
        .toString(QStringLiteral("C:/PeareInput/sample.txt"));
    const QByteArray expectedHash = options.value(QStringLiteral("expected_sha256"))
        .toString(QStringLiteral("931b99a0455cb56c50642cada2e5048efda927f8bb4e861a1ed9c0d2b67ce6ff")).toLatin1();
    QString password = options.value(QStringLiteral("password")).toString();
    QString passphrase = options.value(QStringLiteral("passphrase")).toString();
    const bool encryptedKey = mode == QStringLiteral("encrypted-key") ||
        mode == QStringLiteral("encrypted-key-retry") || mode == QStringLiteral("encrypted-key-kdf-limit");
    if (archive.isEmpty() || keyPath.isEmpty() || password.isEmpty() ||
        (mode != QStringLiteral("key") && mode != QStringLiteral("password") &&
         mode != QStringLiteral("key-only") && mode != QStringLiteral("password-only") &&
         mode != QStringLiteral("cancel-retry") && mode != QStringLiteral("wrong-key-retry") &&
         mode != QStringLiteral("key-to-password") && mode != QStringLiteral("password-to-key") &&
         !encryptedKey) || (encryptedKey && passphrase.isEmpty()) ||
        ((mode == QStringLiteral("wrong-key-retry") || mode == QStringLiteral("key-to-password")) &&
         wrongKeyPath.isEmpty())) return 2;

    if (mode == QStringLiteral("key-only") || mode == QStringLiteral("password-only")) {
        peare_opener_handle opener = nullptr;
        if (peare_opener_create(&opener) != PEARE_STATUS_OK || !opener) return 3;
        const QByteArray path = archive.toUtf8();
        const peare_status status = peare_opener_open_file(opener, path.constData());
        peare_opener_destroy(opener);
        const peare_status expected = mode == QStringLiteral("key-only")
            ? PEARE_STATUS_PRIVATE_KEY_REQUIRED : PEARE_STATUS_PASSWORD_REQUIRED;
        if (status != expected) {
            std::fprintf(stderr, "Unexpected single-method credential status.\n");
            return 3;
        }
    }

    int choices = 0, keyDialogs = 0, passwordDialogs = 0, passphraseDialogs = 0, warnings = 0, cancellations = 0;
    bool unexpected = false;
    QPointer<QFileDialog> currentFileDialog;
    QString selectedKeyPath;
    CredentialDialogObserver observer;
    app.installEventFilter(&observer);
    QTimer driver;
    driver.setInterval(20);
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        QWidget* active = QApplication::activeModalWidget();
        if (!active) return;
        if (unexpected) {
            if (QDialog* dialog = qobject_cast<QDialog*>(active)) dialog->reject();
            return;
        }
        if (QFileDialog* file = qobject_cast<QFileDialog*>(active)) {
            if (currentFileDialog != file) {
                currentFileDialog = file;
                ++keyDialogs;
                if (keyDialogs > 8) { unexpected = true; file->reject(); return; }
                if (mode == QStringLiteral("cancel-retry") && cancellations == 0) {
                    ++cancellations;
                    QDialogButtonBox* buttons = file->findChild<QDialogButtonBox*>();
                    QPushButton* cancel = buttons ? buttons->button(QDialogButtonBox::Cancel) : nullptr;
                    if (!cancel) { unexpected = true; file->reject(); return; }
                    cancel->click();
                    return;
                }
                selectedKeyPath = (mode == QStringLiteral("wrong-key-retry") ||
                                   mode == QStringLiteral("key-to-password")) && keyDialogs == 1
                    ? wrongKeyPath : keyPath;
                file->setDirectory(QFileInfo(selectedKeyPath).absolutePath());
            }
            QFileSystemModel* model = file->findChild<QFileSystemModel*>(QStringLiteral("qt_filesystem_model"));
            QLineEdit* name = file->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
            QDialogButtonBox* buttons = file->findChild<QDialogButtonBox*>();
            QPushButton* open = buttons ? buttons->button(QDialogButtonBox::Open) : nullptr;
            if (!model || !name || !open) { unexpected = true; file->reject(); return; }
            if (!model->index(QFileInfo(selectedKeyPath).absoluteFilePath()).isValid()) return;
            file->selectFile(QFileInfo(selectedKeyPath).fileName());
            name->setText(QFileInfo(selectedKeyPath).fileName());
            const QStringList selected = file->selectedFiles();
            if (open->isEnabled() && selected.size() == 1 &&
                QFileInfo(selected.first()).absoluteFilePath() == QFileInfo(selectedKeyPath).absoluteFilePath())
                open->click();
            return;
        }
        if (QInputDialog* dialog = qobject_cast<QInputDialog*>(active)) {
            if (dialog->objectName() == QStringLiteral("credentialKeyPassphraseDialog")) {
                ++passphraseDialogs;
                const bool invalid = dialog->labelText().contains(QStringLiteral("incorrect"), Qt::CaseInsensitive);
                if (!encryptedKey || passphraseDialogs > 2 ||
                    dialog->textEchoMode() != QLineEdit::Password ||
                    invalid != (passphraseDialogs == 2) ||
                    (passphraseDialogs == 2 && mode != QStringLiteral("encrypted-key-retry"))) {
                    unexpected = true; dialog->reject(); return;
                }
                dialog->setTextValue(mode == QStringLiteral("encrypted-key-retry") && passphraseDialogs == 1
                                     ? QStringLiteral("incorrect-test-passphrase") : passphrase);
                dialog->accept();
                return;
            }
            ++passwordDialogs;
            if (passwordDialogs > 8) { unexpected = true; dialog->reject(); return; }
            if (mode != QStringLiteral("password") && mode != QStringLiteral("password-only") &&
                mode != QStringLiteral("key-to-password") &&
                mode != QStringLiteral("password-to-key")) { unexpected = true; dialog->reject(); return; }
            dialog->setTextValue(mode == QStringLiteral("password-to-key")
                                 ? QStringLiteral("incorrect-test-password") : password);
            dialog->accept();
            return;
        }
        if (QDialog* dialog = qobject_cast<QDialog*>(active)) {
            if (dialog->objectName() == QStringLiteral("credentialChoiceDialog")) {
                const bool choosePassword = mode == QStringLiteral("password") ||
                    (mode == QStringLiteral("password-to-key") && choices == 0) ||
                    (mode == QStringLiteral("key-to-password") && choices == 1);
                const QString wanted = choosePassword
                    ? QStringLiteral("credentialPasswordButton") : QStringLiteral("credentialPrivateKeyButton");
                QPushButton* button = dialog->findChild<QPushButton*>(wanted);
                ++choices;
                if (!button || choices > 8) { unexpected = true; dialog->reject(); return; }
                button->click();
                return;
            }
            QLabel* label = dialog->findChild<QLabel*>(QStringLiteral("credentialWarningText"));
            const bool keyWarning = (mode == QStringLiteral("wrong-key-retry") ||
                                     mode == QStringLiteral("key-to-password")) &&
                label && label->text().contains(QStringLiteral("private key is invalid"), Qt::CaseInsensitive);
            const bool passwordWarning = mode == QStringLiteral("password-to-key") &&
                label && label->text().contains(QStringLiteral("password is incorrect"), Qt::CaseInsensitive);
            const bool kdfWarning = mode == QStringLiteral("encrypted-key-kdf-limit") &&
                label && label->text() == QStringLiteral("The private key derivation work limit was exceeded.");
            if (dialog->objectName() == QStringLiteral("credentialWarningDialog") &&
                (keyWarning || passwordWarning || kdfWarning)) {
                ++warnings;
                QDialogButtonBox* box = dialog->findChild<QDialogButtonBox*>(QStringLiteral("credentialWarningButtons"));
                QPushButton* ok = box ? box->button(QDialogButtonBox::Ok) : nullptr;
                if (!ok || warnings > 8) { unexpected = true; dialog->reject(); return; }
                ok->click();
                return;
            }
            unexpected = true;
            dialog->reject();
            return;
        }
        unexpected = true;
        if (QDialog* dialog = qobject_cast<QDialog*>(active)) dialog->reject();
    });
    window.resize(1280, 800);
    window.show();
    driver.start();
    window.openFilePath(archive);
    app.processEvents();
    if (mode == QStringLiteral("encrypted-key-kdf-limit")) {
        const bool selected = window.navigateToResource(resourcePath);
        app.processEvents();
        driver.stop();
        password.fill(QChar(0));
        passphrase.fill(QChar(0));
        if (selected || unexpected || choices != 0 || keyDialogs != 1 || passwordDialogs != 0 ||
            passphraseDialogs != 1 || warnings != 1 || observer.choicesShown != choices ||
            observer.keyDialogsShown != keyDialogs || observer.passwordDialogsShown != passwordDialogs ||
            observer.passphraseDialogsShown != passphraseDialogs || observer.warningsShown != warnings) {
            std::fprintf(stderr, "KDF limit dialog sequence did not match: choices=%d keys=%d passwords=%d passphrases=%d warnings=%d unexpected=%d.\n",
                         choices, keyDialogs, passwordDialogs, passphraseDialogs, warnings, unexpected ? 1 : 0);
            return 3;
        }
        std::printf("GUI credential scenario passed: %s.\n", mode.toUtf8().constData());
        return 0;
    }
    if (mode == QStringLiteral("cancel-retry")) {
        if (cancellations != 1 || window.navigateToResource(resourcePath)) {
            std::fprintf(stderr, "Credential cancellation was not respected.\n");
            return 3;
        }
        window.openFilePath(archive);
        app.processEvents();
    }
    const int dialogsBeforeNavigation = choices + keyDialogs + passwordDialogs + passphraseDialogs + warnings;
    const bool selected = window.navigateToResource(resourcePath);
    app.processEvents();
    const int dialogsAfterNavigation = choices + keyDialogs + passwordDialogs + passphraseDialogs + warnings;
    driver.stop();
    password.fill(QChar(0));
    passphrase.fill(QChar(0));
    OffscreenPreviewDiagnostics diagnostics;
    if (!selected || !window.inspectOffscreenPreview(&diagnostics) || !diagnostics.leaf ||
        diagnostics.payloadSha256 != expectedHash ||
        !diagnostics.duplicatePaths.isEmpty() || unexpected || dialogsBeforeNavigation != dialogsAfterNavigation) {
        std::fprintf(stderr, "Credential navigation or payload verification failed: choices=%d keys=%d passwords=%d warnings=%d unexpected=%d.\n",
                     choices, keyDialogs, passwordDialogs, warnings, unexpected ? 1 : 0);
        return 3;
    }
    const bool valid = encryptedKey
        ? choices == 0 && keyDialogs == 1 && passwordDialogs == 0 && warnings == 0 &&
          passphraseDialogs == (mode == QStringLiteral("encrypted-key-retry") ? 2 : 1)
        : mode == QStringLiteral("key-only")
        ? choices == 0 && keyDialogs == 1 && passwordDialogs == 0
        : mode == QStringLiteral("password-only")
        ? choices == 0 && passwordDialogs == 1 && keyDialogs == 0
        : mode == QStringLiteral("password")
        ? choices == 1 && passwordDialogs == 1 && keyDialogs == 0
        : mode == QStringLiteral("key") ? choices == 1 && keyDialogs == 1 && passwordDialogs == 0
        : mode == QStringLiteral("cancel-retry") ? choices == 2 && keyDialogs == 2 && cancellations == 1
        : mode == QStringLiteral("wrong-key-retry") ? choices == 2 && keyDialogs == 2 && warnings == 1
        : choices == 2 && keyDialogs == 1 && passwordDialogs == 1 && warnings == 1;
    if (!valid || observer.choicesShown != choices || observer.keyDialogsShown != keyDialogs ||
        observer.passwordDialogsShown != passwordDialogs || observer.passphraseDialogsShown != passphraseDialogs ||
        observer.warningsShown != warnings) {
        std::fprintf(stderr, "Credential dialog sequence did not match: choices=%d keys=%d passwords=%d warnings=%d cancellations=%d.\n",
                     choices, keyDialogs, passwordDialogs, warnings, cancellations);
        return 3;
    }
    std::printf("GUI credential scenario passed: %s.\n", mode.toUtf8().constData());
    return 0;
}

static int runFileAssociationTest(MainWindow& window, QApplication& app,
                                  const QString& screenshotPath)
{
    QAction* action = window.findChild<QAction*>(QStringLiteral("fileAssociationsAction"));
    QMenu* settingsMenu = nullptr;
    for (QAction* menuAction : window.menuBar()->actions()) {
        if (menuAction->menu() && menuAction->text().remove(QLatin1Char('&')) ==
                QStringLiteral("Settings")) {
            settingsMenu = menuAction->menu();
            break;
        }
    }
    if (!action || !settingsMenu || !settingsMenu->actions().contains(action) ||
        window.findChild<QToolBar*>(QStringLiteral("mainToolbar"))) {
        std::fprintf(stderr, "File associations action is not in Settings or a toolbar remains.\n");
        return 3;
    }
    QPixmap screenshot;
    int categories = 0;
    int extensions = 0;
    QTimer::singleShot(0, &window, [&screenshot, &categories, &extensions] {
        QDialog* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        screenshot = dialog->grab();
        const QListWidget* categoryList = dialog->findChild<QListWidget*>(
            QStringLiteral("associationCategories"));
        categories = categoryList ? categoryList->count() : 0;
        for (const QListWidget* list : dialog->findChildren<QListWidget*>())
            if (list->objectName() == QStringLiteral("associationExtensions"))
                extensions += list->count();
        dialog->accept();
    });
    action->trigger();
    if (screenshot.isNull() || !screenshot.save(screenshotPath, "PNG") ||
        categories != 10 || extensions < 100) {
        std::fprintf(stderr, "File association dialog check failed: %d categories, %d extension choices.\n",
            categories, extensions);
        return 3;
    }
    std::printf("File association dialog passed: %d categories, %d extension choices.\n",
        categories, extensions);
    return 0;
}

static int runPreview(MainWindow& window, QApplication& app, const QString& sourcePath,
                      const QString& resourcePath, const QString& screenshotPath)
{
    window.resize(1280, 800);
    window.show();
    window.openFilePath(sourcePath, QString::fromUtf8(qgetenv("PEARE_PREVIEW_PASSWORD")));
    app.processEvents();

    const bool selected = resourcePath.isEmpty()
        ? window.navigateToFirstLeafResource()
        : window.navigateToResource(resourcePath);
    if (!selected) {
        std::fprintf(stderr, "Resource path was not found in the Peare navigation tree.\n");
        return 3;
    }
    app.processEvents();

    QEventLoop iconLoadLoop;
    bool iconLoadCompleted = false;
    window.startBackgroundIconLoading(QString(), [&iconLoadLoop, &iconLoadCompleted](size_t) {
        iconLoadCompleted = true;
        iconLoadLoop.quit();
    });
    QTimer::singleShot(30000, &iconLoadLoop, &QEventLoop::quit);
    iconLoadLoop.exec();
    if (!iconLoadCompleted) {
        std::fprintf(stderr, "Configured icon loading did not complete.\n");
        return 4;
    }

    const QWidget* preview = window.findChild<QWidget*>(QStringLiteral("previewContent"));
    if (!preview) {
        std::fprintf(stderr, "The Peare preview pane was not created.\n");
        return 3;
    }
    if (qgetenv("PEARE_PREVIEW_SCROLL_BOTTOM") == QByteArrayLiteral("1")) {
        QWidget* ancestor = preview->parentWidget();
        while (ancestor && !qobject_cast<QScrollArea*>(ancestor))
            ancestor = ancestor->parentWidget();
        if (QScrollArea* area = qobject_cast<QScrollArea*>(ancestor))
            area->verticalScrollBar()->setValue(area->verticalScrollBar()->maximum());
        app.processEvents();
    }

    int previewTextViews = 0;
    for (QPlainTextEdit* text : preview->findChildren<QPlainTextEdit*>()) {
        if (!text->toPlainText().trimmed().isEmpty()) ++previewTextViews;
    }

    int previewImages = 0;
    int previewTextLabels = 0;
    int previewGrids = 0;
    for (QLabel* label : preview->findChildren<QLabel*>()) {
        const QPixmap pixmap = label->property("pixmap").value<QPixmap>();
        if (!pixmap.isNull()) ++previewImages;
        if (!label->text().trimmed().isEmpty()) ++previewTextLabels;
        const QVariant expectedItems = label->property("previewGridExpectedItems");
        if (expectedItems.isValid()) {
            ++previewGrids;
            const int expected = expectedItems.toInt();
            const int pageCount = label->property("previewGridPageCount").toInt();
            const int pageSize = label->property("previewGridPageSize").toInt();
            auto* next = preview->findChild<QToolButton*>(
                QStringLiteral("fontGlyphMapNextButton"));
            auto* previous = preview->findChild<QToolButton*>(
                QStringLiteral("fontGlyphMapPreviousButton"));
            if (expected <= 0 || pageCount <= 0 || pageSize <= 0 || !next || !previous) {
                std::fprintf(stderr, "Preview glyph pagination controls are incomplete.\n");
                return 3;
            }
            for (int page = 0; page < pageCount; ++page) {
                const int cells = label->property("previewGridCellCount").toInt();
                const int rendered = label->property("previewGridRenderedCount").toInt();
                const int pageIndex = label->property("previewGridPageIndex").toInt();
                const int expectedPageCells = qMin(pageSize, expected - page * pageSize);
                if (pageIndex != page || cells != expectedPageCells || rendered < 0 ||
                    rendered > cells) {
                    std::fprintf(stderr,
                                 "Preview grid page %d is incomplete: expected %d cells, found %d cells and %d glyphs with visible pixels.\n",
                                 page + 1, expectedPageCells, cells, rendered);
                    return 3;
                }
                if (page + 1 < pageCount) {
                    next->click();
                    app.processEvents();
                }
            }
            while (label->property("previewGridPageIndex").toInt() > 0) {
                previous->click();
                app.processEvents();
            }
        }
    }

    if (previewImages == 0 && previewTextViews == 0 && previewTextLabels == 0) {
        std::fprintf(stderr, "The selected tree path produced no visible preview content.\n");
        return 3;
    }

    OffscreenPreviewDiagnostics diagnostics;
    const bool inspected = window.inspectOffscreenPreview(&diagnostics);
    QJsonArray duplicatePaths;
    for (const QString& path : diagnostics.duplicatePaths)
        duplicatePaths.append(path);
    QJsonObject inspection;
    inspection.insert(QStringLiteral("leaf"), diagnostics.leaf);
    inspection.insert(QStringLiteral("type"), diagnostics.selectedType);
    inspection.insert(QStringLiteral("size"), static_cast<qint64>(diagnostics.dataSize));
    inspection.insert(QStringLiteral("sha256"), QString::fromLatin1(diagnostics.payloadSha256));
    inspection.insert(QStringLiteral("preview_images"), diagnostics.previewImageCount);
    inspection.insert(QStringLiteral("preview_text_characters"), diagnostics.previewTextCharacters);
    inspection.insert(QStringLiteral("duplicate_paths"), duplicatePaths);
    inspection.insert(QStringLiteral("error"), diagnostics.error);
    std::printf("GUI_INSPECTION %s\n",
                QJsonDocument(inspection).toJson(QJsonDocument::Compact).constData());
    if (!inspected) {
        std::fprintf(stderr, "GUI inspection failed: %s\n",
                     diagnostics.error.toUtf8().constData());
        return 3;
    }

    const QPixmap screenshot = window.grab();
    if (screenshot.isNull() || screenshot.width() < 640 || screenshot.height() < 400 ||
        !screenshot.save(screenshotPath, "PNG")) {
        std::fprintf(stderr, "Could not save the GUI preview image.\n");
        return 4;
    }

    std::printf("Saved Peare GUI preview for '%s' (%d images, %d text views, %d text labels, %d complete grids).\n",
                resourcePath.toUtf8().constData(), previewImages,
                previewTextViews, previewTextLabels, previewGrids);
    return 0;
}

int main(int argc, char* argv[])
{
    const bool previewMode = isPreviewModeRequested(argc, argv);
    const bool writerTestMode = isWriterTestRequested(argc, argv);
    const bool startupIconTestMode = isStartupIconTestRequested(argc, argv);
    const bool folderOpenTestMode = isFolderOpenTestRequested(argc, argv);
    const bool fileAssociationTestMode = isFileAssociationTestRequested(argc, argv);
    const bool credentialTestMode = isCredentialTestRequested(argc, argv);
    if (previewMode && argc != 5) {
        std::fprintf(stderr,
                     "Usage: Peare.exe --preview-resource <source> <tree-path-or-empty> <preview.png>\n"
                     "An empty tree path selects the first leaf resource.\n");
        return 2;
    }
    if (writerTestMode && argc != 6) {
        std::fprintf(stderr,
                     "Usage: Peare.exe --gui-writer-test <image-copy> <scenario.json> <report.json> <screenshot.png>\n");
        return 2;
    }
    if (writerTestMode && qgetenv("PEARE_ENABLE_GUI_WRITER_TEST") != QByteArrayLiteral("1")) {
        std::fprintf(stderr, "Set PEARE_ENABLE_GUI_WRITER_TEST=1 to enable GUI writer tests.\n");
        return 2;
    }
    if (previewMode || writerTestMode || startupIconTestMode || folderOpenTestMode ||
        fileAssociationTestMode || credentialTestMode) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
#ifdef Q_OS_WIN
        if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
            const QString fontDirectory = QDir::fromNativeSeparators(
                QString::fromLocal8Bit(qgetenv("WINDIR"))) + QStringLiteral("/Fonts");
            if (QDir(fontDirectory).exists())
                qputenv("QT_QPA_FONTDIR", QDir::toNativeSeparators(fontDirectory).toLocal8Bit());
        }
#endif
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Peare"));
    QApplication::setApplicationDisplayName(QStringLiteral("Peare"));
    QApplication::setOrganizationName(QStringLiteral("Raul Merelli"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/peare/icon.ico")));
#ifdef Q_OS_WIN
    if (previewMode || writerTestMode || startupIconTestMode || folderOpenTestMode ||
        fileAssociationTestMode || credentialTestMode)
        app.setFont(QFont(QStringLiteral("Segoe UI"), 9));
#endif

    MainWindow window;
    if (credentialTestMode) {
        if (argc != 3) return 2;
        return runCredentialTest(window, app, app.arguments().at(2));
    }
    if (fileAssociationTestMode) {
        if (argc != 3) {
            std::fprintf(stderr,
                         "Usage: Peare.exe --file-associations-test <screenshot.png>\n");
            return 2;
        }
        return runFileAssociationTest(window, app, app.arguments().at(2));
    }
    if (previewMode) {
        return runPreview(window, app, app.arguments().at(2), app.arguments().at(3),
                          app.arguments().at(4));
    }
    if (writerTestMode) {
        QString error;
        if (!window.runOffscreenWriterScenario(
                app.arguments().at(2), app.arguments().at(3), app.arguments().at(4),
                app.arguments().at(5), &error)) {
            std::fprintf(stderr, "GUI writer test failed: %s\n",
                         error.toUtf8().constData());
            return 3;
        }
        std::printf("GUI writer scenario passed.\n");
        return 0;
    }
    if (folderOpenTestMode) {
        if (argc != 5) {
            std::fprintf(stderr,
                         "Usage: Peare.exe --folder-open-test <folder> <relative-file> <screenshot.png>\n");
            return 2;
        }
        QString error;
        if (!window.runOffscreenFolderOpenScenario(app.arguments().at(2),
                app.arguments().at(3), app.arguments().at(4), &error)) {
            std::fprintf(stderr, "Folder open test failed: %s\n", error.toUtf8().constData());
            return 3;
        }
        std::printf("Folder file opened through Peare.\n");
        return 0;
    }
    if (startupIconTestMode && argc != 3) {
        std::fprintf(stderr,
                     "Usage: Peare.exe --startup-icons-test <settings.ini>\n");
        return 2;
    }
    if (startupIconTestMode) {
        window.show();
        const QString settingsPath = app.arguments().at(2);
        QTimer::singleShot(100, &window, [&window, &app, settingsPath] {
            window.startBackgroundIconLoading(settingsPath, [&app](size_t loadedCount) {
                std::printf("Loaded configured icons: %llu\n",
                            static_cast<unsigned long long>(loadedCount));
                app.exit(loadedCount > 0 ? 0 : 5);
            });
        });
        return app.exec();
    }

    for (int index = 1; index < app.arguments().size(); ++index) {
        const QString argument = app.arguments().at(index);
        if (!argument.startsWith(QLatin1Char('-')) && QFileInfo::exists(argument)) {
            window.openFilePath(argument);
            break;
        }
    }

    window.show();
    QTimer::singleShot(100, &window, [&window] {
        window.startBackgroundIconLoading();
    });
    return app.exec();
}
