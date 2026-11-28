#include "ui/BundledLuaFiles.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QDateTime>
#include <cassert>

static void put(const QString& path, const QByteArray& bytes) {
    assert(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile f(path); assert(f.open(QIODevice::WriteOnly)); assert(f.write(bytes) == bytes.size());
}
static QByteArray get(const QString& path) {
    QFile f(path); assert(f.open(QIODevice::ReadOnly)); return f.readAll();
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp; assert(temp.isValid());
    const QString root = temp.path(), source = root + "/source", data = root + "/data";
    const QString target = root + "/app", scripts = root + "/custom-scripts";
    put(source + "/scripts/demo.lua", "v1"); put(source + "/docs/guide.md", "guide1");
    assert(installBundledLuaFiles(target, scripts, data, source));
    assert(get(scripts + "/demo.lua") == "v1"); assert(get(target + "/docs/guide.md") == "guide1");
    const QString receipt = data + "/lua-bundle.json";
    const auto modified = QFileInfo(receipt).lastModified();
    const auto receiptBytes = get(receipt);
    assert(installBundledLuaFiles(target, scripts, data, source));
    assert(get(receipt) == receiptBytes && QFileInfo(receipt).lastModified() == modified);
    put(scripts + "/demo.lua", "personal edit");
    put(source + "/scripts/demo.lua", "v2"); put(source + "/docs/guide.md", "guide2");
    put(scripts + "/new.lua", "personal file"); put(source + "/scripts/new.lua", "new bundled file");
    assert(installBundledLuaFiles(target, scripts, data, source));
    assert(get(scripts + "/demo.lua") == "personal edit");
    assert(get(scripts + "/new.lua") == "personal file");
    assert(get(target + "/docs/guide.md") == "guide2");
    const QString moved = root + "/other-scripts";
    assert(installBundledLuaFiles(target, moved, data, source));
    assert(get(moved + "/demo.lua") == "v2"); assert(get(scripts + "/demo.lua") == "personal edit");
    const QString real = root + "/release";
    assert(installBundledLuaFiles(real, real + "/scripts", real + "/data"));
    for (const auto& name : {"docs/lua-api.md", "docs/lua-api.ru.md", "docs/lua-examples.md", "docs/lua-examples.ru.md", "scripts/01_Overlay.lua", "scripts/08_WindowControls.lua", "scripts/games/contra-hard-corps-lab.lua"})
        assert(get(real + "/" + name) == get(":/bundled-lua/" + QString::fromLatin1(name)));
    return 0;
}
