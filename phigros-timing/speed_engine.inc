// Only the current gameplay music source is changed. Menus and other audio
// sources retain their original Update, pitch and timekeeping.
std::atomic<float> desiredSpeed{1}, activeSpeed{1};
void* speedAudio=nullptr;
bool speedSession=false;
PracticeSpeedClock speedClock;
float speedRawTime=0;
using DspTimeFn=double(*)(void*);
using PitchFn=void(*)(void*,float,void*);
using AudioSeekFn=void(*)(void*,float,void*);
using AudioScheduleFn=void(*)(void*,double,void*);
using AudioLifecycleFn=void(*)(void*,void*);
DspTimeFn speedDspTime=nullptr;
PitchFn speedPitch=nullptr;
AudioLifecycleFn originalAudioUpdate,originalAudioPause,originalAudioDisable;
AudioSeekFn originalAudioSeek;
AudioScheduleFn originalAudioSchedule;

void applyMusicPitch(void* audio,float rate) {
    void* source=field<void*>(audio,0x30);
    if(source && speedPitch) speedPitch(source,rate,nullptr);
}
void speedAudioUpdate(void* self,void* method) {
    // Retain the game's existing DSP/monotonic smoothing. Its private internal
    // clock advances at 1x independently of source pitch; only the public time
    // consumed by ProgressControl is mapped to the requested practice rate.
    originalAudioUpdate(self,method);
    if(self!=speedAudio || !speedSession) return;
    speedRawTime=field<float>(self,0x40);
    if(field<bool>(self,0x44) && !speedClock.running) {
        float start=field<float>(self,0x38);
        speedClock.seek(start,start,true);
        applyMusicPitch(self,activeSpeed.load());
    }
    float time=field<bool>(self,0x44)?float(speedClock.at(speedRawTime)):field<float>(self,0x38);
    practiceStore(self,0x40,time);
}
void speedAudioSeek(void* self,float target,void* method) {
    originalAudioSeek(self,target,method);
    if(self==speedAudio && speedSession) {
        speedRawTime=field<float>(self,0x40);
        speedClock.seek(speedRawTime,speedRawTime,field<bool>(self,0x44));
    }
}
void speedAudioSchedule(void* self,double wait,void* method) {
    if(self==speedAudio && speedSession) applyMusicPitch(self,activeSpeed.load());
    originalAudioSchedule(self,wait,method);
    if(self==speedAudio && speedSession) {
        speedRawTime=field<float>(self,0x38);
        speedClock.seek(speedRawTime,speedRawTime,true);
    }
}
void speedAudioPause(void* self,void* method) {
    bool tracked=self==speedAudio && speedSession;
    float position=0;
    if(tracked) position=field<float>(self,0x40);
    originalAudioPause(self,method);
    if(tracked) {practiceStore(self,0x38,position);practiceStore(self,0x40,position);speedRawTime=position;speedClock.seek(position,position,false);}
}
void speedAudioDisable(void* self,void* method) {
    if(self==speedAudio) {
        applyMusicPitch(self,1);speedAudio=nullptr;speedSession=false;activeSpeed=1;
        speedClock=PracticeSpeedClock{};
    }
    originalAudioDisable(self,method);
}
void updatePracticeSpeed(void* progress) {
    void* audio=field<void*>(progress,0x38);
    float wanted=desiredSpeed.load();
    if(!audio || !speedDspTime || !speedPitch || (!speedSession && wanted==1)) return;
    if(audio==speedAudio && speedSession && wanted==activeSpeed.load()) return;
    float raw=speedRawTime;
    if(audio!=speedAudio || !speedSession) {
        speedAudio=audio;speedSession=true;
        speedClock=PracticeSpeedClock{};
        raw=field<float>(audio,0x40);speedRawTime=raw;
        speedClock.seek(raw,raw,field<bool>(audio,0x44));
    }
    speedClock.change(wanted,raw);activeSpeed=wanted;
    applyMusicPitch(audio,wanted);
    // Once a run has used a changed rate it remains practice even at 1x.
    if(!practicing) {segmentStart=0;practiceStart=std::max(0.f,currentSongTime.load());}
    practicing=true;
    __android_log_print(ANDROID_LOG_INFO,"PhigrosPractice","Practice speed %.2fx, chart %.3f",wanted,float(speedClock.at(raw)));
}
