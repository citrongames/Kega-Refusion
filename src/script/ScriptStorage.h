#pragma once

#include <QByteArrayView>
#include <QString>
#include <memory>

class ScriptStorage final {
public:
    static constexpr int MaxFileBytes = 1024 * 1024;
    static constexpr int MaxNameBytes = 96;

    ScriptStorage(QString root, QString path, QString identity);
    ~ScriptStorage();

    ScriptStorage(const ScriptStorage&) = delete;
    ScriptStorage& operator=(const ScriptStorage&) = delete;

    static bool validName(QByteArrayView name);
    struct Submission {
        qint64 id = 0;
        const char* error = nullptr;
    };
    struct Result {
        const char* error = nullptr;
        QByteArrayView data;
        bool written = false;
        bool ready = false;
    };

    Submission read(QByteArrayView name, int offset, int length);
    Submission write(QByteArrayView name, QByteArrayView data);
    // The data view stays valid until the completed request is released.
    Result result(qint64 id) const;
    void release(qint64 id);

private:
    struct State;
    std::unique_ptr<State> state_;
    Submission submit(QByteArrayView name, QByteArrayView data, bool write, int offset, int length);
};
