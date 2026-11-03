#include "ui/CheatsDialog.h"
#include "ui/CheatSearchDialog.h"
#include "i18n/Localization.h"

#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QPushButton>
#include <QTableWidget>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}
void preview(QWidget& widget, const QString& path) {
    require(widget.grab().save(path), "could not save UI screenshot");
}
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    try {
        require(app.arguments().size() == 2, "expected screenshot output directory");
        const QString directory = app.arguments().at(1);
        require(QDir().mkpath(directory), "could not create screenshot directory");
        Localization::initialize(QDir::currentPath(), QDir::tempPath());

        const CheatList entries{
            {QStringLiteral("FF0000:0001+FF0001:0002"), QStringLiteral("Lives and score"), true},
            {QStringLiteral("FF0002:0003"), QStringLiteral("Disabled bonus"), false}};
        CheatsDialog editor(entries, Cheats::Platform::MegaDrive,
                            QStringLiteral("Sonic the Hedgehog"));
        editor.show();
        QApplication::processEvents();
        auto* exportButton = editor.findChild<QPushButton*>(QStringLiteral("exportChtButton"));
        auto* importButton = editor.findChild<QPushButton*>(QStringLiteral("importChtButton"));
        auto* cheatTable = editor.findChild<QTableWidget*>();
        require(exportButton && importButton && exportButton->isVisible() &&
                importButton->isVisible() && cheatTable && cheatTable->rowCount() == 2,
                "cheat editor controls are missing");
        require(exportButton->geometry().right() <= editor.width() &&
                importButton->geometry().right() <= editor.width(),
                "CHT actions overflow the default dialog width");
        preview(editor, QDir(directory).filePath(QStringLiteral("cheats-dialog.png")));

        CheatSearchDialog search;
        quint64 requestId = 0;
        QObject::connect(&search, &CheatSearchDialog::searchRequested, &search,
                         [&](quint64 id, int width, int comparison, quint32, bool restart) {
            require(width == 1 && comparison == 5 && restart,
                    "initial search request has wrong options");
            requestId = id;
        });
        search.show();
        QApplication::processEvents();
        auto* newSearch = search.findChild<QPushButton*>(QStringLiteral("cheatSearchNew"));
        auto* results = search.findChild<QTableWidget*>(QStringLiteral("cheatSearchResults"));
        auto* add = search.findChild<QPushButton*>(QStringLiteral("cheatSearchAdd"));
        require(newSearch && results && add && newSearch->isVisible() && add->isVisible(),
                "search controls are missing at default size");
        newSearch->click();
        require(requestId != 0, "search request was not emitted");
        const QByteArray response = R"({"error":"","count":3,"width":1,"base":16711680,"bigEndian":true,"results":[{"address":16711680,"previous":2,"value":3},{"address":16711681,"previous":7,"value":8},{"address":16711682,"previous":9,"value":10}]})";
        search.setResults(requestId, response);
        QApplication::processEvents();
        require(results->rowCount() == 3, "search results were not displayed");
        results->selectRow(1);
        QApplication::processEvents();
        require(add->isEnabled(), "selected search result cannot be added");
        require(add->geometry().right() <= search.width(),
                "search action overflows the default dialog width");
        preview(search, QDir(directory).filePath(QStringLiteral("cheat-search-dialog.png")));

        search.resetSearch();
        search.setResults(requestId, response);
        require(results->rowCount() == 0, "stale response restored reset search results");
        std::puts("cheat dialog visual previews and stale search response: passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cheat UI preview failed: %s\n", error.what());
        return 1;
    }
}
