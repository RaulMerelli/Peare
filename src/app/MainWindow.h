#pragma once

#include "PeareApi.h"
#include "SettingsIcons.h"
#include "peare/peare_writer.h"

#include <QByteArray>
#include <QMainWindow>
#include <QString>
#include <QStringList>

#include <memory>
#include <functional>
#include <vector>

class QTreeWidgetItem;
class QDragEnterEvent;
class QDropEvent;
class QPoint;

class QAction;
class QLabel;
class QLineEdit;
class QMenu;
class QScrollArea;
class QSplitter;
class QStackedWidget;
class QThread;
class QToolButton;
class QTreeWidget;
class QWidget;
class QTimer;

struct OffscreenPreviewDiagnostics {
    QString selectedType;
    quint64 dataSize = 0;
    QByteArray payloadSha256;
    int previewImageCount = 0;
    int previewTextCharacters = 0;
    bool leaf = false;
    QStringList duplicatePaths;
    QString error;
};

class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    void startBackgroundIconLoading(
        const QString& settingsPath = QString(),
        std::function<void(size_t)> completion = {});
    void openFilePath(const QString& path);
    void openFilePath(const QString& path, const QString& password);
    bool navigateToResource(const QString& path);
    bool navigateToFirstLeafResource();
    bool inspectOffscreenPreview(OffscreenPreviewDiagnostics* diagnostics);
    bool runOffscreenWriterScenario(const QString& imagePath,
                                    const QString& scenarioPath,
                                    const QString& reportPath,
                                    const QString& screenshotPath,
                                    QString* error);
    bool runOffscreenFolderOpenScenario(const QString& directoryPath,
                                        const QString& relativeFilePath,
                                        const QString& screenshotPath,
                                        QString* error);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    struct WriterViewState {
        QString locationPath;
        QString selectionPath;
        QStringList expandedPaths;
    };

    void buildMenus();
    void buildCentralUi();
    void refreshConfiguredIcons();
    void setDetailsMode(bool enabled);
    void refreshDetailsView();
    void scheduleDetailsIconLoading();
    void loadNextDetailsIcon();
    void scheduleTreeIconLoading();
    void loadNextTreeIcon();
    void navigateDetailsTo(QTreeWidgetItem* location, bool saveHistory = true);
    void navigateDetailsBack();
    void navigateDetailsForward();
    void navigateDetailsUp();
    void navigateDetailsAddress();
    void updateNavigationUi();
    QString detailsPath(QTreeWidgetItem* item) const;
    QTreeWidgetItem* findDetailsPath(const QString& path) const;
    void selectResourceItem(QTreeWidgetItem* item);
    QTreeWidgetItem* detailsSourceItem(QTreeWidgetItem* row) const;
    void selectDetailsSource(QTreeWidgetItem* source);
    void openFile();
    void openFolder();
    void openFolderPath(const QString& path);
    void showFileAssociationSettings();
    void addFilesToNewContainer();
    void addFilesToContainer(const QStringList& sources = QStringList(),
                             const QString& destination = QString());
    void showAbout();
    void showModuleSummary(const QString& filePath,
                           const QString& initialPassword = QString());
    void showSelectedResource();
    void populateFromSession(pearegui::Session* session, QTreeWidgetItem* parentBase);
    void expandContainerItem(QTreeWidgetItem* item);
    void populateLocalDirectory(const QString& path, QTreeWidgetItem* parent);
    void refreshLocalDirectory(QTreeWidgetItem* parent);
    void showLocalFile(const QString& path);
    bool showImageFile(const QString& path);
    void showItemContextMenu(QWidget* view, const QPoint& position, QTreeWidgetItem* item);
    QString localMutationDirectory(QTreeWidgetItem* selected) const;
    void createLocalFile(bool directory);
    void createLocalLink(bool junction);
    void renameLocalEntry(QTreeWidgetItem* item);
    void deleteLocalEntry(QTreeWidgetItem* item);
    QString writerEntryPath(QTreeWidgetItem* item) const;
    QString writerDestinationPath(QTreeWidgetItem* item) const;
    WriterViewState captureWriterViewState() const;
    void refreshWriterView(const WriterViewState& state,
                           const QString& selectionPath = QString());
    bool beginWriterUpdate(peare_writer_handle writer);
    void showWriterError(const QString& title, const QString& message);
    void reportOffscreenWriterStage(const QString& stage);
    void createWriterEntry(bool directory);
    void renameWriterEntry(QTreeWidgetItem* item);
    void deleteWriterEntry(QTreeWidgetItem* item);
    void replaceWriterEntry(QTreeWidgetItem* item);
    pearegui::Session* sessionOf(QTreeWidgetItem* item) const;
    void exportSelectedOriginal();
    void exportSelectedConverted(const QString& extension);
    void exportAllResources();
    void clearContent();
    QImage buildFontGlyphMap(const pearegui::Resource& resource, const QString& resourceType,
                             const QVector<quint32>& mapValues, bool usesGlyphIndexes,
                             int pageIndex,
                             int* cellCount = nullptr, int* renderedGlyphCount = nullptr,
                             int* inkedGlyphCount = nullptr,
                             int* columnCount = nullptr, int* imageWidth = nullptr,
                             int* imageHeight = nullptr, int* viewportWidth = nullptr,
                             int* viewportHeight = nullptr) const;
    void showFontGlyphMapPage();

    QWidget* navigationPane_ = nullptr;
    QWidget* navigationBar_ = nullptr;
    QToolButton* backButton_ = nullptr;
    QToolButton* forwardButton_ = nullptr;
    QToolButton* upButton_ = nullptr;
    QLineEdit* addressEdit_ = nullptr;
    QStackedWidget* navigationStack_ = nullptr;
    QTreeWidget* treeView_ = nullptr;
    QTreeWidget* detailsView_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QWidget* previewPane_ = nullptr;
    QTimer* detailsIconTimer_ = nullptr;
    QTimer* treeIconTimer_ = nullptr;
    QLabel* messageLabel_ = nullptr;
    QScrollArea* contentArea_ = nullptr;
    QWidget* contentWidget_ = nullptr;
    QLabel* fontGlyphMapLabel_ = nullptr;
    QLabel* fontGlyphMapImageLabel_ = nullptr;
    QToolButton* fontGlyphMapPreviousButton_ = nullptr;
    QToolButton* fontGlyphMapNextButton_ = nullptr;

    QMenu* resourceMenu_ = nullptr;
    QAction* exportOriginalAction_ = nullptr;
    QAction* exportConvertedAction_ = nullptr;
    QMenu* exportConvertedMenu_ = nullptr;
    QString directConvertedExtension_;
    QAction* exportAllAction_ = nullptr;
    QAction* addFilesAction_ = nullptr;
    QAction* showNavigationAction_ = nullptr;
    QAction* showPreviewAction_ = nullptr;
    QAction* newFolderAction_ = nullptr;
    QAction* newFileAction_ = nullptr;
    QAction* newSymbolicLinkAction_ = nullptr;
    QAction* newJunctionAction_ = nullptr;
    QAction* renameAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* replaceAction_ = nullptr;

    QString currentFilePath_;
    QString currentDirectory_;
    bool offscreenWriterScenario_ = false;
    std::function<void(const QString&)> offscreenWriterProgress_;
    int offscreenWriterTargetIndex_ = -1;
    QStringList offscreenWriterTextAnswers_;
    QStringList offscreenWriterFileAnswers_;
    QStringList offscreenWriterAddSources_;
    QString offscreenWriterAddDestination_;
    int offscreenWriterTextIndex_ = 0;
    int offscreenWriterFileIndex_ = 0;
    QString offscreenWriterError_;
    QVector<QByteArray> directResourcePayloads_;
    QString directResourceType_;
    bool folderMode_ = false;
    pearegui::Session currentSession_;
    // Nested container sessions, kept alive as long as their subtree exists.
    std::vector<std::unique_ptr<pearegui::Session>> childSessions_;
    pearegui::Preview currentPreview_;
    QByteArray fontGlyphMapPayload_;
    pearegui::ResourceContext fontGlyphMapContext_;
    QString fontGlyphMapResourceType_;
    QVector<quint32> fontGlyphMapValues_;
    bool fontGlyphMapUsesGlyphIndexes_ = false;
    int fontGlyphMapPage_ = 0;
    SettingsIcons settingsIcons_;
    QThread* iconLoadThread_ = nullptr;

    bool detailsMode_ = false;
    int detailsIconIndex_ = 0;
    size_t treeIconIndex_ = 0;
    std::vector<QTreeWidgetItem*> pendingEmbeddedIconItems_;
    QTreeWidgetItem* currentDetailsLocation_ = nullptr;
    std::vector<QTreeWidgetItem*> previousLocations_;
    std::vector<QTreeWidgetItem*> nextLocations_;
};
