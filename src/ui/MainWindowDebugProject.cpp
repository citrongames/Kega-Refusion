#include "MainWindow.h"
#include "CpuDebuggerDialog.h"
#include "DebugWatchDialog.h"
#include "DebugInspectorWidgets.h"
#include "core/EmulationSession.h"
#include "i18n/Localization.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTimer>
#include <atomic>
#include <thread>

namespace {
QString text(const char* s) { return Localization::text(QString::fromUtf8(s)); }
}
// The worker owns only copied data and local QFile objects. GUI destruction joins
// its one explicit file operation; no worker ever dereferences a window.
struct DebugProjectOperation {
    QString path, error;
    DebugProject project;
    quint64 context=0, revision=0;
    bool save=false, success=false;
    std::atomic_bool finished{false};
    std::thread thread;
    ~DebugProjectOperation() { if (thread.joinable()) thread.join(); }
};

void MainWindow::ensureDebugWatch() {
    if (debugWatchDialog_) return;
    debugWatchDialog_=new DebugWatchDialog(this);
    debugWatchDialog_->setScriptEntries(scriptAnnotations_.watches);
    connect(debugWatchDialog_,&DebugWatchDialog::inspectionChanged,this,&MainWindow::scheduleInspection);
    connect(debugWatchDialog_,&DebugWatchDialog::entriesChanged,this,&MainWindow::debugProjectChanged);
    connect(debugWatchDialog_,&DebugWatchDialog::navigateRequested,this,
        [this](quint32 instance,quint32 domain,quint32 offset) {
            if (!session_.hasGame() || loading_ || achievementsHardcore_ || videoExporting_) return;
            showDebugMemory();
            debugMemoryDialog_->navigateTo(instance,domain,offset);
        });
}

void MainWindow::showDebugWatch() {
    if (loading_ || achievementsHardcore_ || videoExporting_) return;
    ensureDebugWatch();
    debugWatchDialog_->show();
    debugWatchDialog_->raise();
    debugWatchDialog_->activateWindow();
    scheduleInspection();
}

void MainWindow::debugProjectChanged() {
    if (debugProjectApplying_) return;
    ++debugProjectRevision_;
    debugProjectDirty_=true;
    if (debugProjectGame_.isEmpty()) debugProjectGame_=currentGameName_;
    updateDebugProjectActions();
}

void MainWindow::updateDebugProjectActions() {
    const bool pending=debugProjectOperation_ || debugProjectPending_ ||
        (cpuDebuggerDialog_ && cpuDebuggerDialog_->projectPending());
    if (debugProjectOpenAction_)
        debugProjectOpenAction_->setEnabled(!pending && cpuDebuggerBlockedReason().isEmpty());
    const bool canSave=!pending && !debugProjectGame_.isEmpty() && !loading_ &&
        !videoExporting_ && !achievementsHardcore_;
    if (debugProjectSaveAction_) debugProjectSaveAction_->setEnabled(canSave);
    if (debugProjectSaveAsAction_) debugProjectSaveAsAction_->setEnabled(canSave);
    if (debugWatchAction_) debugWatchAction_->setEnabled(
        (session_.hasGame() || !debugProjectGame_.isEmpty()) && !loading_ &&
        !videoExporting_ && !achievementsHardcore_);
    if (debugProjectStatus_) {
        QString name=debugProjectPath_.isEmpty()?text("Без файла"):QFileInfo(debugProjectPath_).fileName();
        if (debugProjectDirty_) name+=QStringLiteral(" *");
        debugProjectStatus_->setText(pending?text("Обработка проекта…"):name);
    }
    updateDebugMenuVisibility();
}

void MainWindow::clearDebugProjectForGame(const QString& path,const QString& contentName) {
    // Include the full archive member name, since distinct games may share a display title.
    if (debugProjectRom_!=path || debugProjectContent_!=contentName) {
        ++debugProjectContext_;
        debugProjectApplying_=true;
        debugProjectPending_.reset();
        if (cpuDebuggerDialog_) cpuDebuggerDialog_->clearProject();
        if (debugWatchDialog_) debugWatchDialog_->setEntries({});
        debugProjectPath_.clear();
        debugProjectDirty_=false;
        ++debugProjectRevision_;
        debugProjectApplying_=false;
    }
    debugProjectRom_=path;
    debugProjectContent_=contentName;
    debugProjectGame_=currentGameName_;
    updateDebugProjectActions();
    scheduleInspection();
}

void MainWindow::openDebugProject() {
    if (debugProjectOperation_ || debugProjectPending_ || !cpuDebuggerBlockedReason().isEmpty() ||
        (cpuDebuggerDialog_ && cpuDebuggerDialog_->projectPending())) return;
    const QString path=QFileDialog::getOpenFileName(this,text("Открыть проект отладки"),
        debugProjectPath_.isEmpty()?dataPath_:debugProjectPath_,
        text("Проекты отладки (*.krdbg);;Все файлы (*)"));
    if (!path.isEmpty()) startDebugProjectIo(path,false);
}

void MainWindow::saveDebugProjectFile(bool choosePath) {
    if (debugProjectOperation_ || debugProjectPending_ || debugProjectGame_.isEmpty() ||
        loading_ || achievementsHardcore_ || videoExporting_ ||
        (cpuDebuggerDialog_ && cpuDebuggerDialog_->projectPending())) return;
    const quint64 context=debugProjectContext_, revision=debugProjectRevision_;
    DebugProject project;
    if (cpuDebuggerDialog_) project=cpuDebuggerDialog_->exportProject();
    project.game=debugProjectGame_;
    if (debugWatchDialog_) project.watches=debugWatchDialog_->entries();
    QString path=debugProjectPath_;
    if (choosePath || path.isEmpty()) {
        QString stem=debugProjectGame_.left(80);
        stem.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")),QStringLiteral("_"));
        if (stem.isEmpty()) stem=QStringLiteral("debug");
        path=QFileDialog::getSaveFileName(this,text("Сохранить проект отладки"),
            path.isEmpty()?QDir(dataPath_).filePath(stem+QStringLiteral(".krdbg")):path,
            text("Проекты отладки (*.krdbg);;Все файлы (*)"));
        if (!path.isEmpty() && QFileInfo(path).suffix().isEmpty()) path+=QStringLiteral(".krdbg");
    }
    if (!path.isEmpty() && context==debugProjectContext_ && revision==debugProjectRevision_)
        startDebugProjectIo(path,true,std::move(project));
}

void MainWindow::startDebugProjectIo(const QString& path,bool save,DebugProject project) {
    if (debugProjectOperation_) return;
    auto job=std::make_shared<DebugProjectOperation>();
    job->path=path;job->save=save;job->project=std::move(project);
    job->context=debugProjectContext_;job->revision=debugProjectRevision_;
    try {
        job->thread=std::thread([operation=job.get()] {
            try {
                operation->success=operation->save
                    ? saveDebugProject(operation->path,operation->project,operation->error)
                    : loadDebugProject(operation->path,operation->project,operation->error);
            } catch (...) {
                operation->error=QStringLiteral("DEBUG_PROJECT_IO_FAILED");
            }
            operation->finished.store(true,std::memory_order_release);
        });
    } catch (...) {
        QMessageBox::warning(this,text("Проект отладки"),text("Не удалось начать обработку проекта."));
        return;
    }
    debugProjectOperation_=std::move(job);
    updateDebugProjectActions();
    QTimer::singleShot(100,this,&MainWindow::pollDebugProjectIo);
}

void MainWindow::pollDebugProjectIo() {
    if (!debugProjectOperation_) return;
    if (!debugProjectOperation_->finished.load(std::memory_order_acquire)) {
        QTimer::singleShot(100,this,&MainWindow::pollDebugProjectIo);
        return;
    }
    const auto job=std::move(debugProjectOperation_);
    if (!job->success) {
        QMessageBox::warning(this,text("Проект отладки"),
            job->error==QStringLiteral("DEBUG_PROJECT_IO_FAILED")
                ?text("Не удалось обработать файл проекта."):job->error);
    } else if (job->save) {
        if (job->context==debugProjectContext_) {
            debugProjectPath_=job->path;
            if (job->revision==debugProjectRevision_) debugProjectDirty_=false;
        }
    } else if (job->context==debugProjectContext_ && cpuDebuggerBlockedReason().isEmpty()) {
        if (job->revision!=debugProjectRevision_) {
            QMessageBox::warning(this,text("Проект отладки"),
                text("Проект изменился во время чтения файла. Откройте файл ещё раз."));
            updateDebugProjectActions();
            return;
        }
        if (job->project.game!=currentGameName_ &&
            QMessageBox::question(this,text("Проект отладки"),
                text("Проект создан для «%1». Применить его к «%2»?")
                    .arg(job->project.game,currentGameName_),
                QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) {
            updateDebugProjectActions();
            return;
        }
        // A modal confirmation can run events that change the loaded game.
        if (job->context!=debugProjectContext_ || job->revision!=debugProjectRevision_ ||
            !cpuDebuggerBlockedReason().isEmpty()) {
            updateDebugProjectActions();
            return;
        }
        ensureCpuDebugger();
        debugProjectPending_=job->project;
        debugProjectPendingPath_=job->path;
        cpuDebuggerDialog_->applyProject(job->project);
    }
    updateDebugProjectActions();
}

void MainWindow::finishDebugProjectApply(bool accepted) {
    if (!debugProjectPending_) return;
    const auto project=std::move(*debugProjectPending_);
    debugProjectPending_.reset();
    if (!accepted && cpuDebuggerDialog_ && !cpuDebuggerDialog_->isVisible())
        QMessageBox::warning(this,text("Проект отладки"),cpuDebuggerDialog_->projectError());
    if (accepted) {
        debugProjectApplying_=true;
        ensureDebugWatch();
        debugWatchDialog_->setEntries(project.watches);
        debugProjectApplying_=false;
        debugProjectPath_=debugProjectPendingPath_;
        debugProjectGame_=currentGameName_;
        ++debugProjectRevision_;
        debugProjectDirty_=false;
        scheduleInspection();
    }
    updateDebugProjectActions();
}
