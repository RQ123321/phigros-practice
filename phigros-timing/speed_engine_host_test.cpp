// Single-threaded host harness for the production engine callbacks. The tiny
// atomic substitute only isolates state-machine tests; it does not test threads.
#include "speed_clock.h"
extern "C" void* memset(void* p,int value,unsigned size) {
    volatile unsigned char* out=static_cast<unsigned char*>(p);
    for(unsigned i=0;i<size;++i) out[i]=static_cast<unsigned char>(value);
    return p;
}
namespace std {
template<class T> struct atomic {
    T value;
    atomic(T v):value(v) {}
    T load() const {return value;}
    operator T() const {return value;}
    atomic& operator=(T v) {value=v;return *this;}
};
template<class T> T max(T a,T b) {return a>b?a:b;}
}
template<class T> T field(void* p,unsigned at) {
    T value{};if(p)__builtin_memcpy(&value,static_cast<char*>(p)+at,sizeof(T));return value;
}
template<class T> void practiceStore(void* p,unsigned at,T value) {
    if(p)__builtin_memcpy(static_cast<char*>(p)+at,&value,sizeof(T));
}
std::atomic<bool> practicing{false};
std::atomic<float> practiceStart{0},currentSongTime{0};
float segmentStart=0;
#define ANDROID_LOG_INFO 4
int __android_log_print(int,const char*,const char*,...) {return 0;}
#include "speed_engine.inc"

double testDsp=100;
float testRaw=10;
float testPitch=1;
int forwardedUpdates=0,forwardedDisables=0;
double fakeDsp(void*) {return testDsp;}
void fakePitch(void*,float rate,void*) {testPitch=rate;}
void fakeUpdate(void* p,void*) {++forwardedUpdates;practiceStore(p,0x40,testRaw);}
void fakeSeek(void* p,float t,void*) {practiceStore(p,0x38,t);practiceStore(p,0x40,t);}
void fakePause(void* p,void*) {practiceStore(p,0x44,false);}
void fakeSchedule(void* p,double wait,void*) {
    practiceStore(p,0x48,testDsp+wait);practiceStore(p,0x44,false);
}
void fakeDisable(void*,void*) {++forwardedDisables;}
bool same(float a,float b) {return a-b<.0001f && a-b>-.0001f;}
extern "C" int runSpeedEngineTests() {
    alignas(16) char audio[256]{},source[64]{},progress[256]{};
    speedDspTime=fakeDsp;speedPitch=fakePitch;
    originalAudioUpdate=fakeUpdate;originalAudioSeek=fakeSeek;originalAudioPause=fakePause;
    originalAudioSchedule=fakeSchedule;originalAudioDisable=fakeDisable;
    practiceStore(audio,0x30,static_cast<void*>(source));
    practiceStore(progress,0x38,static_cast<void*>(audio));
    speedAudioUpdate(audio,nullptr);
    if(forwardedUpdates!=1) return 1;
    practiceStore(audio,0x40,10.f);practiceStore(audio,0x44,true);
    currentSongTime=10;desiredSpeed=.75f;updatePracticeSpeed(progress);
    testRaw=14;speedAudioUpdate(audio,nullptr);
    if(!practicing || !same(testPitch,.75f) || !same(field<float>(audio,0x40),13)) return 2;
    speedAudioPause(audio,nullptr);testRaw=13;speedAudioUpdate(audio,nullptr);
    if(!same(field<float>(audio,0x40),13)) return 3;
    // A direct Play coroutine flips the flag without invoking PlayScheduled.
    practiceStore(audio,0x44,true);speedAudioUpdate(audio,nullptr);
    testRaw=17;speedAudioUpdate(audio,nullptr);
    if(!same(field<float>(audio,0x40),16)) return 4;
    speedAudioSeek(audio,6,nullptr);testRaw=8;speedAudioUpdate(audio,nullptr);
    if(!same(field<float>(audio,0x40),7.5f)) return 5;
    testDsp=300;speedAudioSeek(audio,20,nullptr);speedAudioSchedule(audio,1,nullptr);
    testRaw=20;speedAudioUpdate(audio,nullptr);
    if(!same(field<float>(audio,0x40),20)) return 6;
    practiceStore(audio,0x44,true);testRaw=21;speedAudioUpdate(audio,nullptr);
    if(!same(field<float>(audio,0x40),20.75f)) return 7;
    desiredSpeed=1;updatePracticeSpeed(progress);testRaw=23;speedAudioUpdate(audio,nullptr);
    if(!same(field<float>(audio,0x40),22.75f) || !practicing || !same(testPitch,1)) return 8;
    speedAudioDisable(audio,nullptr);speedAudioUpdate(audio,nullptr);
    if(speedSession || speedAudio || forwardedDisables!=1 || forwardedUpdates!=10 || !same(testPitch,1)) return 9;
    return 0;
}
