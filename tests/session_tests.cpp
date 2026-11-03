#include "core/EmulationSession.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QThread>
#include <QUrlQuery>
#include <QtEndian>
#include <atomic>
#include <cstdio>
#include <functional>
#include <stdexcept>
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void waitFor(const std::function<bool()>& predicate){
 QElapsedTimer t;t.start();while(!predicate() && t.elapsed()<4000){QCoreApplication::processEvents();QThread::msleep(2);}require(predicate(),"session timeout");
}
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);
 try {
 const bool achievementsOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--achievements");
 const bool achievementLoadingOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--achievements-loading");
 const bool fastOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--fast-forward");
 const bool portsOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--controller-ports");
 const bool checksumOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--checksum");
 require(argc==2 || fastOnly || checksumOnly || portsOnly || achievementsOnly || achievementLoadingOnly,"core argument required");QTemporaryDir temp;require(temp.isValid(),"temporary directory");
 QByteArray rom(32768,'\0');qToBigEndian<quint32>(0xffff00,rom.data());qToBigEndian<quint32>(0x200,rom.data()+4);
 rom.replace(0x100,16,QByteArray("SEGA MEGA DRIVE ",16));rom.replace(0x1f0,3,"USA");rom[0x200]=char(0x60);rom[0x201]=char(0xfe);
 const auto path=temp.filePath("original.md");QFile file(path);require(file.open(QIODevice::WriteOnly) && file.write(rom)==rom.size(),"ROM fixture");file.close();
 EmulationSession session(QString::fromLocal8Bit(argv[1]),temp.path());
 std::atomic<int> frames{0};int ready=0,errors=0,actions=0;bool paused=false;
 QObject::connect(&session,&EmulationSession::videoFrame,&app,[&](const QImage&,double){++frames;},Qt::DirectConnection);
 QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&]{++ready;});
 QObject::connect(&session,&EmulationSession::errorOccurred,&app,[&](const QString& s){++errors;std::fprintf(stderr,"%s\n",qPrintable(s));});
 QObject::connect(&session,&EmulationSession::actionCompleted,&app,[&](const QString&){++actions;});
 QObject::connect(&session,&EmulationSession::effectivePauseChanged,&app,[&](bool p){paused=p;});
 if(achievementLoadingOnly){
  // The mock owns every RetroAchievements request: no real network or account data.
  QByteArray secondRom=rom, thirdRom=rom;
  secondRom[0x300]=char(1);thirdRom[0x300]=char(2);
  const auto secondPath=temp.filePath("second.md"), thirdPath=temp.filePath("third.md");
  for(const auto& fixture:{qMakePair(secondPath,secondRom),qMakePair(thirdPath,thirdRom)}){
   QFile out(fixture.first);
   require(out.open(QIODevice::WriteOnly) && out.write(fixture.second)==fixture.second.size(),"switch ROM fixture");
  }
  auto within=[&](const std::function<bool()>& predicate,const char* message,int limitMs=950){
   QElapsedTimer timer;timer.start();
   while(!predicate() && timer.elapsed()<limitMs){QCoreApplication::processEvents();QThread::msleep(2);}
   require(predicate(),message);
  };
  int loadedGames=0,frameAtLoad=0,gameRequests=0,gameNotices=0,failedNotices=0;
  quint64 loginRequest=0,gameRequest=0;
  bool unexpectedRequest=false;
  AchievementsState achievementState;
  QObject::connect(&session,&EmulationSession::gameLoaded,&app,[&](const QString&,double,const QString&){
   ++loadedGames;frameAtLoad=frames.load();
  });
  QObject::connect(&session,&EmulationSession::achievementsChanged,&app,[&](const AchievementsState& state){achievementState=state;});
  QObject::connect(&session,&EmulationSession::achievementNotice,&app,[&](const AchievementNotice& notice){
   if(notice.kind==AchievementNotice::GameReady)++gameNotices;
   if(notice.kind==AchievementNotice::LoadFailed)++failedNotices;
  });
  QObject::connect(&session,&EmulationSession::achievementHttpRequest,&app,[&](const AchievementHttpRequest& request){
   const auto operation=QUrlQuery(QString::fromUtf8(request.postData)).queryItemValue("r");
   if(operation=="login2") loginRequest=request.id;
   else if(operation=="achievementsets"){gameRequest=request.id;++gameRequests;}
   else if(operation=="startsession" || operation=="ping")
    session.achievementResponse(request.id,200,R"({"Success":true})");
   else unexpectedRequest=true;
  });
  session.configureAchievements(true,false);
  session.loginAchievements("Fixture","TestPassword");
  within([&]{return loginRequest!=0;},"mock login request missing");
  session.loadGame(path);
  within([&]{return loadedGames==1 && frames.load()>=frameAtLoad+5;},"ROM waited for login response");
  require(achievementState.loading && gameNotices==0 && !paused,"login pending state disturbed game");
  const int duringLogin=frames.load();
  within([&]{return frames.load()>=duringLogin+5;},"frames stopped during pending login");
  session.achievementResponse(loginRequest,200,
   R"({"Success":true,"User":"Fixture","Token":"FixtureToken","Score":0,"SoftcoreScore":0,"Messages":0,"Permissions":1,"AccountType":"Registered"})");
  within([&]{return gameRequests==1 && gameRequest!=0;},"mock game request missing");
  require(achievementState.loading && gameNotices==0 && !paused,"game request paused ROM");
  const int duringGameLoad=frames.load();
  within([&]{return frames.load()>=duringGameLoad+5;},"frames stopped during pending game response");
  session.achievementResponse(gameRequest,200,
   R"({"Success":true,"GameId":1234,"Title":"Original Fixture","ConsoleId":1,"ImageIconUrl":"https://retroachievements.org/Images/112233.png","RichPresenceGameId":1234,"RichPresencePatch":"","Sets":[{"AchievementSetId":1111,"GameId":1234,"Title":null,"Type":"core","ImageIconUrl":"https://retroachievements.org/Images/112233.png","Achievements":[{"ID":5501,"Title":"Fixture","Description":"Synthetic condition","Flags":3,"Points":5,"MemAddr":"0xH0001=3","Author":"Fixture","BadgeName":"00234","Created":1367266583,"Modified":1376929305}],"Leaderboards":[]}]})");
  within([&]{return gameNotices==1 && achievementState.gameId==1234 && !achievementState.loading;},
         "delayed success did not produce one ready notice");
  require(failedNotices==0 && !paused,"success disturbed game");

  // A failed response while switching ROM must report once and leave emulation running.
  gameRequest=0;session.loadGame(secondPath);
  within([&]{return loadedGames==2 && gameRequests==2 && gameRequest!=0;},"second ROM or game request missing");
  within([&]{return frames.load()>=frameAtLoad+5;},"second ROM waited for achievements");
  session.achievementResponse(gameRequest,400,R"({"Success":false,"Error":"Synthetic failure"})");
  within([&]{return failedNotices==1 && !achievementState.loading;},"HTTP failure notice missing or repeated");
  const int afterFailure=frames.load();
  within([&]{return frames.load()>=afterFailure+5;},"HTTP failure stopped frames");
  require(gameNotices==1 && failedNotices==1 && !paused && actions==0,"HTTP failure reset or paused game");

  // A request that never replies must time out while frames keep advancing.
  gameRequest=0;session.loadGame(thirdPath);
  within([&]{return loadedGames==3 && gameRequests==3 && gameRequest!=0;},"third ROM or game request missing");
  within([&]{return frames.load()>=frameAtLoad+5;},"third ROM waited for achievements");
  const int beforeTimeout=frames.load();
  QElapsedTimer timeout;timeout.start();
  while(failedNotices<2 && timeout.elapsed()<17500){QCoreApplication::processEvents();QThread::msleep(3);}
  require(failedNotices==2 && !achievementState.loading,"unanswered request did not notify once at timeout");
  require(frames.load()>=beforeTimeout+150 && gameNotices==1 && !paused && actions==0,
          "unanswered request stalled or reset emulation");
  session.achievementResponse(gameRequest,200,R"({"Success":true})");
  QElapsedTimer settle;settle.start();
  while(settle.elapsed()<80){QCoreApplication::processEvents();QThread::msleep(2);}
  require(failedNotices==2 && gameNotices==1 && errors==0 && !unexpectedRequest,
          "late response changed completed timeout or unexpected mock request");
  session.shutdown();
  std::puts("Achievement loading: ROMs run during pending login/game requests, success, HTTP failure and no-response timeout passed");
  return 0;
 }
 session.loadGame(path);waitFor([&]{return frames.load()>=5;});
 if(achievementsOnly){
  int requests=0, gameNotices=0;AchievementsState achievementState;bool hardcore=false;
  QObject::connect(&session,&EmulationSession::achievementsChanged,&app,[&](const AchievementsState& s){achievementState=s;});
  QObject::connect(&session,&EmulationSession::achievementsHardcoreChanged,&app,[&](bool value){hardcore=value;});
  QObject::connect(&session,&EmulationSession::achievementNotice,&app,[&](const AchievementNotice& n){if(n.kind==AchievementNotice::GameReady)++gameNotices;});
  QObject::connect(&session,&EmulationSession::achievementHttpRequest,&app,[&](const AchievementHttpRequest& request){
   ++requests;const QUrlQuery query(QString::fromUtf8(request.postData));
   const auto operation=query.queryItemValue("r");QByteArray reply;
   if(operation=="login2") reply=R"({"Success":true,"User":"Fixture","Token":"FixtureToken","Score":0,"SoftcoreScore":0,"Messages":0,"Permissions":1,"AccountType":"Registered"})";
   else if(operation=="achievementsets") reply=R"({"Success":true,"GameId":1234,"Title":"Original Fixture","ConsoleId":1,"ImageIconUrl":"https://retroachievements.org/Images/112233.png","RichPresenceGameId":1234,"RichPresencePatch":"","Sets":[{"AchievementSetId":1111,"GameId":1234,"Title":null,"Type":"core","ImageIconUrl":"https://retroachievements.org/Images/112233.png","Achievements":[{"ID":5501,"Title":"Fixture","Description":"Synthetic condition","Flags":3,"Points":5,"MemAddr":"0xH0001=3","Author":"Fixture","BadgeName":"00234","Created":1367266583,"Modified":1376929305}],"Leaderboards":[]}]})";
   else if(operation=="startsession" || operation=="ping") reply=R"({"Success":true})";
   else {++errors;std::fprintf(stderr,"Unexpected mock API operation: %s\n",qPrintable(operation));reply=R"({"Success":false,"Error":"Unexpected mock operation"})";}
   session.achievementResponse(request.id,200,reply);
  });
  session.configureAchievements(true,true); // Unapproved public Hardcore must stay disabled.
  session.requestAchievements();waitFor([&]{return achievementState.enabled;});
  require(requests==0 && !hardcore && !achievementState.hardcore,"anonymous or unapproved Hardcore enabled");
  session.loginAchievements("Fixture","TestPassword");
  waitFor([&]{return achievementState.gameId==1234 && !achievementState.loading && gameNotices==1;});
  require(!hardcore && !achievementState.hardcore,"Hardcore approval gate bypassed by login");
  const int initial=frames;waitFor([&]{return frames>initial+5;});
  session.setPaused(true);waitFor([&]{return paused;});
  session.saveState(0);waitFor([&]{return actions==1;});
  const QDir states(temp.path()+"/states");const auto files=states.entryList({"*.slot0.krs"},QDir::Files);
  require(files.size()==1,"achievement state missing");
  QFile stateFile(states.filePath(files.first()));require(stateFile.open(QIODevice::ReadOnly),"achievement state read");
  require(stateFile.readAll().contains("KRACT001"),"worker failed to embed achievement progress");stateFile.close();
  session.loadState(0);waitFor([&]{return actions==2;});
  session.configureAchievements(false,false);waitFor([&]{return !achievementState.enabled;});
  const int sent=requests;session.setPaused(false);waitFor([&]{return !paused && frames>initial+12;});
  require(requests==sent && errors==0,"disabled achievements sent requests or disturbed worker");
  session.shutdown();
  std::puts("Achievement worker: no anonymous HTTP, approval gate, synthetic login/game, native state progress and disable/resume passed");return 0;
 }
 if(portsOnly){
  std::atomic<bool> reversed{false};
  QObject::connect(&session,&EmulationSession::rewindingChanged,&app,[&](bool value){reversed=value;},Qt::DirectConnection);
  session.setPaused(true);waitFor([&]{return paused;});
  session.setRewind(true);waitFor([&]{return reversed.load();});
  session.setControllerConnected(1,true);waitFor([&]{return !reversed.load();});
  const int frozen=frames;
  session.setRewind(true);QThread::msleep(80);QCoreApplication::processEvents();
  require(!reversed && frames==frozen,"live port change retained mismatched rewind history");
  session.setRewind(false);session.shutdown();require(errors==0,"port session error");
  std::puts("Controller connection commands run on worker and discard old rewind history: passed");return 0;
 }
 if(checksumOnly){
  session.setPaused(true);waitFor([&]{return paused;});
  std::atomic<int> audioResets{0};
  QObject::connect(&session,&EmulationSession::audioReset,&app,[&]{++audioResets;},Qt::DirectConnection);
  session.saveState(0);waitFor([&]{return actions==1;});
  const QDir states(temp.path()+"/states");
  const auto files=states.entryList({"*.slot0.krs"},QDir::Files);
  require(files.size()==1,"checksum test state missing");
  auto readState=[&]{QFile state(states.filePath(files.first()));require(state.open(QIODevice::ReadOnly),"state read failed");return state.readAll();};
  const QByteArray original=readState();const int frozen=frames;
  for(bool enabled:{true,false}){
   const int completed=actions;
   session.setAutoFixChecksum(enabled);session.saveState(0);
   waitFor([&]{return actions>=completed+1;});
   require(errors==0 && audioResets==0 && frames==frozen && paused,
           "checksum toggle disturbed the running session");
   require(readState()==original,"checksum toggle changed the game state");
  }
  session.setAutoFixChecksum(true);const int completed=actions;
  session.hardResetGame();waitFor([&]{return actions>=completed+1;});
  require(audioResets>0 && errors==0,"explicit hard reset no longer works");
  session.shutdown();
  std::puts("Checksum toggles preserve exact paused state; explicit hard reset passed");
  return 0;
 }
 if(fastOnly){
  std::atomic<bool> reversed{false};
  QObject::connect(&session,&EmulationSession::rewindingChanged,&app,[&](bool value){reversed=value;},Qt::DirectConnection);
  session.setFastForwardFactor(0);session.setFastForward(true);
  const int initial=frames;waitFor([&]{return frames>=initial+8;});
  session.setPaused(true);waitFor([&]{return paused;});
  session.setFastForward(false);session.setRewind(true);
  const int boundary=frames;QThread::msleep(100);QCoreApplication::processEvents();
  require(frames==boundary && !reversed,"rewind crossed fast-forward gap or recorded accelerated frames");
  session.setRewind(false);session.setPaused(false);waitFor([&]{return !paused && frames>=boundary+8;});
  session.setPaused(true);waitFor([&]{return paused;});
  session.setRewind(true);waitFor([&]{return reversed.load();});
  require(frames>boundary+8,"rewind did not resume after fast-forward");
  session.setRewind(false);session.shutdown();require(errors==0,"fast-forward session error");
  std::puts("Unlimited fast-forward, no history across gap, normal rewind recording resumed: passed");
  return 0;
 }
 const int before=frames;QThread::msleep(180);QCoreApplication::processEvents();
 require(frames>before+5,"GUI stall stopped emulation worker");require(ready==1,"video notifications accumulated without consumer");
 QImage image;double aspect=0;require(session.takeVideoFrame(image,aspect)&&!image.isNull(),"latest frame unavailable");
 require(!session.takeVideoFrame(image,aspect),"same delivery consumed twice");waitFor([&]{return ready==2;});
 session.setPaused(true);waitFor([&]{return paused;});const int frozen=frames;QThread::msleep(80);require(frames==frozen,"manual pause runs frames");
 session.setBackgroundPaused(true);session.setPaused(false);QThread::msleep(80);require(frames==frozen,"background pause lost to manual resume");
 session.setBackgroundPaused(false);waitFor([&]{return !paused && frames>frozen;});
 for(int region:{2,3}){
  session.setRegion(region);QElapsedTimer settle;settle.start();while(settle.elapsed()<150){QCoreApplication::processEvents();QThread::msleep(2);}
  const int begin=frames;QElapsedTimer timer;timer.start();
  while(timer.elapsed()<2000){session.requestSlotStatus(0);QCoreApplication::processEvents();QThread::msleep(4);}
  const double measured=(frames.load()-begin)*1000.0/timer.elapsed();
  require(measured>(region==2?57:47) && measured<(region==2?63:53),"slot queries perturb frame cadence");
  std::printf("Session region %d cadence %.2f fps under slot queries\n",region,measured);
 }
 session.resetGame();waitFor([&]{return actions==1;});session.hardResetGame();waitFor([&]{return actions==2;});
 require(errors==0,"session reported error");session.shutdown();
 std::puts("Session: coalesced latest frame, GUI stall isolation, pause reasons, PAL/NTSC cadence and resets passed");
 return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
