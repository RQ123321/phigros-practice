// Included inside timing.cpp's anonymous namespace; only the verified 4.0.1 core.
std::string unlockAuditPath;
std::atomic<bool> unlockEnabled{false};
uintptr_t unlockBase = 0;
UpdateFn originalAwake;
using UnlockFn = void(*)(void*, void*, int*, void**, bool*, void**, void*);
UnlockFn originalUnlock;
using NewStringFn = void*(*)(const char*);
NewStringFn managedString;
// SongSelectorItem::.ctor sets Unlocked iff unlockType == -1 (0 is locked).
constexpr int UnlockedSongMode = -1;
void* unlockAppliedOwner = nullptr;
bool unlockMigrationRunning = false;
void applyUnlockProfile(void* self);

template<class T> void storeField(void* object, size_t offset, T value) {
    if (object) std::memcpy(static_cast<char*>(object) + offset, &value, sizeof(value));
}
size_t arrayLength(void* array) { return field<size_t>(array, 0x18); }
void* arrayObject(void* array, size_t index) {
    return array && index < arrayLength(array) ? field<void*>(array, 0x20 + index * 8) : nullptr;
}
std::string textOf(void* string) {
    int length = field<int>(string, 0x10);
    if (!string || length < 0 || length > 1024) return {};
    std::string result;
    for (int i = 0; i < length; ++i) {
        uint32_t c = field<uint16_t>(string, 0x14 + i * 2);
        if (c < 0x80) result += char(c);
        else if (c < 0x800) { result += char(0xc0 | c >> 6); result += char(0x80 | (c & 63)); }
        else { result += char(0xe0 | c >> 12); result += char(0x80 | ((c >> 6) & 63)); result += char(0x80 | (c & 63)); }
    }
    return result;
}
bool hasChart(void* song, size_t index) {
    auto* levels = field<void*>(song, 0x50);
    auto* ratings = field<void*>(song, 0x30);
    auto* level = arrayObject(levels, index);
    if (!level || field<int>(level, 0x10) <= 0 || index >= arrayLength(ratings)) return false;
    float rating = field<float>(ratings, 0x20 + index * 4);
    return std::isfinite(rating) && std::fabs(rating) > .001f;
}
void checkUnlocked(void* self, void* songId, int* mode, void** charts,
                   bool* appeared, void** info, void* method) {
    if (unlockEnabled.load() && !unlockMigrationRunning && unlockAppliedOwner != self) applyUnlockProfile(self);
    originalUnlock(self, songId, mode, charts, appeared, info, method);
    if (!unlockEnabled.load() || !charts || !*charts || arrayLength(*charts) > 8) return;
    auto getSong = reinterpret_cast<void*(*)(void*,void*,void*)>(unlockBase + 0x1ca669c);
    void* song = getSong(self, songId, nullptr);
    if (!song) return;
    for (size_t i = 0; i < arrayLength(*charts); ++i) storeField(*charts, 0x20 + i, hasChart(song, i));
    if (mode) *mode = UnlockedSongMode;
    if (appeared) *appeared = true;
}
void applyUnlockProfile(void* self) {
    if (!self || !unlockEnabled.load() || !managedString || unlockMigrationRunning || unlockAppliedOwner == self) return;
    void* songBase = field<void*>(self, 0xe8);
    if (!songBase) return; // Awake may still be waiting for catalog loading.
    auto getSongs = reinterpret_cast<void*(*)(void*,void*)>(unlockBase + 0x1cb6850);
    void* songs = getSongs(songBase, nullptr);
    void* items = field<void*>(songs, 0x10);
    int songCount = field<int>(songs, 0x18);
    if (!songs || songCount < 1 || songCount > 2000 || size_t(songCount) > arrayLength(items)) return;
    struct MigrationScope {
        MigrationScope() { unlockMigrationRunning = true; }
        ~MigrationScope() { unlockMigrationRunning = false; }
    } scope;
    auto make = [](const std::string& s) { return managedString(s.c_str()); };
    auto saveBool = reinterpret_cast<void(*)(void*,bool,void*)>(unlockBase + 0x1cc311c);
    auto saveInt = reinterpret_cast<void(*)(void*,int,void*)>(unlockBase + 0x1cc31d0);
    auto loadBool = reinterpret_cast<bool(*)(void*,bool,void*)>(unlockBase + 0x1cc3458);
    auto loadString = reinterpret_cast<void*(*)(void*,void*,void*)>(unlockBase + 0x1cc42c8);
    auto saveString = reinterpret_cast<void(*)(void*,void*,void*)>(unlockBase + 0x1cc33dc);
    auto deleteRecord = reinterpret_cast<void(*)(void*,void*,void*)>(unlockBase + 0x1cb9d34);
    auto getRecord = reinterpret_cast<void*(*)(void*,void*,void*,void*)>(unlockBase + 0x1cb959c);
    auto savePrefs = reinterpret_cast<void(*)(void*)>(unlockBase + 0x3c53f20);
    auto* marker = make("TimingCleanUnlockProfileV1");
    bool migrate = !loadBool(marker, false, nullptr);
    // RemoveAllSongControl hides the original All Songs entry while this
    // string is empty. AboutUs.StartPlay writes the canonical value "4.0".
    auto* completedKey = make("GameCompleted");
    if (textOf(loadString(completedKey, make(""), nullptr)).empty()) {
        saveString(completedKey, make("4.0"), nullptr);
        savePrefs(nullptr);
    }
    const bool allSongsEntryEnabled = !textOf(loadString(completedKey, make(""), nullptr)).empty();

    // Chapter selector gates are separate from chart gates. Only unlock state,
    // never a fabricated score/rank/accuracy, is changed here.
    void* chapters = field<void*>(self, 0xe0);
    if (arrayLength(chapters) > 100) return;
    for (size_t i = 0; i < arrayLength(chapters); ++i) {
        void* chapter = arrayObject(chapters, i);
        storeField(field<void*>(chapter, 0x20), 0x10, 0);
    }
    if (migrate) {
        const char* flags[]{
            "finishLegacyChapter", "chapter8UnlockBegin", "chapter8UnlockSecondPhase", "chapter8Passed",
            "Chapter9UnlockBegin", "Chapter9UnlockSecondPhaseBegin", "Chapter9Phase2Passed",
            "C9BaselineChallengeReached", "C8CraveWaveUnlocked", "C8TheChariotREVIIVALUnlocked",
            "C8LuminescenceUnlocked", "C8RetributionUnlocked", "C8DESTRUCTION321Unlocked", "C8DistortedFateUnlocked",
            "C9AboutTheUniverseUnlocked", "C9AmetrineUnlocked", "C9DesultorySignalsUnlocked",
            "C9EntranceToTheChaosUnlocked", "C9EvanescentUnlocked", "C9ExoplanetaryMirageUnlocked",
            "C9HappyEndingUnlocked", "C9HateUnlocked", "C9ImplexroughUnlocked", "C9MessageUnlocked",
            "C9PetrichorUnlocked", "C9S6Unlocked", "inferiorINSGrade", "YouaretheMiserableINSGrade",
            "ShadowINSGrade", "心之所向INSGrade", "StasisINSGrade", "DESTRUCTION321INSGrade",
            "DistortedFateINSGrade", "iLArtifactINSGrade", "CuvismINSGrade",
            "atruthseekerCommunicationwithUtopiawillbelostINSGrade"
        };
        for (const char* key : flags) saveBool(make(key), true, nullptr);
        for (const char* prefix : {"unlockFlagOfSpasmodic", "unlockFlagOfIgallta", "unlockFlagOfRrharil"})
            for (const char* level : {"EZ", "HD", "IN", "AT"}) saveBool(make(std::string(prefix)+level), true, nullptr);
        for (int i = 0; i < 8; ++i) saveBool(make("randomVersionUnlocked"+std::to_string(i)), true, nullptr);
        // Verified Step enum: 0..4; BaselineChallenge is the final state.
        saveInt(make("Chapter9Phase2Step"), 4, nullptr);
        saveInt(make("ChallengeModeRank"), 0, nullptr);
    }
    int chartCount = 0, atCount = 0, nonzero = 0, locked = 0;
    for (int i = 0; i < songCount; ++i) {
        void* song = arrayObject(items, i);
        void* id = field<void*>(song, 0x10);
        void* levels = field<void*>(song, 0x50);
        if (!id || arrayLength(levels) > 8) continue;
        for (size_t j = 0; j < arrayLength(levels); ++j) {
            void* level = arrayObject(levels, j);
            if (migrate && level && field<int>(level, 0x10) > 0) deleteRecord(id, level, nullptr);
            if (!hasChart(song, j)) continue;
            ++chartCount;
            if (textOf(level) == "AT") ++atCount;
            void* record = getRecord(self, id, level, nullptr);
            if (!record || field<int>(record, 0x10) || field<float>(record, 0x14) || field<int>(record, 0x18)) ++nonzero;
        }
        int mode = 0; void* charts = nullptr; bool appeared = false; void* info = nullptr;
        checkUnlocked(self, id, &mode, &charts, &appeared, &info, nullptr);
        for (size_t j = 0; j < arrayLength(charts); ++j)
            if (hasChart(song, j) && (mode != UnlockedSongMode || !appeared || !field<bool>(charts, 0x20+j))) ++locked;
    }
    if (migrate && nonzero == 0 && locked == 0) {
        storeField(self, 0xd8, 0.f);
        saveBool(marker, true, nullptr);
        savePrefs(nullptr);
    }
    std::ofstream audit(unlockAuditPath, std::ios::trunc);
    audit << "{\"songs\":" << songCount << ",\"charts\":" << chartCount
          << ",\"atCharts\":" << atCount << ",\"nonzeroRecords\":" << nonzero
          << ",\"lockedCharts\":" << locked << ",\"initialMigration\":" << (migrate ? "true" : "false")
          << ",\"allSongsEntryEnabled\":" << (allSongsEntryEnabled ? "true" : "false")
          << ",\"resourcePlaybackVerified\":false}";
    __android_log_print(ANDROID_LOG_INFO, "PhigrosTiming", "Unlock audit: songs=%d charts=%d AT=%d nonzero=%d locked=%d", songCount, chartCount, atCount, nonzero, locked);
    if (locked == 0 && (!migrate || nonzero == 0)) unlockAppliedOwner = self;
}
void unlockAwake(void* self, void* method) {
    originalAwake(self, method);
    applyUnlockProfile(self);
}
