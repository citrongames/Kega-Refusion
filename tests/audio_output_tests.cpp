#include "ui/AudioOutput.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
thread_local bool producerCallback = false;
void require(bool okay,const char* text) { if(!okay)throw std::runtime_error(text); }
template<class Predicate> void waitWithoutGui(Predicate predicate) {
    QElapsedTimer timer;timer.start();
    while(!predicate()&&timer.elapsed()<2000)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(predicate(),"audio worker did not make progress without GUI events");
}
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    try {
        std::atomic<int> workerFeedback{0},producerInvalidations{0};
        const auto gui=std::this_thread::get_id();
        AudioOutput audio;
        audio.setVolume(0);
        QObject::connect(&audio,&AudioOutput::clockFeedback,&app,
            [&](qint64,qint64,bool available) {
                if(available)return;
                const auto caller=std::this_thread::get_id();
                if(producerCallback)++producerInvalidations;
                else if(caller!=gui)++workerFeedback;
            },Qt::DirectConnection);
        // Stay below prefill: exercise a real worker without opening any device.
        audio.play(QByteArray(4,'x'),44100);
        waitWithoutGui([&]{return workerFeedback.load()>0;});
        const int before=workerFeedback.load();
        audio.clear();
        waitWithoutGui([&]{return workerFeedback.load()>before;});
        std::thread producer([&]{
            producerCallback=true;
            audio.play(QByteArray(4,'x'),48000);
            audio.clear();
            producerCallback=false;
        });
        producer.join();
        require(producerInvalidations==2,"producer invalidation was deferred through GUI");
        const QByteArray missing("kega-test-missing-device");
        audio.setOutputDeviceId(missing);
        require(audio.outputDeviceId()==missing,"missing device preference was discarded");
        audio.setOutputDeviceId({});
        require(audio.outputDeviceId().isEmpty(),"default device preference not restored");
        for(int profile:{1,2,0}) {
            audio.setLatencyProfile(AudioQueue::normalizeProfile(profile));
            require(int(audio.latencyProfile())==profile,"profile not updated");
        }
        audio.setMuted(true);audio.setVolume(37);audio.setMuted(false);
        // A concurrent producer may finish its call, but may not dereference a
        // deleted worker after shutdown. No GUI event pumping occurs here.
        std::atomic<bool> running{true};
        std::thread racing([&]{while(running.load()){audio.play({},44100);audio.clear();std::this_thread::yield();}});
        audio.shutdown();running=false;racing.join();
        audio.shutdown();audio.play(QByteArray(4,'x'),44100);audio.clear();
        for(int i=0;i<8;++i) {
            AudioOutput pending;
            pending.setOutputDeviceId(missing);
            pending.setLatencyProfile(AudioQueue::LatencyProfile::Stable);
            pending.play(QByteArray(4,'x'),44100);
            // Immediate destruction drains no audio and joins all worker threads.
        }
        std::puts("audio output tests passed: worker independent of GUI, direct invalidation, controls, concurrent shutdown and repeated lifecycle");
    } catch(const std::exception& e) {
        std::fprintf(stderr,"audio output tests failed: %s\n",e.what());return 1;
    }
}
