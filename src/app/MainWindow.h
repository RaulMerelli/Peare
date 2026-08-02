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
class QMenu;
class QScrollArea;
class QTreeWidget;
class QWidget;

class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void buildMenus();
    void buildCentralUi();
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

    QTreeWidget* treeView_ = nullptr;
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
};
