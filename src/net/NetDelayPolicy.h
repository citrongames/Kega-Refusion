#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Wall-clock, not emulated-frame, policy. GGPO remains responsible for queueing
// input and rolling back. This only requests a small local queue delay.
class NetDelayPolicy final {
public:
    static constexpr int MaxDelay = 4;
    int frames() const { return applied_; }
    int pending() const { return pending_; }
    void suspend() { high_=low_=0;pending_=applied_; }
    void observe(int pingMs,int jitterMs,double fps,std::int64_t nowMs,
                 bool eligible,bool quiet) {
        if(!eligible || pingMs<0 || !std::isfinite(fps) || fps<=0) {suspend();return;}
        // Keep two frames of prediction rather than buffering the whole trip.
        const double transit=std::clamp(pingMs,0,10000)*0.5+
                             2.0*std::clamp(jitterMs,0,1000);
        const int target=std::clamp(int(std::ceil(transit*fps/1000.0))-2,0,MaxDelay);
        pending_=applied_;
        if(target>applied_) {
            low_=0;high_=std::min(high_+1,3);
            if(high_>=3 && (lastChange_<0 || nowMs-lastChange_>=5000))pending_=applied_+1;
        } else if(target<applied_ && quiet) {
            high_=0;low_=std::min(low_+1,15);
            if(low_>=15 && (lastChange_<0 || nowMs-lastChange_>=15000))pending_=applied_-1;
        } else high_=low_=0;
    }
    void applied(int frames,std::int64_t nowMs) {
        applied_=pending_=std::clamp(frames,0,MaxDelay);lastChange_=nowMs;high_=low_=0;
    }
private:
    int applied_=0,pending_=0,high_=0,low_=0;
    std::int64_t lastChange_=-1;
};

// One update per new GGPO QualityReply, never per repeated stats poll.
class NetRttEstimate final {
public:
    void sample(int milliseconds) {
        const double value=std::clamp(milliseconds,0,10000);
        if(!valid_) {mean_=previous_=value;valid_=true;return;}
        variation_+=(std::abs(value-previous_)-variation_)*0.25;
        mean_+=(value-mean_)*0.25;previous_=value;
    }
    int pingMs() const {return valid_?int(std::lround(mean_)):-1;}
    int jitterMs() const {return valid_?int(std::lround(variation_)):0;}
private:
    bool valid_=false;
    double mean_=0,previous_=0,variation_=0;
};
