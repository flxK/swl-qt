#include <QApplication>
#include "weblobbywindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    WebLobbyWindow webLobbyWindow;
    webLobbyWindow.showMaximized();

    return app.exec();
}
