
// All engine mutations run on the Unity Update thread. UI only queues commands.
std::atomic<bool> recording{false}, practicing{false};
std::atomic<int> practiceCommand{0};
std::atomic<float> requestedTime{0}, currentSongTime{0}, songLength{0}, practiceStart{0};
float segmentStart=0;
std::atomic<bool> resultScene{false}, restartPending{false};
std::u16string resultObjectName;
struct Mistake { float time, code, delta; int grade; };
std::vector<Mistake> mistakes;
std::unordered_set<int> mistakeCodes;
std::mutex practiceLock;
std::atomic<int> recordRevision{0};
std::unordered_map<int,float> noteTimeIndex;
void* indexedProgress=nullptr;void* indexedNotes=nullptr;int indexedNoteCount=-1;
uintptr_t practiceBase=0;
void* practiceOwner=nullptr;
using GoodFn=void(*)(void*,float,float,Vec3,bool,void*);
using BadFn=void(*)(void*,float,float,void*);
using MissFn=void(*)(void*,float,void*);
using CreateFn=void(*)(void*,int,bool,void*);
using SaveRecordFn=void(*)(void*,void*,void*,void*,void*);
GoodFn originalGood; BadFn originalBad; MissFn originalMiss;
CreateFn originalCreate; SaveRecordFn originalSaveRecord;
using ResultStartFn=void*(*)(void*,void*);
ResultStartFn originalResultStart;
void* practiceResultStart(void* self,void* method) {
    auto obj=reinterpret_cast<void*(*)(void*,void*)>(practiceBase+0x3c567e0)(self,nullptr);
    auto name=reinterpret_cast<void*(*)(void*,void*)>(practiceBase+0x3c5d7e8)(obj,nullptr);
    {std::lock_guard<std::mutex> guard(practiceLock);int len=field<int>(name,0x10);if(len>0 && len<512)resultObjectName.assign(reinterpret_cast<char16_t*>(static_cast<char*>(name)+0x14),len);}
    resultScene=true;
    return originalResultStart(self,method);
}

template<class T> void practiceStore(void* p,size_t at,T value) { if(p) std::memcpy(static_cast<char*>(p)+at,&value,sizeof(value)); }
#include "speed_engine.inc"
int listSize(void* list) { int n=field<int>(list,0x18); return n>=0 && n<100000?n:0; }
void* listItem(void* list,int i) { return i>=0 && i<listSize(list)?field<void*>(field<void*>(list,0x10),0x20+8*i):nullptr; }
void indexNoteTimes(void* progress) {
    void* notes=field<void*>(field<void*>(progress,0x20),0x38);
    int count=listSize(notes);
    if(indexedProgress==progress && indexedNotes==notes && indexedNoteCount==count) return;
    noteTimeIndex.clear();noteTimeIndex.reserve(count);
    for(int i=0;i<count;++i) {
        void* n=listItem(notes,i);if(!n)continue;
        float code=field<float>(n,0x38),time=field<float>(n,0x2c);int key;
        if(!std::isfinite(code) || !std::isfinite(time))continue;
        std::memcpy(&key,&code,sizeof(key));noteTimeIndex.emplace(key,time);
    }
    indexedProgress=progress;indexedNotes=notes;indexedNoteCount=count;
}
void addMistake(void* score,float code,float delta,int grade) {
    if(!recording || practicing || !std::isfinite(code)) return;
    void* progress=field<void*>(score,0x90);
    float when=field<float>(progress,0x90);
    int key=0; std::memcpy(&key,&code,sizeof(key));
    // Build once per chart; every subsequent callback resolves its note in O(1).
    indexNoteTimes(progress);
    auto found=noteTimeIndex.find(key);if(found!=noteTimeIndex.end())when=found->second;
    if(!std::isfinite(when)) return;
    std::lock_guard<std::mutex> guard(practiceLock);
    if(mistakes.size()<20000 && mistakeCodes.insert(key).second) {
        mistakes.push_back({when,code,delta,grade});++recordRevision;
    }
}
void practiceGood(void* s,float code,float delta,Vec3 point,bool isHold,void* m) { originalGood(s,code,delta,point,isHold,m);addMistake(s,code,delta,1); }
void practiceBad(void* s,float code,float delta,void* m) { originalBad(s,code,delta,m);addMistake(s,code,delta,2); }
void practiceMiss(void* s,float code,void* m) { originalMiss(s,code,m);addMistake(s,code,0,3); }
void practiceSaveRecord(void* self,void* song,void* level,void* rec,void* m) { if(!practicing) originalSaveRecord(self,song,level,rec,m); }
void practiceCreate(void* line,int index,bool above,void* method) {
    void* n=listItem(field<void*>(line,above?0xa0:0xa8),index);
    if(practicing && n && field<float>(n,0x2c)<segmentStart-.001f) return;
    originalCreate(line,index,above,method);
}
bool validPracticeStart(float start,float length) {
    return std::isfinite(start) && start>=0 && (!std::isfinite(length) || length<=0 || start<length);
}
bool rebuildAt(void* progress,float target) {
    void* info=field<void*>(progress,0x20); void* judge=field<void*>(progress,0x30);
    void* audio=field<void*>(progress,0x38); void* manager=field<void*>(judge,0x90);
    if(!info || !judge || !audio || !manager || !field<bool>(info,0x68) || field<bool>(progress,0xa6)) return false;
    float length=field<float>(progress,0x94);
    if(!std::isfinite(target)||!std::isfinite(length)||length<=0) return false;
    target=std::max(0.f,std::min(std::max(0.f,length-.1f),target));
    // Seeking is exact. Holds whose heads precede the start are skipped, never rewind the target.
    void* notes=field<void*>(info,0x38);
    practicing=true; segmentStart=target; practiceStart=target;
    // Prevent the original result/record coroutine for a partial run.
    practiceStore(info,0x6b,false);practiceStore(info,0x6a,false);
    practiceStore(progress,0xa0,-1.e9f);
    for(size_t offset:{size_t(0x38),size_t(0x40),size_t(0x48),size_t(0x50)}) {
        void* list=field<void*>(manager,offset); void* array=field<void*>(list,0x10);
        int count=listSize(list);
        for(int i=0;i<count;i++) {
            void* control=listItem(list,i);
            if(control) {
                auto getObject=reinterpret_cast<void*(*)(void*,void*)>(practiceBase+0x3c567e0);
                auto destroy=reinterpret_cast<void(*)(void*,void*)>(practiceBase+0x3c5e324);
                destroy(getObject(control,nullptr),nullptr);
            }
            practiceStore(array,0x20+i*8,static_cast<void*>(nullptr));
        }
        practiceStore(list,0x18,0);practiceStore(list,0x1c,field<int>(list,0x1c)+1);
    }
    int first=0;
    for(int i=0;i<listSize(notes);i++) {
        void* n=listItem(notes,i); bool past=field<float>(n,0x2c)<target;
        practiceStore(n,0x28,past);practiceStore(n,0x29,past);
        if(past) first=i+1;
    }
    practiceStore(judge,0x80,first);practiceStore(judge,0x84,first);
    void* lines=field<void*>(judge,0x30);
    for(int i=0;i<listSize(lines);i++) {
        void* line=listItem(lines,i);
        for(size_t at:{size_t(0xec),size_t(0xf0),size_t(0xf4),size_t(0xf8)}) practiceStore(line,at,0);
        practiceStore(line,0x104,-1);practiceStore(line,0x108,-1);
    }
    void* score=field<void*>(progress,0x28);
    for(size_t at:{size_t(0x40),size_t(0x44),size_t(0x4c),size_t(0x54),size_t(0x58),size_t(0x5c),size_t(0x60),size_t(0x64),size_t(0x68),size_t(0x6c)}) practiceStore(score,at,0);
    practiceStore(score,0x50,false);practiceStore(score,0x51,false);
    reinterpret_cast<void(*)(void*,bool,void*)>(practiceBase+0x1d859d0)(progress,true,nullptr);
    reinterpret_cast<void(*)(void*,bool,void*)>(practiceBase+0x3c597b0)(field<void*>(progress,0x40),false,nullptr);
    reinterpret_cast<void(*)(void*,bool,void*)>(practiceBase+0x3c55d24)(field<void*>(progress,0x50),false,nullptr);
    practiceStore(progress,0x9c,0.f);practiceStore(progress,0xa4,false);
    practiceStore(info,0x69,true);
    currentSongTime=target;
    practiceStore(progress,0x90,target);practiceStore(progress,0x8a,true);
    practiceStore(progress,0x8b,true);practiceStore(progress,0x8c,true);
    float seek=target+field<float>(progress,0x98);
    if(!field<bool>(audio,0x44)) reinterpret_cast<void(*)(void*,void*)>(practiceBase+0x1db5668)(audio,nullptr);
    reinterpret_cast<void(*)(void*,float,void*)>(practiceBase+0x1db5b90)(audio,seek,nullptr);
    reinterpret_cast<void(*)(void*,float,void*)>(practiceBase+0x1db5500)(audio,field<float>(info,0x70),nullptr);
    reset(progress);
    __android_log_print(ANDROID_LOG_INFO,"PhigrosPractice","Practice seek %.3f (no end boundary)",segmentStart);
    return true;
}
void practiceUpdate(void* progress) {
    if(practiceOwner!=progress) {
        practiceOwner=progress; resultScene=false;
        indexedProgress=nullptr;indexedNotes=nullptr;indexedNoteCount=-1;noteTimeIndex.clear();
        if(!restartPending) {practicing=false;practiceStart=0;}
        if(!restartPending) {std::lock_guard<std::mutex> guard(practiceLock);mistakes.clear();mistakeCodes.clear();++recordRevision;}
    }
    currentSongTime=field<float>(progress,0x90);
    songLength=field<float>(progress,0x94);
    if(field<bool>(field<void*>(progress,0x20),0x68) && field<bool>(field<void*>(progress,0x38),0x3c)
       && !field<bool>(progress,0xa6) && std::fabs(field<float>(progress,0x98)+100.f)>.1f)
        updatePracticeSpeed(progress);
    if(recording && !practicing && field<bool>(field<void*>(progress,0x20),0x68)) indexNoteTimes(progress);
    int command=practiceCommand.exchange(0);
    if(command==1) restartPending=true;
    if(restartPending && field<bool>(field<void*>(progress,0x20),0x68) && field<bool>(field<void*>(progress,0x38),0x3c) && std::fabs(field<float>(progress,0x98)+100.f)>.1f) {
        if(rebuildAt(progress,requestedTime.load())) restartPending=false;
    }
    if(practicing) {
        practiceStore(field<void*>(progress,0x20),0x6b,false);
        practiceStore(field<void*>(progress,0x20),0x6a,false);
        practiceStore(progress,0xa0,-1.e9f);
        // Stop only at the song's end; manual restart returns to practiceStart.
        if(field<bool>(progress,0x8a) && songLength>0 && currentSongTime>=songLength)
            reinterpret_cast<void(*)(void*,bool,void*)>(practiceBase+0x1d859d0)(progress,false,nullptr);
    }

    static auto diagnosticAt=Clock::time_point{};
    if(Clock::now()-diagnosticAt>std::chrono::seconds(5)) {
        diagnosticAt=Clock::now();
        void* info=field<void*>(progress,0x20);
        __android_log_print(ANDROID_LOG_INFO,"PhigrosPractice","state time=%.3f recording=%d practice=%d pending=%d playing=%d prepared=%d loaded=%d began=%d offset=%.3f notes=%d",currentSongTime.load(),int(recording.load()),int(practicing.load()),int(restartPending.load()),field<bool>(progress,0x8a),field<bool>(progress,0x8b),field<bool>(info,0x68),field<bool>(info,0x69),field<float>(progress,0x98),listSize(field<void*>(info,0x38)));
    }
}
