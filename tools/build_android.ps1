# Builds Red Two for Android (arm64-v8a) and packs a signed APK, without Gradle: the NDK's CMake
# toolchain compiles libmain.so (the program with SDL and Dawn linked in), the SDK's build tools
# do the rest (javac + d8 for SDL's, Aurora's and Red Two's Java classes, aapt2 for the manifest
# and icons, zipalign, apksigner).
#   powershell -ExecutionPolicy Bypass -File tools\build_android.ps1 [-Disc <rvz|iso>] [-HostRecomp <r2recomp.exe>]
#       [-VersionName 1.0.0] [-VersionCode 1] [-Out build\RedTwo-android-arm64.apk] [-Keystore <jks>] [-Configure]
# Needs: the Android SDK (ANDROID_HOME, else %LOCALAPPDATA%\Android\Sdk) with an NDK, build-tools,
# platforms;android-35 and its CMake; a JDK 17+ (JAVA_HOME, else Android Studio's); Python 3 (zip
# and notices). The recompiler runs on this machine: tools\build.ps1 builds one (r2recomp.exe).
# Signing: -Keystore, its password in <keystore without .jks>.password.txt; by default
# %USERPROFILE%\.android\redtwo-release.jks, created on first use. Keep that file: Android
# installs an update only when it is signed with the same key.
param(
    [string]$Disc = "",
    [string]$HostRecomp = "",
    [string]$VersionName = "1.0.0",
    [int]$VersionCode = 1,
    [string]$Out = "",
    [string]$Keystore = "",
    [string]$BuildDir = "",
    [string]$Config = "RelWithDebInfo",
    [string]$Env = "",   # "R2_WATCHDOG=5;R2_IRQ_LOG=1": environment for the app (SDL's SDL_ENV.* meta-data; debugging)
    [switch]$Configure,
    [switch]$Debuggable  # test builds: the launch intent's R2_* extras become environment variables
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = if ($BuildDir) { $BuildDir } else { Join-Path $root "build\android" }
if (-not $Out) { $Out = Join-Path $root "build\RedTwo-android-arm64.apk" }
$minSdk = 29      # Android 10
$targetSdk = 35

function Newest([string]$dir) {
    Get-ChildItem $dir -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+(\.\d+)*$' } |
        Sort-Object { [version]($_.Name + $(if ($_.Name -notmatch '\.') { '.0' } else { '' })) } |
        Select-Object -Last 1
}
function Run([string]$exe, [string[]]$a) {
    & $exe @a
    if ($LASTEXITCODE) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

# --- tools ------------------------------------------------------------------------------------
$sdk = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, "$env:LOCALAPPDATA\Android\Sdk") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $sdk) { throw "Android SDK not found (set ANDROID_HOME)" }
$ndk = if ($env:ANDROID_NDK_HOME -and (Test-Path $env:ANDROID_NDK_HOME)) { $env:ANDROID_NDK_HOME } else { (Newest "$sdk\ndk").FullName }
if (-not $ndk) { throw "no NDK in $sdk\ndk (sdkmanager `"ndk;27.3.13750724`")" }
$bt = (Newest "$sdk\build-tools").FullName
if (-not $bt) { throw "no build-tools in $sdk" }
$androidJar = "$sdk\platforms\android-$targetSdk\android.jar"
if (-not (Test-Path $androidJar)) { throw "missing $androidJar (sdkmanager `"platforms;android-$targetSdk`")" }
$sdkCmake = (Newest "$sdk\cmake").FullName
$cmake = if ($sdkCmake) { "$sdkCmake\bin\cmake.exe" } else { (Get-Command cmake).Source }
$ninja = if ($sdkCmake -and (Test-Path "$sdkCmake\bin\ninja.exe")) { "$sdkCmake\bin\ninja.exe" } else { (Get-Command ninja).Source }
$jdk = @($env:JAVA_HOME, "$env:ProgramFiles\Android\Android Studio\jbr") | Where-Object { $_ -and (Test-Path "$_\bin\javac.exe") } | Select-Object -First 1
if (-not $jdk) { throw "no JDK (set JAVA_HOME)" }
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { throw "python not found" }
if (-not $HostRecomp) {
    foreach ($c in @("$root\build\release-win\r2recomp.exe", "$root\build\windows\r2recomp.exe")) { if (Test-Path $c) { $HostRecomp = $c; break } }
}
if (-not $HostRecomp -or -not (Test-Path $HostRecomp)) { throw "no r2recomp.exe for this machine: run tools\build.ps1 first or pass -HostRecomp" }
$HostRecomp = (Resolve-Path $HostRecomp).Path
Write-Host "NDK $ndk; build-tools $bt; JDK $jdk"

# --- Aurora's patches (as tools\build.ps1) ----------------------------------------------------
$auroraClean = -not (git -C "$root\extern\aurora" status --porcelain --untracked-files=no)
foreach ($patch in Get-ChildItem "$root\patches\aurora\*.patch" -ErrorAction SilentlyContinue | Sort-Object Name) {
    if (-not $auroraClean) {
        cmd /c "git -C `"$root\extern\aurora`" apply --reverse --check `"$($patch.FullName)`" 2>nul"
        if ($LASTEXITCODE -eq 0) { continue }
    }
    cmd /c "git -C `"$root\extern\aurora`" apply `"$($patch.FullName)`" 2>nul"
    if ($LASTEXITCODE -eq 0) { Write-Host "applied $($patch.Name)"; continue }
    if ($auroraClean) { throw "could not apply $($patch.Name) to extern/aurora" }
    Write-Host "note: $($patch.Name) neither applies nor reverses on the modified extern/aurora (assumed applied)"
}

# --- libmain.so -------------------------------------------------------------------------------
if ($Disc) { $Disc = (Resolve-Path $Disc).Path; $Configure = $true }
if ($Configure -or -not (Test-Path "$build\build.ninja")) {
    $a = @("-S", $root, "-B", $build, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_BUILD_TYPE=$Config",
        "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake", "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-$minSdk", "-DANDROID_STL=c++_static", "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON",
        # NDK r27's libc++ has std::jthread / std::stop_token, which Aurora uses, only as
        # experimental, and no std::atomic_ref (android/compat/libcxx_compat.h)
        "-DCMAKE_CXX_FLAGS=-fexperimental-library -include $($root.Replace('\', '/'))/android/compat/libcxx_compat.h",
        "-DR2_HOST_RECOMP=$HostRecomp", "-DAURORA_SDL3_PROVIDER=vendor", "-DAURORA_SDL3_LINKAGE=static",
        "-DAURORA_DAWN_PROVIDER=auto", "-DAURORA_DAWN_LINKAGE=static")
    if ($Disc) { $a += "-DR2_DOL=$Disc" }
    Run $cmake $a
}
Run $cmake @("--build", $build, "--target", "red_two")
$so = "$build\libmain.so"
if (-not (Test-Path $so)) { throw "no $so" }

# --- APK ----------------------------------------------------------------------------------------
$stage = "$build\apk"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\classes", "$stage\dex", "$stage\lib\arm64-v8a", "$stage\assets" | Out-Null

# native code: debug info stripped (the dynamic symbols stay for crash call stacks; the
# unstripped $so is the one to symbolize with)
Run "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" @("--strip-debug", "-o", "$stage\lib\arm64-v8a\libmain.so", $so)

# Java: SDL's activity (the copy matching the SDL built in), Aurora's surface, Red Two's activity
$cache = Get-Content "$build\CMakeCache.txt"
$sdlJava = ($cache | Select-String '^AURORA_SDL3_JAVA_SOURCE_DIR:INTERNAL=(.*)$').Matches[0].Groups[1].Value
$auroraJava = ($cache | Select-String '^AURORA_ANDROID_JAVA_SOURCE_DIR:INTERNAL=(.*)$').Matches[0].Groups[1].Value
$sources = @(Get-ChildItem -Recurse -Filter *.java $sdlJava, $auroraJava, "$root\android\java" | ForEach-Object { $_.FullName })
$sources | Set-Content -Encoding ascii "$stage\sources.txt"
Run "$jdk\bin\javac.exe" @("-source", "11", "-target", "11", "-Xlint:-options", "-nowarn", "-encoding", "UTF-8",
    "-classpath", $androidJar, "-d", "$stage\classes", "@$stage\sources.txt")
Run "$jdk\bin\jar.exe" @("cf", "$stage\classes.jar", "-C", "$stage\classes", ".")
Run "$bt\d8.bat" @("--release", "--min-api", "$minSdk", "--lib", $androidJar, "--output", "$stage\dex", "$stage\classes.jar")

# assets: the licenses
Copy-Item "$root\LICENSE" "$stage\assets\LICENSE.txt"
Run $python @("$root\tools\third_party_notices.py", $build, "$stage\assets\THIRD_PARTY_NOTICES.txt")

# manifest (-Env: environment variables SDL sets before the program starts), resources, assets
$manifest = "$root\android\AndroidManifest.xml"
if ($Env -or $Debuggable) {
    $text = Get-Content -Raw $manifest
    if ($Env) {
        $meta = ($Env -split ';' | Where-Object { $_ -match '^([^=]+)=(.*)$' } | ForEach-Object {
            "        <meta-data android:name=`"SDL_ENV.$($Matches[1])`" android:value=`"$($Matches[2])`" />" }) -join "`n"
        $text = $text.Replace("    </application>", "$meta`n    </application>")
        Write-Host "app environment: $Env"
    }
    if ($Debuggable) {
        $text = $text -replace '<application(\r?\n)', "<application`$1        android:debuggable=`"true`"`$1"
        if ($text -notmatch 'android:debuggable') { throw "could not mark the manifest debuggable" }
        Write-Host "debuggable test build"
    }
    $text | Set-Content -Encoding utf8 "$stage\AndroidManifest.xml"
    $manifest = "$stage\AndroidManifest.xml"
}
Run "$bt\aapt2.exe" @("compile", "--dir", "$root\android\res", "-o", "$stage\res.zip")
Run "$bt\aapt2.exe" @("link", "-o", "$stage\base.apk", "-I", $androidJar, "--manifest", $manifest,
    "--min-sdk-version", "$minSdk", "--target-sdk-version", "$targetSdk", "--version-code", "$VersionCode",
    "--version-name", $VersionName, "-A", "$stage\assets", "$stage\res.zip")

# classes.dex and the library into it (Python's zipfile appends without touching aapt2's entries)
$zipScript = @'
import sys, zipfile
apk, dex, so = sys.argv[1:4]
with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    z.write(dex, "classes.dex")
    z.write(so, "lib/arm64-v8a/libmain.so")
'@
$zipScript | Set-Content -Encoding ascii "$stage\addfiles.py"
Run $python @("-I", "$stage\addfiles.py", "$stage\base.apk", "$stage\dex\classes.dex", "$stage\lib\arm64-v8a\libmain.so")
Run "$bt\zipalign.exe" @("-f", "-p", "4", "$stage\base.apk", "$stage\aligned.apk")

# signing
if (-not $Keystore) { $Keystore = "$env:USERPROFILE\.android\redtwo-release.jks" }
$pwFile = [IO.Path]::ChangeExtension($Keystore, $null).TrimEnd('.') + ".password.txt"
if (-not (Test-Path $Keystore)) {
    if (-not (Test-Path $pwFile)) {
        $bytes = New-Object byte[] 24
        [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
        [Convert]::ToBase64String($bytes).Replace('/', '_').Replace('+', '-') | Set-Content -NoNewline -Encoding ascii $pwFile
    }
    Write-Host "creating the signing key $Keystore (password in $pwFile; keep both)"
    Run "$jdk\bin\keytool.exe" @("-genkeypair", "-keystore", $Keystore, "-alias", "redtwo", "-keyalg", "RSA", "-keysize", "4096",
        "-validity", "36500", "-dname", "CN=Red Two", "-storepass:file", $pwFile, "-keypass:file", $pwFile)
}
if (-not (Test-Path $pwFile)) { throw "no password file $pwFile for $Keystore" }
# (the key's password is the store's: PKCS12, as keytool makes it)
Run "$bt\apksigner.bat" @("sign", "--ks", $Keystore, "--ks-key-alias", "redtwo", "--ks-pass", "file:$pwFile",
    "--out", $Out, "$stage\aligned.apk")
Run "$bt\apksigner.bat" @("verify", $Out)
"{0}: {1:N0} MB (version {2}, code {3})" -f $Out, ((Get-Item $Out).Length / 1MB), $VersionName, $VersionCode
