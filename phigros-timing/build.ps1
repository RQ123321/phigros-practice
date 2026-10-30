param([Parameter(Mandatory=$true)][string]$SourceApk)
$ErrorActionPreference='Stop'
$timingRoot=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $timingRoot
python phigros-timing/prepare.py $SourceApk
if($LASTEXITCODE -ne 0){throw 'Unsupported input APK'}
$timingJdk=(Get-ChildItem .tools/jdk -Directory | Select-Object -First 1).FullName
if(!(Test-Path .tools/sdk/android.jar)){throw 'Android SDK android.jar is required'}
if(Test-Path P:\){throw 'Drive P: must be unused for the ASCII build alias'}
subst.exe P: $timingRoot
try {
    & P:/.tools/cmake/bin/cmake.exe -S P:/phigros-timing -B P:/.tools/phigros-timing-build -G Ninja '-DCMAKE_MAKE_PROGRAM=P:/.tools/cmake/bin/ninja.exe' '-DCMAKE_TOOLCHAIN_FILE=P:/.tools/android-ndk-r27c/build/cmake/android.toolchain.cmake' '-DANDROID_ABI=arm64-v8a' '-DANDROID_PLATFORM=android-26' '-DANDROID_STL=c++_static' '-DCMAKE_BUILD_TYPE=Release'
    if($LASTEXITCODE -ne 0){throw 'CMake failed'}
    & P:/.tools/cmake/bin/cmake.exe --build P:/.tools/phigros-timing-build -j 4
    if($LASTEXITCODE -ne 0){throw 'Native build failed'}
} finally { subst.exe P: /D }
New-Item -ItemType Directory -Force .tools/phigros-timing-java,.tools/phigros-timing-dex,dist | Out-Null
& "$timingJdk/bin/javac.exe" -encoding UTF-8 -source 8 -target 8 -classpath .tools/sdk/android.jar -d .tools/phigros-timing-java phigros-timing/java/com/pigeongames/timing/TimingBridge.java
if($LASTEXITCODE -ne 0){throw 'Java build failed'}
@'
from pathlib import Path
from zipfile import ZipFile,ZIP_DEFLATED
with ZipFile('.tools/phigros-timing-java.jar','w',ZIP_DEFLATED) as z:
 root=Path('.tools/phigros-timing-java')
 for p in root.rglob('*.class'):z.write(p,p.relative_to(root).as_posix())
'@ | python -
& "$timingJdk/bin/java.exe" -cp .tools/sdk/android-15/lib/d8.jar com.android.tools.r8.D8 --lib .tools/sdk/android.jar --min-api 26 --output .tools/phigros-timing-dex .tools/phigros-timing-java.jar
if($LASTEXITCODE -ne 0){throw 'DEX build failed'}
python phigros-timing/build_apk.py
if($LASTEXITCODE -ne 0){throw 'APK assembly failed'}
& .tools/sdk/android-15/zipalign.exe -P 16 -f 4 .tools/phigros-inspect/timing-unsigned.apk .tools/phigros-inspect/timing-aligned.apk
if($LASTEXITCODE -ne 0){throw 'APK alignment failed'}
$timingOutput='dist/Phigros-Timing-arm64-v4.0.1-unlocked-test.17.apk'
& "$timingJdk/bin/java.exe" -jar .tools/sdk/android-15/lib/apksigner.jar sign --ks .tools/signing/practice.p12 --ks-key-alias practice --ks-pass file:.tools/signing/password.txt --out $timingOutput .tools/phigros-inspect/timing-aligned.apk
if($LASTEXITCODE -ne 0){throw 'Signing failed'}
& "$timingJdk/bin/java.exe" -jar .tools/sdk/android-15/lib/apksigner.jar verify --verbose $timingOutput
if($LASTEXITCODE -ne 0){throw 'Signature verification failed'}
& .tools/sdk/android-15/zipalign.exe -P 16 -c 4 $timingOutput
if($LASTEXITCODE -ne 0){throw 'Final alignment verification failed'}
Get-FileHash $timingOutput -Algorithm SHA256
