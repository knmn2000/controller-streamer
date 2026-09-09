REM controller-streamer - stream a game controller over the LAN to a Windows PC
REM Copyright (C) 2026 knmn2000
REM
REM This program is free software: you can redistribute it and/or modify
REM it under the terms of the GNU General Public License as published by
REM the Free Software Foundation, either version 3 of the License, or
REM (at your option) any later version.
REM
REM This program is distributed in the hope that it will be useful,
REM but WITHOUT ANY WARRANTY; without even the implied warranty of
REM MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
REM GNU General Public License for more details.
REM
REM You should have received a copy of the GNU General Public License
REM along with this program.  If not, see <https://www.gnu.org/licenses/>.
REM
@echo off
REM build-apk.cmd - build a signed, installable APK using the Android SDK tools
REM directly. No Gradle, no AGP, no network downloads.
REM
REM Pipeline: aapt2 compile/link -> javac -> d8 -> add dex -> zipalign -> apksigner
REM Output:   dist\controller-streamer-sender.apk   (debug-signed; sideload it)
REM
REM Everything machine-specific is detected, not hardcoded:
REM   SDK  - %ANDROID_HOME%, %ANDROID_SDK_ROOT%, or %LOCALAPPDATA%\Android\Sdk
REM   tools- highest build-tools and highest platform found in that SDK
REM   JDK  - %JAVA_HOME%, else Android Studio's bundled jbr, else javac on PATH
REM
REM Pure ASCII on purpose, same reasoning as receiver-win\setup-pc.ps1.

setlocal
set HERE=%~dp0
set OUT=%HERE%out
set DIST=%HERE%dist

echo [1/8] locating the Android SDK
if not defined SDK if defined ANDROID_HOME     set SDK=%ANDROID_HOME%
if not defined SDK if defined ANDROID_SDK_ROOT set SDK=%ANDROID_SDK_ROOT%
if not defined SDK set SDK=%LOCALAPPDATA%\Android\Sdk
if not exist "%SDK%\platforms" (
    echo   MISSING Android SDK. Looked in "%SDK%".
    echo   Install it, or set ANDROID_HOME to your SDK directory.
    exit /b 1
)
REM /o-n sorts descending, so the first hit is the newest version.
for /f "delims=" %%d in ('dir /b /ad /o-n "%SDK%\build-tools" 2^>nul') do if not defined BTV set BTV=%%d
for /f "delims=" %%d in ('dir /b /ad /o-n "%SDK%\platforms" 2^>nul')    do if not defined APL set APL=%%d
if not defined BTV ( echo   MISSING build-tools in "%SDK%\build-tools" & exit /b 1 )
if not defined APL ( echo   MISSING platforms in "%SDK%\platforms"    & exit /b 1 )
set BT=%SDK%\build-tools\%BTV%
set PLATFORM=%SDK%\platforms\%APL%\android.jar
if not exist "%PLATFORM%" ( echo   MISSING "%PLATFORM%" & exit /b 1 )
echo   SDK       %SDK%
echo   toolchain %BTV%, %APL%

echo [2/8] locating a JDK
if not defined JAVA_HOME if exist "%ProgramFiles%\Android\Android Studio\jbr\bin\javac.exe" set JAVA_HOME=%ProgramFiles%\Android\Android Studio\jbr
if not defined JAVA_HOME if exist "%LOCALAPPDATA%\Programs\Android Studio\jbr\bin\javac.exe" set JAVA_HOME=%LOCALAPPDATA%\Programs\Android Studio\jbr
if not defined JAVA_HOME for /f "delims=" %%j in ('where javac 2^>nul') do if not defined JAVAC set JAVAC=%%j
if defined JAVA_HOME set JAVAC=%JAVA_HOME%\bin\javac.exe
if not defined JAVAC ( echo   MISSING a JDK. Set JAVA_HOME or put javac on PATH. & exit /b 1 )
REM d8.bat and apksigner.bat are wrapper scripts that require JAVA_HOME.
if not defined JAVA_HOME for %%p in ("%JAVAC%") do set JAVA_HOME=%%~dp0..
set PATH=%JAVA_HOME%\bin;%PATH%
echo   JDK       %JAVA_HOME%

if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\classes" 2>nul
mkdir "%OUT%\gen" 2>nul
if not exist "%DIST%" mkdir "%DIST%"

echo [3/8] aapt2 compile + link (resources + manifest)
"%BT%\aapt2.exe" compile --dir "%HERE%res" -o "%OUT%\res.zip" || exit /b 1
"%BT%\aapt2.exe" link -I "%PLATFORM%" --manifest "%HERE%AndroidManifest.xml" ^
    -R "%OUT%\res.zip" --java "%OUT%\gen" --auto-add-overlay ^
    --min-sdk-version 24 --target-sdk-version 35 ^
    -o "%OUT%\base.apk" || exit /b 1

echo [4/8] javac
REM Sources are globbed into an argfile rather than listed, so adding a class
REM does not require editing this script.
REM -source/-target 8 because JDK 21 only permits -bootclasspath at that level,
REM and --release refuses to coexist with it at all. The sources deliberately
REM use no Java 9+ constructs; d8 handles the rest.
dir /s /b "%HERE%src\*.java" > "%OUT%\sources.txt" 2>nul
dir /s /b "%OUT%\gen\*.java" >> "%OUT%\sources.txt" 2>nul
"%JAVAC%" -source 8 -target 8 -nowarn -encoding UTF-8 -bootclasspath "%PLATFORM%" ^
    -classpath "%PLATFORM%" -d "%OUT%\classes" "@%OUT%\sources.txt" || exit /b 1

echo [5/8] d8: class files to classes.dex
REM Jar the classes first: cmd does not expand wildcards, so d8 would receive a
REM literal *.class path. A jar also picks up every anonymous inner class
REM without having to enumerate them.
"%JAVA_HOME%\bin\jar.exe" cf "%OUT%\classes.jar" -C "%OUT%\classes" . || exit /b 1
call "%BT%\d8.bat" --lib "%PLATFORM%" --min-api 24 --output "%OUT%" "%OUT%\classes.jar" || exit /b 1

echo [6/8] add classes.dex to the apk
pushd "%OUT%"
"%JAVA_HOME%\bin\jar.exe" uf base.apk classes.dex || ( popd & exit /b 1 )
popd

echo [7/8] zipalign
"%BT%\zipalign.exe" -f -p 4 "%OUT%\base.apk" "%OUT%\aligned.apk" || exit /b 1

echo [8/8] sign
REM A local debug keystore, generated on first run and gitignored. It is only
REM good for sideloading; a public release would need a real signing key.
if not exist "%HERE%debug.keystore" (
    echo   generating a local debug keystore
    "%JAVA_HOME%\bin\keytool.exe" -genkeypair -keystore "%HERE%debug.keystore" ^
        -storepass android -keypass android -alias controllerstreamer ^
        -keyalg RSA -keysize 2048 -validity 10000 ^
        -dname "CN=Controller Streamer Debug, O=controller-streamer" || exit /b 1
)
call "%BT%\apksigner.bat" sign --ks "%HERE%debug.keystore" ^
    --ks-pass pass:android --key-pass pass:android --ks-key-alias controllerstreamer ^
    --out "%DIST%\controller-streamer-sender.apk" "%OUT%\aligned.apk" || exit /b 1

call "%BT%\apksigner.bat" verify "%DIST%\controller-streamer-sender.apk" || exit /b 1
echo.
echo BUILT: %DIST%\controller-streamer-sender.apk
endlocal
