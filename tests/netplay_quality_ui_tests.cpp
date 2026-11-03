#include "ui/NetplayLobby.h"
#include "i18n/Localization.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
QString text(const char* source) {return Localization::text(QString::fromUtf8(source));}
QJsonObject sample(int ping=42,int delay=3) {
    return {{"active",true},{"pingMs",ping},{"jitterMs",7},{"delayFrames",delay},
        {"rollbackFrames",12},{"rollbackEvents",2},{"maxRollbackFrames",8},
        {"peers",QJsonArray{QJsonObject{{"id","7"},{"pingMs",40},{"jitterMs",6}},
            QJsonObject{{"id","9"},{"pingMs",-1},{"jitterMs",0}}}}};
}
void checkLobby(const QString& language,const QString& projectRoot) {
    QTemporaryDir data;require(data.isValid(),"language settings unavailable");
    {QSettings settings(data.filePath("settings.ini"),QSettings::IniFormat);settings.setValue("ui/language",language);settings.sync();}
    Localization::initialize(projectRoot,data.path());
    require(Localization::currentLanguage()==language,"requested UI language unavailable");
    NetplayLobby lobby;
    const QJsonArray participants{
        QJsonObject{{"id","0"},{"nickname","Local"},{"role","player1"},{"local",true},{"host",true},{"readiness","ready"},{"pingMs",0}},
        QJsonObject{{"id","7"},{"nickname","Peer"},{"role","player2"},{"readiness","ready"},{"pingMs",88}},
        QJsonObject{{"id","9"},{"nickname","Watcher"},{"role","spectator"},{"readiness","ready"},{"pingMs",55}}};
    QJsonObject state{{"active",true},{"host",true},{"participants",participants}};
    lobby.setLobbyState(state);lobby.show();QApplication::processEvents();
    auto* summary=lobby.findChild<QLabel*>(QStringLiteral("netplayQuality"));
    auto* table=lobby.findChild<QTableWidget*>(QStringLiteral("netplayParticipants"));
    require(summary&&table,"quality summary or participant table missing");
    require(summary->text()==text("Качество связи появится после запуска игры"),"pre-game quality did not show an honest placeholder");
    require(table->item(1,3)->text()==text("%1 мс").arg(88)&&table->item(1,3)->toolTip().contains("TCP"),"pre-game ping is not marked as TCP");
    require(table->item(0,3)->text()==QStringLiteral("—"),"local row displayed a fake zero network ping");
    auto* role=qobject_cast<QComboBox*>(table->cellWidget(1,1));
    require(role,"host role control missing");table->selectRow(1);int roleRequests=0;
    QObject::connect(&lobby,&NetplayLobby::roleRequested,&lobby,[&](const QString&,const QString&){++roleRequests;});
    lobby.setNetworkQuality(sample());QApplication::processEvents();
    const QString expected=text("У вас: пинг %1 · разброс %2\nВвод: %3 кадр. (авто) · Откаты: %4 кадр./с")
        .arg(text("%1 мс").arg(42),text("%1 мс").arg(7),QStringLiteral("3"),QStringLiteral("12"));
    require(summary->text()==expected,"local ping, jitter, auto delay or rollback rate label incorrect");
    require(summary->toolTip().contains(language=="ru"?QStringLiteral("а не частота кадров игры"):QStringLiteral("not game FPS")),"rollback rate tooltip does not distinguish resimulation from game FPS");
    require(summary->toolTip().contains("2")&&summary->toolTip().contains("8"),"rollback events or maximum length missing from tooltip");
    require(table->item(1,3)->text()==text("%1 мс").arg(40)&&table->item(1,3)->toolTip().contains("UDP"),"gameplay UDP ping did not replace lobby TCP ping");
    require(table->item(2,3)->text()==text("%1 мс").arg(55)&&table->item(2,3)->toolTip()==text("Служебный пинг лобби (TCP). Измерение игрового пинга сейчас недоступно."),"unmeasured peer lost the TCP fallback or promised unavailable gameplay stats");
    for(int i=0;i<5;++i)lobby.setNetworkQuality(sample());
    require(table->cellWidget(1,1)==role&&table->currentRow()==1&&roleRequests==0,"quality updates rebuilt role controls, changed selection or emitted a role request");
    auto unknown=sample(-1,-1);unknown.insert("rollbackFrames",0);unknown.insert("peers",QJsonArray{});
    lobby.setNetworkQuality(unknown);
    require(summary->text()==text("У вас: пинг %1 · разброс %2\nВвод: %3 кадр. (авто) · Откаты: %4 кадр./с")
        .arg(QStringLiteral("—"),QStringLiteral("—"),QStringLiteral("—"),QStringLiteral("0")),"unknown ping or spectator input delay displayed a fake zero");
    const QString beforeHidden=summary->text();lobby.hide();lobby.setNetworkQuality(sample(99,4));
    require(summary->text()==beforeHidden,"hidden lobby updated widgets instead of caching the sample");
    lobby.show();QApplication::processEvents();
    require(summary->text().contains("99")&&summary->text().contains("4")&&table->cellWidget(1,1)==role&&table->currentRow()==1,"opening lobby did not apply latest cached sample without rebuilding roles");
    lobby.setNetworkQuality({{"active",false}});
    require(summary->text()==text("Качество связи появится после запуска игры")&&table->item(1,3)->text()==text("%1 мс").arg(88),"inactive quality retained stale game measurements");
    state.insert("active",false);lobby.setLobbyState(state);
    require(summary->isHidden(),"quality summary stayed visible after leaving session");
}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        const QString root=app.arguments().size()>1?app.arguments()[1]:QDir::currentPath();
        checkLobby(QStringLiteral("ru"),root);checkLobby(QStringLiteral("en"),root);
        std::puts("lobby local quality: RU/EN, UDP/TCP fallback, unknown values, stable roles/selection, hidden cache and inactive cleanup passed");return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"quality UI test failed: %s\n",error.what());return 1;}
}
