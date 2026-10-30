#pragma once

// Chart seconds are integrated from the audio DSP clock, never from frame count.
// No chart timestamps or event data are rescaled.
struct PracticeSpeedClock {
    double anchorDsp=0, anchorChart=0;
    float rate=1;
    bool running=false;
    static bool validRate(float value) { return value>=.5f && value<=1.5f; }
    double at(double dsp) const {
        return anchorChart+(running && dsp>anchorDsp ? (dsp-anchorDsp)*rate : 0);
    }
    void seek(double chart,double dsp,bool play) {
        anchorChart=chart;anchorDsp=dsp;running=play;
    }
    void change(float value,double dsp) {
        if(!running || dsp>=anchorDsp) {anchorChart=at(dsp);anchorDsp=dsp;}
        rate=value;
    }
    void pause(double dsp) { anchorChart=at(dsp);anchorDsp=dsp;running=false; }
};
