#include "core/EmulationSession.h"
#include "core/MovieTape.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QThread>
#include <QUrlQuery>
#include <QtEndian>
#include <atomic>
#include <array>
#include <cstdio>
#include <functional>
#include <stdexcept>
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void waitFor(const std::function<bool()>& predicate,const char* description="session timeout"){
 QElapsedTimer t;t.start();bool ready=predicate();while(!ready && t.elapsed()<4000){QCoreApplication::processEvents();QThread::msleep(2);ready=predicate();}require(ready,description);
}
// Original 68000 fixture: change CRAM every VBlank and sample the pad into
// system RAM and cartridge SRAM. Distinct pictures expose stale frame restores;
// input-sensitive native state exposes replay/branch mistakes.
QByteArray frameBackRom(){
 QByteArray rom(64*1024,char(0xff));
 const auto put=[&](int at,quint32 value){qToBigEndian(value,rom.data()+at);};
 put(0,0x00ff0000);put(4,0x200);
 rom.replace(0x100,16,"SEGA GENESIS    ");rom.replace(0x120,16,"FRAME BACK TEST ");
 rom.replace(0x150,16,"FRAME BACK TEST ");rom.replace(0x180,2,"GM");
 put(0x1a0,0);put(0x1a4,quint32(rom.size()-1));put(0x1a8,0x00ff0000);put(0x1ac,0x00ffffff);
 rom.replace(0x1f0,1,"U");rom.replace(0x1b0,2,"RA");rom[0x1b2]=char(0xf8);rom[0x1b3]=char(0x20);
 put(0x1b4,0x00200000);put(0x1b8,0x0020ffff);
 int pc=0x200;
 const auto word=[&](quint16 value){rom[pc++]=char(value>>8);rom[pc++]=char(value);};
 word(0x33fc);word(0x8004);word(0x00c0);word(0x0004);
 word(0x33fc);word(0x8144);word(0x00c0);word(0x0004);
 word(0x33fc);word(0x8700);word(0x00c0);word(0x0004);
 word(0x13fc);word(0x0040);word(0x00a1);word(0x0009);
 word(0x13fc);word(0x0040);word(0x00a1);word(0x0003);
 word(0x7000);word(0x7400);
 const int waitOn=pc;
 word(0x3239);word(0x00c0);word(0x0004);word(0x0241);word(0x0008);
 const int branchOn=pc;word(0x6700);
 const int waitOff=pc;
 word(0x3239);word(0x00c0);word(0x0004);word(0x0241);word(0x0008);
 const int branchOff=pc;word(0x6600);
 // Sample and change CRAM at the start of active display, so actual input
 // changes this frame's pixels as well as native state and cartridge SRAM.
 word(0x1039);word(0x00a1);word(0x0003);
 word(0x33c0);word(0x00ff);word(0x0000);
 word(0x13c0);word(0x0020);word(0x0000);
 word(0xd040); // ADD.W D0,D0: pad bits reach visible CRAM colour bits.
 word(0x5442); // ADDQ.W #2,D2: pictures also change with neutral input.
 word(0xb142); // EOR.W D0,D2.
 word(0x23fc);word(0xc000);word(0x0000);word(0x00c0);word(0x0004);
 word(0x33c2);word(0x00c0);word(0x0000);
 const int branchAgain=pc;word(0x6000);
 const int branches[][2]={{branchOn,waitOn},{branchOff,waitOff},{branchAgain,waitOn}};
 for(const auto& branch:branches){
  const int displacement=branch[1]-(branch[0]+2);
  require(displacement>=-128 && displacement<=127 && displacement!=0,"frame-back fixture branch");
  rom[branch[0]+1]=char(displacement&0xff);
 }
 return rom;
}
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);
 try {
 const bool achievementsOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--achievements");
 const bool achievementLoadingOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--achievements-loading");
 const bool spriteOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--sprite-ownership");
 const bool fastOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--fast-forward");
 const bool portsOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--controller-ports");
 const bool frameBackOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--frame-back");
 const bool inputDisplayOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--input-display");
 const bool speedOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--game-speed");
 const bool timerOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--run-timer");
 const bool rewindResetsOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--rewind-resets");
 const bool checksumOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--checksum");
 require(argc==2 || rewindResetsOnly || spriteOnly || fastOnly || checksumOnly || portsOnly || achievementsOnly || achievementLoadingOnly || frameBackOnly || inputDisplayOnly || timerOnly || speedOnly,"core argument required");QTemporaryDir temp;require(temp.isValid(),"temporary directory");
 QByteArray rom(32768,'\0');qToBigEndian<quint32>(0xffff00,rom.data());qToBigEndian<quint32>(0x200,rom.data()+4);
 rom.replace(0x100,16,QByteArray("SEGA MEGA DRIVE ",16));rom.replace(0x1f0,3,"USA");rom[0x200]=char(0x60);rom[0x201]=char(0xfe);
 if(frameBackOnly || inputDisplayOnly || rewindResetsOnly)rom=frameBackRom();
 if(spriteOnly){
  const auto program=QByteArray::fromHex("33fc800400c0000433fc814400c0000460fe");
  rom.replace(0x200,program.size(),program);
 }
 const auto path=temp.filePath("original.md");QFile file(path);require(file.open(QIODevice::WriteOnly) && file.write(rom)==rom.size(),"ROM fixture");file.close();
 EmulationSession session(QString::fromLocal8Bit(argv[1]),temp.path());
 std::atomic<int> frames{0};int ready=0,errors=0,actions=0;bool paused=false;
 QObject::connect(&session,&EmulationSession::videoFrame,&app,[&](const QImage&,double){++frames;},Qt::DirectConnection);
 QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&]{++ready;});
 QObject::connect(&session,&EmulationSession::errorOccurred,&app,[&](const QString& s){++errors;std::fprintf(stderr,"%s\n",qPrintable(s));});
 QObject::connect(&session,&EmulationSession::actionCompleted,&app,[&](const QString&){++actions;});
 QObject::connect(&session,&EmulationSession::effectivePauseChanged,&app,[&](bool p){paused=p;});
 if(inputDisplayOnly){
  InputDisplayState shown;
  int changes=0,closed=0;
  std::atomic<int> movieMode{0};
  std::atomic<unsigned> movieLocalMask{0};
  std::atomic<bool> pauseNext{false};
  QObject::connect(&session,&EmulationSession::inputDisplayChanged,&app,[&]{
   shown=session.inputDisplay();++changes;
  });
  QObject::connect(&session,&EmulationSession::gameClosed,&app,[&]{++closed;});
  QObject::connect(&session,&EmulationSession::movieStatusChanged,&app,
   [&](int mode,const QString&,quint64,quint64,double){
    movieMode=mode;
    if ((mode==1 || mode==2) && movieLocalMask.load())
     session.setButtons(1,movieLocalMask.load());
   },Qt::DirectConnection);
  QObject::connect(&session,&EmulationSession::videoFrame,&app,
   [&](const QImage&,double){if(pauseNext.exchange(false))session.setPaused(true);},
   Qt::DirectConnection);
  session.setInputDisplayEnabled(true);
  session.loadGame(path);
  waitFor([&]{return frames.load()>=5 && shown.players[0].device==InputDisplayDevice::MegaDrive6;},
          "input display startup");
  session.setPaused(true);waitFor([&]{return paused;},"input display pause");
  session.setControllerConnected(1,true);
  session.setControllerSixButton(1,false);
  waitFor([&]{return shown.players[1].device==InputDisplayDevice::MegaDrive3;},
          "P2 three-button topology");
  require(shown.players[0].buttons==0 && shown.players[1].buttons==0,
          "topology change retained buttons");
  QImage image;double aspect=0;
  const auto step=[&](bool backwards,quint16 expected){
   session.takeVideoFrame(image,aspect);
   const int before=frames.load();
   if(backwards)session.backFrame();else session.advanceFrame();
   waitFor([&]{return frames.load()>before && shown.players[1].buttons==expected;},
           backwards?"input display frame back":"input display frame advance");
   require(frames.load()==before+1 && paused && shown.players[0].buttons==0,
           "input display step skipped a frame or showed P1 input");
  };
  constexpr quint16 face=quint16(1u<<4), sixOnly=quint16(1u<<9);
  session.setButtons(0,0);session.setButtons(1,face|sixOnly);
  step(false,face);
  require(shown.players[1].device==InputDisplayDevice::MegaDrive3,
          "three-button pad did not filter six-button input");
  session.setControllerSixButton(1,true);
  waitFor([&]{return shown.players[1].device==InputDisplayDevice::MegaDrive6;},
          "P2 six-button topology");
  session.setButtons(1,face|sixOnly);
  step(false,face|sixOnly);
  session.setButtons(1,quint16(1u<<5));
  step(false,quint16(1u<<5));
  step(true,face|sixOnly);
  require(shown.players[1].device==InputDisplayDevice::MegaDrive6,
          "frame back lost P2 device type");

  // Playback must display the recorded P2 input, not the local live mask.
  const auto moviePath=temp.filePath("input-display.krm");
  movieLocalMask=face;
  const int start=frames.load();
  session.startMovieRecording(moviePath);
  waitFor([&]{return movieMode.load()==1 && frames.load()>=start+8;},
          "input display movie recording");
  session.stopMovie();session.setPaused(true);
  waitFor([&]{return movieMode.load()==0 && paused && QFile::exists(moviePath);},
          "input display movie stop");
  MovieTape tape;QString error;require(tape.load(moviePath,error),qPrintable(error));
  require(tape.size()>=8,"input display movie too short");
  for(quint64 i=0;i<tape.size();++i){
   MovieInput input;
   require(tape.inputAt(i,input) && input.buttons[0]==0 && input.buttons[1]==face,
           "input display movie did not record P2-only input");
  }
  const quint16 decoy=quint16(1u<<5);
  movieLocalMask=decoy;
  session.setButtons(1,decoy);
  step(false,decoy);
  pauseNext=true;
  session.playMovie(moviePath);
  waitFor([&]{return movieMode.load()==2 && paused && shown.players[1].buttons==face;},
          "playback displayed local input instead of recorded P2 input");
  require(shown.players[0].buttons==0 && shown.players[1].device==InputDisplayDevice::MegaDrive6,
          "playback displayed wrong player or hardware");
  step(false,face);
  session.closeGame();
  waitFor([&]{return closed==1 && shown==InputDisplayState{};},
          "closed game retained input display");
  require(changes>0 && errors==0,"input display session emitted a core error");
  session.shutdown();
  std::puts("Input display: P2-only, three/six-button, frame back, TAS playback and close passed");
  return 0;
 }
 if(spriteOnly){
  int closed=0;bool reversing=false;
  QObject::connect(&session,&EmulationSession::gameClosed,&app,[&]{++closed;});
  QObject::connect(&session,&EmulationSession::rewindingChanged,&app,[&](bool value){reversing=value;});
  QImage image,mask;double aspect=0;
  const auto take=[&](bool expectedMask){
   return session.takeVideoFrame(image,aspect,&mask) && (mask.isNull()!=expectedMask);
  };
  const auto check=[&]{
   require(!image.isNull() && mask.size()==image.size() && mask.format()==QImage::Format_Grayscale8,
           "sprite ownership geometry/format not paired with video");
   for(int y=0;y<image.height();++y){
    const auto* pixels=reinterpret_cast<const QRgb*>(image.constScanLine(y));
    const auto* hints=mask.constScanLine(y);
    for(int x=0;x<image.width();++x)
     require(qAlpha(pixels[x])==255 && hints[x]<=1,"sprite ownership contaminated clean pixels or values");
   }
  };
  session.setSpriteOwnershipEnabled(true); // Must apply before the very first game.
  session.loadGame(path);waitFor([&]{return take(true);},"startup native ownership timed out");check();
  const QSize native=image.size();
  session.setPaused(true);waitFor([&]{return paused;},"sprite test pause timed out");
  session.takeVideoFrame(image,aspect,&mask); // Drain the last running presentation.
  session.setSpriteOwnershipEnabled(false);session.advanceFrame();
  waitFor([&]{return take(false);},"ownership fallback presentation timed out");
  session.setSpriteOwnershipEnabled(true);session.advanceFrame();
  waitFor([&]{return take(true);},"pause enable native ownership timed out");check();
  session.setPaused(false);waitFor([&]{return !paused;},"sprite test resume timed out");
  session.setBlarggFilter(2);
  waitFor([&]{return take(false) && image.width()>native.width();},"filter resize without ownership timed out");
  session.setBlarggFilter(0);
  waitFor([&]{return take(true) && image.size()==native;},"native ownership after filter timed out");check();
  const int start=frames.load();waitFor([&]{return frames.load()>=start+30;},"rewind history capture timed out");
  session.setRewind(true);waitFor([&]{return reversing;},"sprite test rewind start timed out");
  waitFor([&]{return take(false);},"ownership fallback presentation timed out"); // Rewind screenshots deliberately carry no ownership.
  session.setRewind(false);waitFor([&]{return !reversing;},"sprite test rewind stop timed out");
  waitFor([&]{return take(true);},"rewind resume native ownership timed out");check();
  session.closeGame();waitFor([&]{return closed==1;},"sprite test close timed out");
  require(!session.takeVideoFrame(image,aspect,&mask),"closed game retained a pending ownership frame");
  session.loadGame(path);waitFor([&]{return take(true);},"reload native ownership timed out");check();
  session.setSpriteOwnershipEnabled(false);waitFor([&]{return take(false);},"ownership fallback presentation timed out");
  require(errors==0,"sprite ownership session emitted a core error");
  session.shutdown();
  std::puts("Sprite ownership: startup, pause switches, clean frame pairing, filter resize/fallback, rewind and reload passed");
  return 0;
 }
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
 if(speedOnly){
  std::atomic<qint64> samples{0};std::atomic<int> sampleRate{0};
  QObject::connect(&session,&EmulationSession::audioSamples,&app,[&](const QByteArray& pcm,int rate){
   samples+=pcm.size()/4;sampleRate=rate;
  },Qt::DirectConnection);
  session.setRewindEnabled(false);
  for (int percent : {5,75,100,200}) {
   session.setGameSpeedPercent(percent);
   int start=frames.load();
   waitFor([&]{return frames.load()>=start+2;},"speed change stalled");
   const auto baseFrames=frames.load();const auto baseSamples=samples.load();
   QElapsedTimer clock;clock.start();
   const int interval=percent==5 ? 3 : percent/5;
   waitFor([&]{return frames.load()>=baseFrames+interval;},"game speed stalled");
   const auto elapsed=clock.elapsed();
   const auto expectedMs=interval*100000.0/(60.0*percent);
   require(elapsed>expectedMs*0.78 && elapsed<expectedMs*1.4,"wrong wall time for game speed");
   const auto playedMs=(samples.load()-baseSamples)*1000.0/sampleRate.load();
   require(playedMs>expectedMs*0.9 && playedMs<expectedMs*1.1,"live PCM duration does not follow game speed");
  }
  session.shutdown();std::puts("Game speed: 5/75/100/200% pacing and live PCM duration passed");return 0;
 }
 if(timerOnly){
  quint64 timerFrames=0,timerUs=0;int timerEvents=0;
  QObject::connect(&session,&EmulationSession::runTimerChanged,&app,
   [&](quint64 count,quint64 microseconds){
    timerFrames=count;timerUs=microseconds;++timerEvents;
   });
  session.setPaused(true);waitFor([&]{return paused;},"timer initial pause");
  QImage image;double aspect=0;session.takeVideoFrame(image,aspect);
  session.setRunTimerEnabled(true);
  const auto reset=[&](bool hard){
   const int completed=actions+1,before=timerEvents;
   if(hard)session.hardResetGame();else session.resetGame();
   waitFor([&]{return actions>=completed && timerEvents>before && timerFrames==0 && timerUs==0;},
           hard?"timer hard reset":"timer soft reset");
   require(paused,"timer reset left explicit pause");
  };
  reset(false);
  const auto step=[&](bool backward,quint64 expected){
   session.takeVideoFrame(image,aspect);
   const int beforeFrame=frames.load(),beforeTimer=timerEvents;
   if(backward)session.backFrame();else session.advanceFrame();
   waitFor([&]{return frames.load()>beforeFrame && timerEvents>beforeTimer;},
           backward?"timer backward step":"timer forward step");
   require(frames.load()==beforeFrame+1 && timerFrames==expected,
           "timer step skipped or counted a frame twice");
   require(paused,"timer step left explicit pause");
  };
  const int frozen=frames.load(),frozenEvents=timerEvents;
  QElapsedTimer idle;idle.start();
  while(idle.elapsed()<100){QCoreApplication::processEvents();QThread::msleep(2);}
  require(frames.load()==frozen && timerEvents==frozenEvents && timerFrames==0,
          "paused timer advanced without an emulated frame");
  step(false,1);const quint64 oneUs=timerUs;
  require(oneUs>=16000 && oneUs<=17000,"timer did not use emulated NTSC frame duration");
  step(false,2);const quint64 savedUs=timerUs;
  require(savedUs>oneUs && savedUs<2*oneUs+2,"timer duration did not follow frame count");
  const auto savedPath=temp.filePath("timer.krs");
  const int saved=actions+1;session.saveStateToFile(savedPath);
  waitFor([&]{return actions>=saved && QFile::exists(savedPath);},"timer state save");
  step(false,3);step(false,4);
  step(true,3);step(true,2);
  require(timerUs==savedUs,"backward step did not restore elapsed time");
  step(false,3);
  const int restored=actions+1,beforeRestore=timerEvents;
  session.loadStateFromFile(savedPath);
  waitFor([&]{return actions>=restored && timerEvents>beforeRestore && timerFrames==2;},
          "timer state restore");
  require(timerUs==savedUs && paused,"state restore did not restore timer and pause");
  step(false,3);
  reset(false);step(false,1);
  reset(true);
  require(errors==0,"timer session reported a core error");
  session.shutdown();
  std::puts("Run timer: paused hold, frame steps, reverse, state restore and resets passed");
  return 0;
 }
 if(rewindResetsOnly){
  struct Shot { QByteArray state;QImage frame;double aspect=0; };
  int serial=0;
  const auto state=[&]{
   const auto target=temp.filePath(QStringLiteral("reset-%1.krs").arg(serial++));
   const int completed=actions+1;session.saveStateToFile(target);
   waitFor([&]{return actions>=completed && QFile::exists(target);},"rewind reset state save");
   QFile saved(target);require(saved.open(QIODevice::ReadOnly),"rewind reset state read");return saved.readAll();
  };
  QImage image;double aspect=0;
  const auto step=[&](bool backwards){
   session.takeVideoFrame(image,aspect);const int before=frames.load();
   if(backwards)session.backFrame();else session.advanceFrame();
   waitFor([&]{return frames.load()>before;},"rewind reset frame step");
   require(frames.load()==before+1 && session.takeVideoFrame(image,aspect) && paused,
           "rewind reset frame step skipped or lost pause");
  };
  session.setPaused(true);waitFor([&]{return paused;},"rewind reset initial pause");
  session.setRegion(2);session.setRemoveSpriteLimit(false);session.setBlarggFilter(0);
  step(false);step(false);
  const auto preserved=[&](const std::function<void()>& edit,const char* why,bool samePresentation=true){
   step(false);const Shot previous{state(),image,aspect};
   step(false);const auto current=state();
   edit();require(state()==current,"presentation or no-op edit changed native state");
   step(true);
   require(state()==previous.state,why);
   require(!image.isNull() && aspect>0 && aspect<8,"rewind preview has invalid geometry");
   if(samePresentation)require(image==previous.frame && aspect==previous.aspect,why);
  };
  preserved([&]{session.setRewindMemoryMiB(64);},"memory reduction cleared history");
  preserved([&]{session.setRewindMemoryMiB(256);},"memory increase cleared history");
  preserved([&]{session.setRewindMemoryMiB(16);},"minimum memory limit cleared retained history");
  preserved([&]{session.setRegion(2);},"no-op region command cleared history");
  preserved([&]{session.setRemoveSpriteLimit(false);},"no-op sprite limit cleared history");
  preserved([&]{session.setFastCdLoading(true);session.setFastCdLoading(true);},"CD policy on cartridge cleared history");
  preserved([&]{session.setAutoFixChecksum(true);session.setExpansionDevices(ExpansionMode::Connected, ExpansionMode::Connected);},"pending power options cleared history");
  preserved([&]{session.setControllerConnected(0,true);session.setControllerSixButton(0,true);
                 session.setControllerKind(0,ControllerType::Pad);session.setControllerAdapter(ControllerAdapter::None);},
            "no-op controller settings cleared history");
  preserved([&]{session.configureAchievements(false,false);},"no-op achievements configuration cleared history");
  preserved([&]{session.configureAchievements(true,false);},"softcore achievements configuration cleared history");
  preserved([&]{session.logoutAchievements();},"achievement logout cleared native history");
  preserved([&]{session.configureAchievements(false,false);},"achievement disable cleared native history");
  preserved([&]{session.setDebugMasks(1,1);session.setDebugBackdrop(0x123456);session.setDebugAudioGain(0,75);},
            "debug presentation controls cleared history",false);
  preserved([&]{session.setDebugMasks(0,0);session.setDebugBackdrop(0);session.setDebugAudioGain(0,100);},
            "restoring debug presentation controls cleared history",false);
  const int plainWidth=image.width();
  preserved([&]{session.setBlarggFilter(2);},"Blargg filter cleared history before its first frame",false);
  require(image.width()!=plainWidth,"reverse did not regenerate the current Blargg filter");
  step(false);require(image.width()!=plainWidth,"Blargg filter did not apply after reverse");
  const Shot filtered{state(),image,aspect};
  step(true);require(image.size()==filtered.frame.size() && aspect==filtered.aspect,
                     "reverse did not preserve current filter geometry");
  step(false);require(image==filtered.frame && aspect==filtered.aspect && state()==filtered.state,
                      "filter transition changed native state or forward branch");
  preserved([&]{session.setBlarggFilter(0);},"disabling Blargg cleared filtered history",false);
  require(image.width()==plainWidth,"reverse did not regenerate with Blargg disabled");
  step(false);require(image.width()==plainWidth,"Blargg filter did not disable after reverse");
  const int expectedErrors=errors+1;
  preserved([&]{session.changeDisc(temp.filePath("missing.cue"));},"failed media validation cleared history");
  require(errors==expectedErrors,"failed media validation was not reported");
  const auto cleared=[&](const std::function<void()>& edit,const char* why){
   step(false);step(false);edit();state();const int before=frames.load();
   session.backFrame();state();require(frames.load()==before,why);
  };
  cleared([&]{session.resetGame();},"reset retained old history");
  cleared([&]{session.setControllerConnected(1,true);},"controller topology change retained old history");
  cleared([&]{session.setRemoveSpriteLimit(true);},"sprite-limit change retained old history");
  cleared([&]{session.setRegion(3);},"region change retained old history");
  session.setRewindEnabled(false);state();step(false);
  preserved([&]{session.setRewindMemoryMiB(32);},"memory increase cleared manual frame-back history");
  preserved([&]{session.setRewindMemoryMiB(16);},"memory reduction cleared manual frame-back history");
  require(errors==expectedErrors,"rewind reset scenario emitted an unexpected error");
  session.shutdown();
  std::puts("Rewind resets: memory, no-ops, presentation/filter geometry, softcore, media validation and real boundaries passed");
  return 0;
 }
 if(frameBackOnly){
  struct Shot { QByteArray state;QImage frame;double aspect=0; };
  const auto read=[&](const QString& target){
   QFile saved(target);require(saved.open(QIODevice::ReadOnly),"frame-back state read");
   return saved.readAll();
  };
  int serial=0;
  const auto state=[&]{
   const auto target=temp.filePath(QStringLiteral("reverse-%1.krs").arg(serial++));
   const int completed=actions+1;session.saveStateToFile(target);
   waitFor([&]{return actions>=completed && QFile::exists(target);},"frame-back state save timed out");
   return read(target);
  };
  QImage image;double aspect=0;
  const auto take=[&]{
   require(session.takeVideoFrame(image,aspect) && !image.isNull(),"frame-back image missing");
  };
  const auto step=[&](bool backwards){
   session.takeVideoFrame(image,aspect);const int before=frames.load();
   if(backwards)session.backFrame();else session.advanceFrame();
   waitFor([&]{return frames.load()>before;},"frame-back step timed out");
   require(frames.load()==before+1,"single frame command published multiple frames");
   take();require(paused,"frame command left explicit pause");
  };
  const auto compare=[&](const Shot& expected,const char* why){
   require(image==expected.frame && aspect==expected.aspect,why);
   require(state()==expected.state,why);
  };
  session.setPaused(true);waitFor([&]{return paused;},"frame-back initial pause");
  session.takeVideoFrame(image,aspect);

  // Save reference states on the worker's publication boundary while it really
  // runs at normal speed. These commands are drained before its next runFrame.
  std::array<Shot,6> live;
  std::array<QString,6> livePaths;
  for(int i=0;i<int(livePaths.size());++i)
   livePaths[size_t(i)]=temp.filePath(QStringLiteral("live-%1.krs").arg(i));
  std::atomic<int> liveCount{0};std::atomic<bool> captureLive{false},pauseNext{false};
  const auto liveConnection=QObject::connect(&session,&EmulationSession::videoFrame,&app,
   [&](const QImage& frame,double frameAspect){
    if(pauseNext.exchange(false))session.setPaused(true);
    if(!captureLive.load())return;
    const int i=liveCount.fetch_add(1);
    if(i>=int(live.size()))return;
    live[size_t(i)].frame=frame;live[size_t(i)].aspect=frameAspect;
    session.saveStateToFile(livePaths[size_t(i)]);
    if(i+1==int(live.size())){captureLive=false;session.setPaused(true);}
   },Qt::DirectConnection);
  const int liveActions=actions+int(live.size());
  captureLive=true;session.setPaused(false);
  waitFor([&]{return paused && liveCount.load()==int(live.size()) && actions>=liveActions;},
          "realtime frame-back references timed out");
  take();
  for(int i=0;i<int(live.size());++i)live[size_t(i)].state=read(livePaths[size_t(i)]);
  require(live.front().frame!=live.back().frame,"frame-back fixture pictures are static");
  compare(live.back(),"realtime final reference does not match pause boundary");
  for(int i=int(live.size())-2;i>=0;--i){
   step(true);compare(live[size_t(i)],"realtime reverse skipped or mismatched a frame");
  }

  // Manual frame editing must work even when ordinary rewind recording is off.
  session.setRewindEnabled(false);state();QThread::msleep(12);
  std::array<Shot,5> manual;
  manual[0]={state(),image,aspect};
  for(int i=1;i<int(manual.size());++i){
   session.setButtons(0,unsigned((i&1)?1u<<4:1u<<5));step(false);
   manual[size_t(i)]={state(),image,aspect};
  }
  session.setButtons(0,1u<<7); // Live input differs from every saved frame.
  for(int i=int(manual.size())-2;i>=0;--i){
   step(true);compare(manual[size_t(i)],"disabled-rewind manual history lost an exact frame");
  }
  const int oldest=frames.load();session.backFrame();state();
  require(frames.load()==oldest,"manual reverse passed its oldest frame");
  session.setButtons(0,1u<<6);step(false);
  require(state()!=manual[1].state && image!=manual[1].frame,
          "divergent input reused the discarded future state or picture");
  step(true);compare(manual[0],"manual branch did not return to its exact origin");

  // A background pause alone must not authorize manual frame edits.
  session.setBackgroundPaused(true);session.setPaused(false);state();
  const auto backgroundState=state();const int backgroundFrames=frames.load();
  session.backFrame();session.advanceFrame();session.setFrameBackHeld(true);
  QElapsedTimer held;held.start();
  while(held.elapsed()<420){QCoreApplication::processEvents();QThread::msleep(2);}
  session.setFrameBackHeld(false);require(state()==backgroundState && frames.load()==backgroundFrames,
          "background pause authorized manual frame edits");
  session.setPaused(true);session.setBackgroundPaused(false);state();
  require(paused,"explicit pause was lost after background-pause guard");

  // Movie snapshots include cursor, native/battery state and preview. Returning
  // and branching must update all three while ordinary rewind remains disabled.
  std::atomic<int> movieMode{0};
  QObject::connect(&session,&EmulationSession::movieStatusChanged,&app,
   [&](int mode,const QString&,quint64,quint64,double){movieMode=mode;},Qt::DirectConnection);
  const auto moviePath=temp.filePath("reverse.krm");
  pauseNext=true;const int movieStart=frames.load();session.startMovieRecording(moviePath);
  waitFor([&]{return movieMode.load()==1 && paused && frames.load()>movieStart;},"frame-back movie start");
  // The queued pause must have completed, rather than observing its old value.
  state();waitFor([&]{return paused;},"frame-back movie pause");take();
  const auto cursor=[&](const QByteArray& saved){
   require(saved.startsWith("KRMSLOT1") && saved.size()>76,"movie frame-back snapshot format");
   const auto native=qFromBigEndian<quint32>(saved.constData()+40);
   const qsizetype prefix=76+qsizetype(native);
   require(prefix+20<=saved.size() && saved.mid(prefix,8)=="KRMCHK01","movie frame-back checkpoint");
   return qFromLittleEndian<quint64>(saved.constData()+prefix+12);
  };
  std::array<Shot,5> movie;
  movie[0]={state(),image,aspect};const quint64 initialCursor=cursor(movie[0].state);
  for(int i=1;i<int(movie.size());++i){
   session.setButtons(0,unsigned((i&1)?1u<<4:1u<<5));step(false);
   movie[size_t(i)]={state(),image,aspect};
   require(cursor(movie[size_t(i)].state)==initialCursor+quint64(i),"movie manual cursor skipped a frame");
  }
  session.setButtons(0,1u<<7);
  for(int i=3;i>=1;--i){
   step(true);compare(movie[size_t(i)],"movie backward state, preview or cursor mismatch");
  }
  session.setButtons(0,1u<<6);step(false);
  const auto branch=state();
  require(cursor(branch)==initialCursor+2 && branch!=movie[2].state,"movie backward branch cursor or input mismatch");
  session.stopMovie();waitFor([&]{return movieMode.load()==0;},"frame-back movie stop");
  MovieTape tape;QString error;require(tape.load(moviePath,error),qPrintable(error));
  require(tape.size()==initialCursor+2 && tape.resumeFrame()==tape.size(),"movie retained the discarded future");
  MovieInput input;require(tape.inputAt(tape.size()-1,input) && input.buttons[0]==(1u<<6),
                          "movie branch did not record divergent input");
  // Replay must retain the tape's input while live controls disagree. Picture
  // regeneration must neither append frames nor move the movie's cursor.
  require(tape.size()>=3,"movie replay fixture too short");
  session.setButtons(0,1u<<7);pauseNext=true;
  session.playMovie(moviePath);
  waitFor([&]{return movieMode.load()==2 && paused;},"frame-back playback start");
  state();take();
  std::array<Shot,3> playback;
  playback[0]={state(),image,aspect};
  for(int i=1;i<int(playback.size());++i){
   step(false);playback[size_t(i)]={state(),image,aspect};
  }
  for(int i=int(playback.size())-2;i>=0;--i){
   step(true);compare(playback[size_t(i)],"playback reverse used live input or changed the movie cursor");
  }
  session.stopMovie();waitFor([&]{return movieMode.load()==0;},"frame-back playback stop");
  MovieTape unchanged;require(unchanged.load(moviePath,error) && unchanged.size()==tape.size(),
                              "playback preview modified the recorded timeline");
  QObject::disconnect(liveConnection);
  require(errors==0,"frame-back worker reported a core error");session.shutdown();
  std::puts("Frame back: realtime exact history, disabled-rewind manual steps, branches, pause guard and movie cursor passed");
  return 0;
 }
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
  session.setPaused(true);waitFor([&]{return paused;});
  const int pausedFrames=frames.load(),completed=actions+1;
  session.setFastForward(true);session.saveStateToFile(temp.filePath("fast-paused.krs"));
  waitFor([&]{return actions>=completed;},"paused fast-forward toggle acknowledgement");
  require(frames==pausedFrames,"paused fast-forward executed a frame");
  session.setFastForward(false);session.backFrame();
  waitFor([&]{return frames>pausedFrames;},"paused fast-forward toggle discarded history");
  require(frames==pausedFrames+1 && paused,"paused fast-forward toggle lost exact backward step");
  session.setFastForwardFactor(0);session.setFastForward(true);session.setPaused(false);
  const int initial=frames;waitFor([&]{return frames>=initial+8;});
  session.setPaused(true);waitFor([&]{return paused;});
  session.setFastForward(false);session.setRewind(true);
  const int boundary=frames;QThread::msleep(100);QCoreApplication::processEvents();
  require(frames==boundary && !reversed,"rewind crossed fast-forward gap or recorded accelerated frames");
  session.setRewind(false);session.setPaused(false);waitFor([&]{return !paused && frames>=boundary+8;});
  session.setPaused(true);waitFor([&]{return paused;});
  const int beforeReverse=frames.load();
  session.setRewind(true);
  waitFor([&]{return reversed.load() && frames>beforeReverse;},"rewind did not resume after fast-forward");
  session.setRewind(false);session.shutdown();require(errors==0,"fast-forward session error");
  std::puts("Paused fast-forward preserves history; executed gap clears history; normal recording and rewind resume: passed");
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
