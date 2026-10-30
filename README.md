# Phigros 准度练习测试版（test.17）

面向 Android ARM64 的个人练习修改工具。此仓库包含新增功能的 C++ / Java 实现与打包脚本，**不是 Phigros 官方源码**。Phigros 及原游戏内容属于其相关权利人。

## 功能与使用

- 显示最近击打的准度条，以及对应音符旁的“早／晚 + 毫秒”；两项可以在「练习」菜单独立开关并保存选择。
- 勾选记录后，普通完整游玩的 Good / Bad / Miss 可列出并跳转练习；Perfect 不进入失误列表。
- 左下角「练习」「后退」，右下角「快进」「重练」。可设置练习起点和前后跳转秒数，只设起点、不限制结束时间，没有额外预备倒计时。
- 「练习 → 变速练习」支持 **0.50～1.50 倍**，音乐音调随速度一起变化。音乐、音符与判定线按相同倍率推进，谱面文件及事件数据保持原样。
- 起点、跳转秒数、准度毫秒均按**原谱时间**计量；例如 0.5 倍下，4 个谱面秒相当于 8 秒实际播放时长。慢速时真实时间上的判定窗口相应放宽。
- 用过跳转或变速的本次游玩属于练习，**不保存成绩或 RKS**，恢复到 1 倍也不会把本次练习写成正式成绩。恢复 1 倍后重新正常开始歌曲可正常保存。重启应用默认恢复 1 倍。
- 沿用测试版的曲目可用逻辑，包含实际存在的 AT 谱面；不会为不存在的难度生成谱面。
- 已做音符时间索引、记录变更检查、后台原子写入、失误列表条目复用和关闭提示时跳过投影等处理。尚无量化 FPS 提升结论。

## 下载与安装

安装包请查看本仓库的 Releases。独立包名为 `com.PigeonGames.Phigros.timing`，界面名称为「Phigros 准度测试版」。从同一签名的旧测试版更新可覆盖安装；正常使用不需要卸载原版 Phigros。

APK 较大，发布时可能分卷提供。请按 Release 中的合并说明恢复完整 APK 并核对 SHA-256，再安装。

## 自行构建

当前脚本针对 **Phigros 4.0.1 的已验证 ARM64 核心**，使用文件哈希和运行时入口检查。版本不匹配时会拒绝构建；运行时检查不匹配则不启用修改。不支持把地址直接套用到其他版本。

需要自行准备受支持的原版 APK、Windows PowerShell、Python 3、JDK 17、Android NDK r27c、CMake / Ninja、Android SDK 平台 `android.jar` 与 Build Tools，以及自己的 APK 签名证书。仓库不包含原版 APK、官方游戏资源、工具链、发布签名密钥或用户存档。

脚本从仓库根目录运行，依赖目录约定如下（可按自己的环境修改脚本路径）：

```text
.tools/
  jdk/<JDK目录>/bin/java.exe、javac.exe
  android-ndk-r27c/
  cmake/bin/cmake.exe、ninja.exe
  sdk/android.jar
  sdk/android-15/zipalign.exe
  sdk/android-15/lib/apksigner.jar、d8.jar
  signing/practice.p12
  signing/password.txt
phigros-timing/
```

`practice.p12` 与密码文件由构建者自行创建，证书别名在脚本中为 `practice`。请使用自己的签名材料，勿上传密钥或密码。自行构建的签名不同于发布版时，不能直接覆盖发布版安装。

```powershell
./phigros-timing/build.ps1 -SourceApk 'D:/input/Phigros-4.0.1.apk'
```

构建产物在 `dist/`。打包脚本保留原游戏核心、原 DEX 和游戏资源内容，额外加入测试版原生库与界面 DEX，并修改独立包名及版本信息。`speed_clock_test.cpp` 和 `speed_engine_host_test.cpp` 提供时钟及音频回调的离线测试代码；`timing_selftest` 可在 ARM64 Android 上执行原生自检。

## 验证范围

test.17 已在 Android ARM64 手机上启动并完成基本变速、回退、快进及重练验证，使用者确认音乐与音符同步。本项目没有进行全谱面、全部设备的覆盖测试，也没有提供帧率提升幅度保证。
