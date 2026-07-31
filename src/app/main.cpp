#include "MainWindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Peare"));
    QApplication::setApplicationDisplayName(QStringLiteral("Peare"));
    QApplication::setOrganizationName(QStringLiteral("Raul Merelli"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/peare/icon.ico")));

    MainWindow window;
    window.show();

    return app.exec();
}

