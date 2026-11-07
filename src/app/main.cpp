#include "MainWindow.h"

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QFileInfo>
#include <QIcon>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
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
    if (previewMode || writerTestMode || startupIconTestMode) {
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
    if (previewMode || writerTestMode || startupIconTestMode)
        app.setFont(QFont(QStringLiteral("Segoe UI"), 9));
#endif

    MainWindow window;
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
