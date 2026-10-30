#include <jni.h>
#include <android/log.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstring>
#include <cmath>
#include <array>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <fstream>
#include <vector>
#include <algorithm>
#include <dlfcn.h>
#include "core_guard.h"
#include "speed_clock.h"

namespace {
using Clock = std::chrono::steady_clock;
constexpr int Capacity = 24;
constexpr int SpotCapacity = 32;
constexpr int SpotLifetimeMs = 1000;
struct Vec3 { float x, y, z; };
struct HitPoint { float x{}, y{}; bool valid{}; };
struct HitSpot { float ms{}, x{}, y{}; Clock::time_point time{}; };
std::array<HitSpot, SpotCapacity> spots{};
int spotCount = 0;
using TransformFn = void*(*)(void*, void*);
using PointFn = Vec3(*)(void*, Vec3, void*);
using ProjectFn = Vec3(*)(void*, Vec3, int, void*);
using ScreenSizeFn = int(*)(void*);
TransformFn componentTransform = nullptr;
PointFn transformPoint = nullptr;
ProjectFn worldToScreen = nullptr;
ScreenSizeFn screenWidth = nullptr, screenHeight = nullptr;
std::mutex lock;
std::atomic<int> status{0};
std::atomic<bool> captureBar{true},captureSpots{true};
std::array<float, Capacity> history{};
std::array<unsigned, 7> bins{};
int count = 0, misses = 0;
unsigned total = 0;
std::unordered_set<void*> capturedNotes;
float last = 0, previousTime = -999;
void* progressOwner = nullptr;
Clock::time_point lastFrame{};

template<class T> T field(void* object, size_t offset) {
    T value{};
    if (object) std::memcpy(&value, static_cast<char*>(object) + offset, sizeof(T));
    return value;
}
void reset(void* owner) {
    std::lock_guard<std::mutex> guard(lock);
    history.fill(0); bins.fill(0); count = misses = 0; total = 0;
    capturedNotes.clear();
    spots.fill({}); spotCount = 0;
    last = 0; previousTime = -999; progressOwner = owner;
}
void record(float seconds, void* note = nullptr, HitPoint point = {}) {
    if ((!captureBar && !captureSpots) || !std::isfinite(seconds) || std::fabs(seconds) > 0.5f) return;
    float ms = seconds * 1000.f;
    std::lock_guard<std::mutex> guard(lock);
    if (note && !capturedNotes.insert(note).second) return;
    if (count == Capacity) {
        for (int i = 1; i < Capacity; ++i) history[i - 1] = history[i];
    } else ++count;
    history[count - 1] = last = ms;
    constexpr float edges[]{-160, -80, -40, 40, 80, 160};
    int bucket = 0;
    while (bucket < 6 && ms > edges[bucket]) ++bucket;
    ++bins[bucket]; ++total;
    if (point.valid && std::isfinite(point.x) && std::isfinite(point.y) &&
        point.x >= 0 && point.x <= 1 && point.y >= 0 && point.y <= 1) {
        if (spotCount == SpotCapacity) {
            for (int i = 1; i < SpotCapacity; ++i) spots[i-1] = spots[i];
        } else ++spotCount;
        spots[spotCount-1] = {ms, point.x, point.y, Clock::now()};
    }
}

// Called only on Unity's judgement thread. Use the note's own judge line and
// local X at Y=0, the same anchor used by the original hit effect. Camera
// projection handles moving/rotating lines and render-resolution scaling.
HitPoint noteHitPoint(void* line, void* progress, float noteX) {
    if (!captureSpots || !line || !progress || !std::isfinite(noteX) || !componentTransform ||
        !transformPoint || !worldToScreen || !screenWidth || !screenHeight) return {};
    auto* camera = field<void*>(field<void*>(progress, 0x30), 0xa0);
    if (!camera) return {};
    auto* transform = componentTransform(line, nullptr);
    if (!transform) return {};
    Vec3 world = transformPoint(transform, {noteX, 0, 0}, nullptr);
    Vec3 screen = worldToScreen(camera, world, 2 /* Mono */, nullptr);
    int width = screenWidth(nullptr), height = screenHeight(nullptr);
    if (width <= 0 || height <= 0 || !std::isfinite(screen.z) || screen.z <= 0) return {};
    return {screen.x / width, 1.f - screen.y / height, true};
}

// IL2CPP Judge returns bool: the caller removes completed notes on true.
// Losing this value causes the same live note to be judged on later frames.
using JudgeFn = bool(*)(void*, void*);
using UpdateFn = void(*)(void*, void*);
JudgeFn originalClick, originalHold;
UpdateFn originalUpdate;

bool click(void* self, void* method) {
    auto* score = field<void*>(self, 0x20);
    auto* note = field<void*>(self, 0x40);
    auto* progress = field<void*>(self, 0x28);
    auto* line = field<void*>(self, 0x38);
    float noteX = field<float>(note, 0x18);
    int before = field<int>(score, 0x58) + field<int>(score, 0x5c) + field<int>(score, 0x60);
    float delta = field<float>(progress, 0x90) - field<float>(note, 0x2c);
    const bool completed = originalClick(self, method);
    int after = field<int>(score, 0x58) + field<int>(score, 0x5c) + field<int>(score, 0x60);
    if (score && note && progress && after > before)
        record(delta, note, noteHitPoint(line, progress, noteX));
    return completed;
}
bool hold(void* self, void* method) {
    bool judgedBefore = field<bool>(self, 0x8e);
    auto* note = field<void*>(self, 0x38);
    auto* progress = field<void*>(self, 0x28);
    auto* line = field<void*>(self, 0x40);
    float noteX = field<float>(note, 0x18);
    float delta = field<float>(progress, 0x90) - field<float>(note, 0x2c);
    const bool completed = originalHold(self, method);
    if (!judgedBefore && field<bool>(self, 0x8e) && note && progress)
        record(delta, note, noteHitPoint(line, progress, noteX));
    return completed;
}
#include "practice_engine.inc"

void update(void* self, void* method) {
    practiceUpdate(self);
    float now = field<float>(self, 0x90);
    // Reset before original callbacks can emit hits on a restarted timeline.
    if (progressOwner != self || now < previousTime - 0.5f) reset(self);
    originalUpdate(self, method);
    now = field<float>(self, 0x90);
    if (now < previousTime - 0.5f) reset(self);
    std::lock_guard<std::mutex> guard(lock);
    previousTime = now;
    misses = field<int>(field<void*>(self, 0x28), 0x64);
    if (!field<bool>(self, 0xa4)) lastFrame = Clock::now();
}

// Version-locked ARM64 entry trampoline. Only straight-line prologues and
// ADR/ADRP are accepted; all other PC-relative instructions are rejected.
void jump(unsigned char* out, uintptr_t target) {
    const uint32_t instructions[]{0x58000051, 0xd61f0220}; // ldr x17,+8; br x17
    std::memcpy(out, instructions, 8); std::memcpy(out + 8, &target, 8);
}
struct InstalledHook { uintptr_t target{}; unsigned char saved[16]{}; void* trampoline{}; };
bool writeEntry(uintptr_t target, const void* bytes) {
    size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    uintptr_t first = target & ~(page - 1);
    size_t length = ((target + 16 + page - 1) & ~(page - 1)) - first;
    if (mprotect((void*)first, length, PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
    std::memcpy((void*)target, bytes, 16);
    __builtin___clear_cache((char*)target, (char*)target + 16);
    return mprotect((void*)first, length, PROT_READ | PROT_EXEC) == 0;
}
template<class Fn>
bool attach(uintptr_t target, uintptr_t replacement, Fn* original, InstalledHook& hook) {
    size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto* memory = (unsigned char*)mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return false;
    std::memcpy(hook.saved, (void*)target, 16);
    size_t used = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t word;
        std::memcpy(&word, hook.saved + i * 4, 4);
        if ((word & 0x1f000000) == 0x10000000) {
            int64_t imm = ((word >> 5) & 0x7ffff) * 4 | ((word >> 29) & 3);
            if (imm & (1 << 20)) imm -= 1 << 21;
            uintptr_t pc = target + i * 4;
            uintptr_t value = (word & 0x80000000) ? (pc & ~uintptr_t(4095)) + imm * 4096 : pc + imm;
            uint32_t load = 0x58000040 | (word & 31), skip = 0x14000003;
            std::memcpy(memory + used, &load, 4); std::memcpy(memory + used + 4, &skip, 4);
            std::memcpy(memory + used + 8, &value, 8); used += 16;
        } else {
            bool relative = (word & 0x7c000000) == 0x14000000 || (word & 0xff000010) == 0x54000000 ||
                (word & 0x7e000000) == 0x34000000 || (word & 0x7e000000) == 0x36000000 ||
                (word & 0x3b000000) == 0x18000000;
            if (relative) { munmap(memory, page); return false; }
            std::memcpy(memory + used, &word, 4); used += 4;
        }
    }
    jump(memory + used, target + 16); used += 16;
    __builtin___clear_cache((char*)memory, (char*)memory + used);
    if (mprotect(memory, page, PROT_READ | PROT_EXEC)) { munmap(memory, page); return false; }
    *original = reinterpret_cast<Fn>(memory);
    unsigned char branch[16]; jump(branch, replacement);
    hook.target = target; hook.trampoline = memory;
    if (!writeEntry(target, branch)) {
        writeEntry(target, hook.saved);
        *original = nullptr; munmap(memory, page); hook = {};
        return false;
    }
    return true;
}

#include "unlock_profile.inc"

int findCore(dl_phdr_info* info, size_t, void* result) {
    if (info->dlpi_name && std::strstr(info->dlpi_name, "libil2cpp.so")) {
        *static_cast<uintptr_t*>(result) = info->dlpi_addr;
        return 1;
    }
    return 0;
}
void install() {
    uintptr_t base = 0;
    for (int i = 0; i < 1200 && !base; ++i) {
        dl_iterate_phdr(findCore, &base);
        if (!base) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!base) { status = -1; return; }
    for (const auto& entry : CoreGuards) {
        if (std::memcmp(reinterpret_cast<void*>(base + entry.offset), entry.bytes, sizeof(entry.bytes))) {
            status = -2;
            __android_log_print(ANDROID_LOG_ERROR, "PhigrosTiming", "Unsupported core: guard mismatch");
            return;
        }
    }
    unlockBase = base;
    practiceBase = base;
    componentTransform = reinterpret_cast<TransformFn>(base + 0x3c567a4);
    transformPoint = reinterpret_cast<PointFn>(base + 0x3c66894);
    worldToScreen = reinterpret_cast<ProjectFn>(base + 0x3c1b130);
    screenWidth = reinterpret_cast<ScreenSizeFn>(base + 0x3c24f6c);
    screenHeight = reinterpret_cast<ScreenSizeFn>(base + 0x3c24f94);
    if (unlockEnabled.load()) {
        void* handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        managedString = handle ? reinterpret_cast<NewStringFn>(dlsym(handle, "il2cpp_string_new")) : nullptr;
        if (!managedString) unlockEnabled = false;
    }
    speedDspTime=reinterpret_cast<DspTimeFn>(base+0x3c0f8b0);
    speedPitch=reinterpret_cast<PitchFn>(base+0x3c11498);
    InstalledHook installedHooks[16]{};
    if (!attach(base + 0x1db57c0,(uintptr_t)speedAudioUpdate,&originalAudioUpdate,installedHooks[11]) ||
        !attach(base + 0x1db5b90,(uintptr_t)speedAudioSeek,&originalAudioSeek,installedHooks[12]) ||
        !attach(base + 0x1db53e0,(uintptr_t)speedAudioPause,&originalAudioPause,installedHooks[13]) ||
        !attach(base + 0x1db5d00,(uintptr_t)speedAudioSchedule,&originalAudioSchedule,installedHooks[14]) ||
        !attach(base + 0x1db5aa4,(uintptr_t)speedAudioDisable,&originalAudioDisable,installedHooks[15]) ||
        !attach(base + 0x1d7ac10, (uintptr_t)practiceResultStart, &originalResultStart, installedHooks[10]) ||
        !attach(base + 0x1d87680, (uintptr_t)practiceGood, &originalGood, installedHooks[5]) ||
        !attach(base + 0x1d8758c, (uintptr_t)practiceBad, &originalBad, installedHooks[6]) ||
        !attach(base + 0x1d874ac, (uintptr_t)practiceMiss, &originalMiss, installedHooks[7]) ||
        !attach(base + 0x1d74334, (uintptr_t)practiceCreate, &originalCreate, installedHooks[8]) ||
        !attach(base + 0x1cb97f0, (uintptr_t)practiceSaveRecord, &originalSaveRecord, installedHooks[9]) ||
        !attach(base + 0x1d85b5c, (uintptr_t)update, &originalUpdate, installedHooks[0]) ||
        !attach(base + 0x1d82114, (uintptr_t)click, &originalClick, installedHooks[1]) ||
        !attach(base + 0x1d83b50, (uintptr_t)hold, &originalHold, installedHooks[2]) ||
        (unlockEnabled.load() &&
         (!attach(base + 0x1cbae10, (uintptr_t)checkUnlocked, &originalUnlock, installedHooks[3]) ||
          !attach(base + 0x1cb7c6c, (uintptr_t)unlockAwake, &originalAwake, installedHooks[4])))) {
        for (int i = 15; i >= 0; --i)
            if (installedHooks[i].target) writeEntry(installedHooks[i].target, installedHooks[i].saved);
        status = -3;
        __android_log_print(ANDROID_LOG_ERROR, "PhigrosTiming", "Hook installation failed; reverted");
        return;
    }
    status = 1;
    __android_log_print(ANDROID_LOG_INFO, "PhigrosTiming", "4.0.1 ARM64 timing and speed practice ready");
}
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_pigeongames_timing_TimingBridge_snapshot(JNIEnv* env, jclass) {
    // status, visible, count, total, misses, latest, 7 counters, 24 samples.
    // Existing 37 HUD values; visible spot count; then ms,x,y,age-ms per spot.
    std::array<float, 38 + SpotCapacity * 4> result{};
    result[0] = status.load();
    {
        std::lock_guard<std::mutex> guard(lock);
        result[1] = Clock::now() - lastFrame < std::chrono::milliseconds(600) ? 1.f : 0.f;
        result[2] = count; result[3] = total; result[4] = misses; result[5] = last;
        for (int i = 0; i < 7; ++i) result[6 + i] = bins[i];
        for (int i = 0; i < count; ++i) result[13 + i] = history[i];
        int visibleSpots = 0;
        const auto now = Clock::now();
        for (int i = 0; i < spotCount; ++i) {
            float age = std::chrono::duration<float, std::milli>(now - spots[i].time).count();
            if (age < 0 || age >= SpotLifetimeMs) continue;
            int at = 38 + visibleSpots++ * 4;
            result[at] = spots[i].ms; result[at+1] = spots[i].x;
            result[at+2] = spots[i].y; result[at+3] = age;
        }
        result[37] = visibleSpots;
    }
    auto array = env->NewFloatArray(result.size());
    if (array) env->SetFloatArrayRegion(array, 0, result.size(), result.data());
    return array;
}

extern "C" JNIEXPORT void JNICALL
Java_com_pigeongames_timing_TimingBridge_configureUnlock(JNIEnv* env, jclass, jstring path) {
    if (!path) return;
    const char* value = env->GetStringUTFChars(path, nullptr);
    if (!value) return;
    unlockAuditPath = value;
    env->ReleaseStringUTFChars(path, value);
    unlockEnabled = !unlockAuditPath.empty();
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    std::thread(install).detach();
    return JNI_VERSION_1_6;
}

#ifdef PHIGROS_TIMING_SELF_TEST
#include <cstdio>
#include <limits>
int testReplacement() { return 99; }
int contractCalls, contractReadyAfter;
bool contractHold;
void* contractMethod;
bool contractArgumentsValid;
bool projectionArgumentsValid = true;
void* testTransform(void* line, void* method) {
    projectionArgumentsValid &= line != nullptr && method == nullptr;
    return line;
}
Vec3 testWorldPoint(void*, Vec3 local, void* method) {
    projectionArgumentsValid &= local.y == 0 && local.z == 0 && method == nullptr;
    // A translated, rotated line: local X becomes world Y.
    return {10, 20+local.x, 5};
}
Vec3 testScreenPoint(void* camera, Vec3 world, int eye, void* method) {
    projectionArgumentsValid &= camera != nullptr && eye == 2 && method == nullptr;
    return {world.x*100,world.y*10,1};
}
int testScreenWidth(void*) { return 2000; }
int testScreenHeight(void*) { return 1000; }
void* unlockTestSong;
void* unlockTestCharts;
int unlockTestCalls;
void unlockQueryStub(void*, void*, int* mode, void** charts, bool* appeared, void**, void*) {
    ++unlockTestCalls; *mode = 0; *charts = unlockTestCharts; *appeared = false;
}
void* unlockSongStub(void*, void*, void*) { return unlockTestSong; }
template<class T> void testStore(void* object, size_t offset, T value) {
    std::memcpy(static_cast<char*>(object) + offset, &value, sizeof(value));
}
bool contractJudge(void* self, void* method) {
    contractArgumentsValid &= method == contractMethod;
    if (++contractCalls < contractReadyAfter) return false;
    auto* score = field<void*>(self, 0x20);
    testStore(score, 0x58, field<int>(score, 0x58) + 1);
    if (contractHold) testStore(self, 0x8e, true);
    return true;
}

int practiceCallbackCalls=0,practiceSaveCalls=0;
void testGoodCallback(void*,float,float,Vec3,bool,void*) {++practiceCallbackCalls;}
void testBadCallback(void*,float,float,void*) {++practiceCallbackCalls;}
void testMissCallback(void*,float,void*) {++practiceCallbackCalls;}
void testSaveCallback(void*,void*,void*,void*,void*) {++practiceSaveCalls;}
extern "C" int runSpeedClockTests();
int main() {
    if(int result=runSpeedClockTests()) return 40+result;
    std::puts("PASS speed DSP clock: bounds, continuity, pause, scheduled start, seek/restart and ten-minute drift");
    if(!validPracticeStart(12.5f,100) || !validPracticeStart(0,100) || !validPracticeStart(120,0)) return 29;
    if(validPracticeStart(100,100) || validPracticeStart(-1,100) || validPracticeStart(NAN,100) || validPracticeStart(INFINITY,100)) return 30;
    std::puts("PASS practice start validation: fractional, zero, unknown length; invalid and past-end starts rejected");
    originalGood=testGoodCallback;originalBad=testBadCallback;originalMiss=testMissCallback;originalSaveRecord=testSaveCallback;
    alignas(16) char practiceScore[256]{},practiceProgress[256]{};
    testStore(practiceScore,0x90,static_cast<void*>(practiceProgress));testStore(practiceProgress,0x90,10.f);
    alignas(16) char indexInfo[256]{},indexList[64]{},indexArray[64]{},indexNoteA[96]{},indexNoteB[96]{};
    testStore(practiceProgress,0x20,static_cast<void*>(indexInfo));testStore(indexInfo,0x38,static_cast<void*>(indexList));
    testStore(indexList,0x10,static_cast<void*>(indexArray));testStore(indexList,0x18,2);
    testStore(indexArray,0x20,static_cast<void*>(indexNoteA));testStore(indexArray,0x28,static_cast<void*>(indexNoteB));
    testStore(indexNoteA,0x38,1.1f);testStore(indexNoteA,0x2c,2.5f);testStore(indexNoteB,0x38,1.2f);testStore(indexNoteB,0x2c,4.75f);
    int revisionBefore=recordRevision.load();
    recording=true;practicing=false;
    practiceGood(practiceScore,1.1f,-.05f,{1,2,3},false,nullptr);
    practiceBad(practiceScore,1.2f,.12f,nullptr);
    practiceMiss(practiceScore,1.3f,nullptr);
    practiceMiss(practiceScore,1.3f,nullptr);
    if(practiceCallbackCalls!=4 || mistakes.size()!=3 || mistakes[0].grade!=1 || mistakes[2].grade!=3) return 26;
    if(mistakes[0].time!=2.5f || mistakes[1].time!=4.75f || noteTimeIndex.size()!=2 || recordRevision!=revisionBefore+3) return 32;
    std::puts("PASS note-time index resolves exact fractional codes and increments record revision only for distinct new records");
    practiceSaveRecord(nullptr,nullptr,nullptr,nullptr,nullptr);
    practicing=true;practiceSaveRecord(nullptr,nullptr,nullptr,nullptr,nullptr);
    practiceMiss(practiceScore,2.f,nullptr);
    if(practiceSaveCalls!=1 || mistakes.size()!=3) return 27;
    practicing=false;recording=false;mistakes.clear();mistakeCodes.clear();
    if(rebuildAt(nullptr,5.f) || practicing) return 28;
    std::puts("PASS original Good/Bad/Miss called once, distinct fractional note codes retained, duplicate excluded; recording and practice excluded correctly");
    std::puts("PASS normal record write forwarded, practice record write blocked, invalid rewind rejected without entering practice");
    size_t page = sysconf(_SC_PAGESIZE);
    auto* code = (unsigned char*)mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 1;
    // This real ARM64 function returns the word stored at its own address+256.
    // Its relocated prologue includes ADRP and stack saves.
    uintptr_t address = (uintptr_t)code;
    uint32_t dataOffset = (address + 256) & 4095;
    const uint32_t words[]{0xa9bf7bfd, 0x910003fd, 0x90000000,
        0x91000000 | (dataOffset << 10), 0xb9400000, 0xa8c17bfd, 0xd65f03c0};
    std::memcpy(code, words, sizeof(words));
    uint32_t value = 42; std::memcpy(code + 256, &value, 4);
    __builtin___clear_cache((char*)code, (char*)code + sizeof(words));
    if (mprotect(code, page, PROT_READ | PROT_EXEC)) return 2;
    auto fn = reinterpret_cast<int(*)()>(code);
    if (fn() != 42) return 3;
    int (*trampoline)() = nullptr;
    InstalledHook installed;
    if (!attach(address, (uintptr_t)testReplacement, &trampoline, installed)) return 4;
    if (fn() != 99) return 5;
    if (reinterpret_cast<int(*)()>(trampoline)() != 42) return 6;
    if (!writeEntry(address, installed.saved) || fn() != 42) return 7;
    std::puts("PASS actual ARM64 detour, ADRP relocation, original call and restoration");
    munmap(installed.trampoline, page);
    reset(nullptr);
    for (int i = 0; i < 40; ++i) record((i - 20) / 1000.f);
    record(std::numeric_limits<float>::quiet_NaN());
    record(std::numeric_limits<float>::infinity());
    if (count != 24 || total != 40 || std::fabs(history[0] + 4) > .001 || std::fabs(last - 19) > .001) return 8;
    reset(nullptr);
    if (count || total || misses) return 9;
    std::puts("PASS bounded signed history, invalid input and session reset");
    int noteA = 0, noteB = 0;
    for (int i = 0; i < 200; ++i) record(.015f + i * .001f, &noteA);
    if (total != 1 || count != 1 || std::fabs(last - 15) > .001) return 14;
    record(-.02f, &noteB);
    if (total != 2 || count != 2 || std::fabs(last + 20) > .001) return 15;
    reset(nullptr);
    record(.025f, &noteA);
    if (total != 1 || std::fabs(last - 25) > .001) return 16;
    std::puts("PASS repeated judgement of one note is recorded once; distinct notes and restart work");
    // Exercise the production wrappers, including their bool ABI and a caller
    // that keeps a note active until Judge returns true (not just HUD counters).
    componentTransform = testTransform; transformPoint = testWorldPoint;
    worldToScreen = testScreenPoint; screenWidth = testScreenWidth; screenHeight = testScreenHeight;
    for (bool isHold : {false, true}) for (int readyAfter : {1, 3}) {
        alignas(16) char controller[256]{}, score[256]{}, progress[256]{}, note[256]{}, judge[256]{}, line[256]{};
        testStore(controller, 0x20, static_cast<void*>(score));
        testStore(controller, 0x28, static_cast<void*>(progress));
        testStore(controller, isHold ? 0x38 : 0x40, static_cast<void*>(note));
        testStore(controller, isHold ? 0x40 : 0x38, static_cast<void*>(line));
        testStore(progress, 0x30, static_cast<void*>(judge));
        testStore(judge, 0xa0, static_cast<void*>(line));
        testStore(progress, 0x90, readyAfter == 1 ? 1.025f : .975f);
        testStore(note, 0x2c, 1.f); testStore(note, 0x18, -3.f);
        reset(nullptr);
        contractCalls = 0; contractReadyAfter = readyAfter; contractHold = isHold;
        contractMethod = note; contractArgumentsValid = true;
        originalClick = originalHold = contractJudge;
        bool removed = false;
        for (int frame = 0; frame < 200 && !removed; ++frame)
            removed = isHold ? hold(controller, contractMethod) : click(controller, contractMethod);
        if (!removed || contractCalls != readyAfter || !contractArgumentsValid ||
            field<int>(score, 0x58) != 1 || total != 1) return 17;
        float expectedMs = readyAfter == 1 ? 25.f : -25.f;
        if (spotCount != 1 || !projectionArgumentsValid ||
            std::fabs(spots[0].ms-expectedMs) > .001f ||
            std::fabs(spots[0].x-.5f) > .0001f || std::fabs(spots[0].y-.83f) > .0001f) return 22;
    }
    std::puts("PASS real click/hold wrappers preserve false and true; caller removes note once; score increments once over 200 frames");
    std::puts("PASS per-note early/late milliseconds and rotated-line anchor projection through real wrappers");
    reset(nullptr);
    for (int i=0;i<50;i++) record(.01f, nullptr, {.2f,.3f,true});
    if (spotCount != SpotCapacity || total != 50) return 23;
    record(.02f, nullptr, {-1,.3f,true});
    record(.02f, nullptr, {.2f,std::numeric_limits<float>::quiet_NaN(),true});
    if (spotCount != SpotCapacity) return 24;
    reset(nullptr);
    if (spotCount || total) return 25;
    std::puts("PASS bounded note spots, invalid projection rejection and restart clearing");
    alignas(16) char song[160]{}, levels[64]{}, ratings[64]{}, levelName[32]{};
    storeField(song, 0x50, static_cast<void*>(levels));
    storeField(song, 0x30, static_cast<void*>(ratings));
    storeField(levels, 0x18, size_t(4)); storeField(ratings, 0x18, size_t(4));
    storeField(levelName, 0x10, 2);
    for (int i = 0; i < 4; ++i) storeField(levels, 0x20+i*8, static_cast<void*>(levelName));
    storeField(ratings, 0x20, 5.f); storeField(ratings, 0x24, 10.f); storeField(ratings, 0x28, 14.f);
    if (!hasChart(song, 0) || !hasChart(song, 2) || hasChart(song, 3) || hasChart(song, 4)) return 18;
    storeField(ratings, 0x2c, 16.f);
    if (!hasChart(song, 3)) return 19;
    storeField(levels, 0x38, static_cast<void*>(nullptr));
    if (hasChart(song, 3)) return 20;
    std::puts("PASS chart availability includes real AT, excludes absent/zero-rated charts and out-of-bounds indices");
    alignas(16) char chartResults[64]{};
    storeField(chartResults, 0x18, size_t(4));
    unlockTestSong = song; unlockTestCharts = chartResults; unlockTestCalls = 0;
    unlockBase = reinterpret_cast<uintptr_t>(unlockSongStub) - 0x1ca669c;
    originalUnlock = unlockQueryStub; unlockAppliedOwner = song; unlockEnabled = true;
    int songMode = 0; void* chartPointer = nullptr; bool appeared = false; void* info = nullptr;
    char unchangedSong[sizeof(song)]; std::memcpy(unchangedSong, song, sizeof(song));
    checkUnlocked(song, song, &songMode, &chartPointer, &appeared, &info, nullptr);
    // Actual SongSelectorItem::.ctor uses (unlockType == -1), not (unlockType == 0).
    if (unlockTestCalls != 1 || songMode != -1 || !appeared || chartPointer != chartResults ||
        !field<bool>(chartResults, 0x20) || !field<bool>(chartResults, 0x22) || field<bool>(chartResults, 0x23) ||
        std::memcmp(unchangedSong, song, sizeof(song))) return 21;
    unlockEnabled = false; unlockBase = 0;
    std::puts("PASS production unlock wrapper sets selector's -1 unlocked state, preserves catalog and excludes missing AT");
    uint32_t unsupported = 0x14000001;
    if (mprotect(code, page, PROT_READ | PROT_WRITE)) return 10;
    std::memcpy(code, &unsupported, 4);
    if (mprotect(code, page, PROT_READ | PROT_EXEC)) return 11;
    InstalledHook rejected;
    if (attach(address, (uintptr_t)testReplacement, &trampoline, rejected)) return 12;
    uint32_t unchanged; std::memcpy(&unchanged, code, 4);
    if (unchanged != unsupported) return 13;
    std::puts("PASS unsupported relative branch rejected without patching");
    munmap(code, page);
    return 0;
}
#endif

extern "C" JNIEXPORT void JNICALL Java_com_pigeongames_timing_TimingBridge_practiceAction(JNIEnv*,jclass,jint command,jfloat value) {
    if(command==0) {recording=value!=0;++recordRevision;}
    if(command==1 && std::isfinite(value)) { requestedTime=std::max(0.f,value);practiceCommand=1; }
    if(command==3 && practicing) { requestedTime=practiceStart.load();practiceCommand=1; }
}
extern "C" JNIEXPORT jfloatArray JNICALL Java_com_pigeongames_timing_TimingBridge_practiceData(JNIEnv* env,jclass) {
    std::lock_guard<std::mutex> guard(practiceLock);
    std::vector<float> data{currentSongTime.load(),practicing?1.f:0.f,practiceStart.load(),recording?1.f:0.f};
    for(auto& m:mistakes) {data.push_back(m.time);data.push_back(m.grade);data.push_back(m.delta*1000);data.push_back(m.code);}
    auto result=env->NewFloatArray(data.size());if(result)env->SetFloatArrayRegion(result,0,data.size(),data.data());return result;
}

extern "C" JNIEXPORT jstring JNICALL Java_com_pigeongames_timing_TimingBridge_restartTarget(JNIEnv* env,jclass) {
    std::lock_guard<std::mutex> guard(practiceLock);
    return resultScene && !resultObjectName.empty()?env->NewString(reinterpret_cast<const jchar*>(resultObjectName.data()),resultObjectName.size()):env->NewStringUTF("");
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_pigeongames_timing_TimingBridge_practiceFrom(JNIEnv*,jclass,jfloat start) {
    if(!validPracticeStart(start,songLength.load())) return false;
    requestedTime=start;practiceCommand=1;
    return true;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_pigeongames_timing_TimingBridge_setPracticeSpeed(JNIEnv*,jclass,jfloat rate) {
    if(!PracticeSpeedClock::validRate(rate)) return false;
    desiredSpeed=rate;
    return true;
}

extern "C" JNIEXPORT jint JNICALL Java_com_pigeongames_timing_TimingBridge_recordRevision(JNIEnv*,jclass) {
    return recordRevision.load();
}
extern "C" JNIEXPORT void JNICALL Java_com_pigeongames_timing_TimingBridge_setDisplayOptions(JNIEnv*,jclass,jboolean bar,jboolean notes) {
    captureBar=bar;captureSpots=notes;
    std::lock_guard<std::mutex> guard(lock);
    if(!notes) spotCount=0;
    if(!bar && !notes) {history.fill(0);bins.fill(0);count=0;total=0;last=0;}
}
