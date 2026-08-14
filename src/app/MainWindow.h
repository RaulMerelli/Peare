#pragma once

#include "PeareApi.h"
#include "SettingsIcons.h"

#include <QMainWindow>
#include <QString>

#include <memory>
#include <vector>

class QTreeWidgetItem;

class QAction;
class QLabel;
class QLineEdit;
class QMenu;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class QTreeWidget;
class QWidget;
class QTimer;

class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void buildMenus();
    void buildCentralUi();
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
    QTreeWidgetItem* detailsSourceItem(QTreeWidgetItem* row) const;
    void selectDetailsSource(QTreeWidgetItem* source);
    void openFile();
    void showAbout();
    void showModuleSummary(const QString& filePath);
    void showSelectedResource();
    void populateFromSession(pearegui::Session* session, QTreeWidgetItem* parentBase);
    void expandContainerItem(QTreeWidgetItem* item);
    pearegui::Session* sessionOf(QTreeWidgetItem* item) const;
    void exportSelectedOriginal();
    void exportSelectedConverted(const QString& extension);
    void exportAllResources();
    void clearContent();
    QImage buildFontGlyphMap(const pearegui::Resource& resource, const QString& resourceType) const;

    QWidget* navigationPane_ = nullptr;
    QWidget* navigationBar_ = nullptr;
    QToolButton* backButton_ = nullptr;
    QToolButton* forwardButton_ = nullptr;
    QToolButton* upButton_ = nullptr;
    QLineEdit* addressEdit_ = nullptr;
    QStackedWidget* navigationStack_ = nullptr;
    QTreeWidget* treeView_ = nullptr;
    QTreeWidget* detailsView_ = nullptr;
    QTimer* detailsIconTimer_ = nullptr;
    QTimer* treeIconTimer_ = nullptr;
    QLabel* messageLabel_ = nullptr;
    QScrollArea* contentArea_ = nullptr;
    QWidget* contentWidget_ = nullptr;

    QMenu* resourceMenu_ = nullptr;
    QAction* exportOriginalAction_ = nullptr;
    QAction* exportConvertedAction_ = nullptr;
    QMenu* exportConvertedMenu_ = nullptr;
    QString directConvertedExtension_;
    QAction* exportAllAction_ = nullptr;

    QString currentFilePath_;
    pearegui::Session currentSession_;
    // Nested container sessions, kept alive as long as their subtree exists.
    std::vector<std::unique_ptr<pearegui::Session>> childSessions_;
    pearegui::Preview currentPreview_;
    SettingsIcons settingsIcons_;

    bool detailsMode_ = false;
    int detailsIconIndex_ = 0;
    size_t treeIconIndex_ = 0;
    std::vector<QTreeWidgetItem*> pendingEmbeddedIconItems_;
    QTreeWidgetItem* currentDetailsLocation_ = nullptr;
    std::vector<QTreeWidgetItem*> previousLocations_;
    std::vector<QTreeWidgetItem*> nextLocations_;
};
