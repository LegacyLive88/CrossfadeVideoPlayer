# CrossfadeVideoPlayer

Video player with crossfade effects for Windows XP through Windows 11.

On Windows 7 and later, playback uses Media Foundation so MP4 files play without a codec pack. Older Windows versions fall back to DirectShow.

Build with MinGW-w64:

```
./build.sh
```

Or:

```
x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows -static crossfade_player.cpp -o CrossFadePlayer.exe -lstrmiids -lole32 -loleaut32 -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lgdi32 -luser32 -luuid
```

`test-files/test1.mp4` and `test-files/test2.mp4` are the sample clips. Pass them on the command line to start playing immediately:

```
CrossFadePlayer.exe test-files\test1.mp4 test-files\test2.mp4
```

The dev environment includes a Windows 10 Pro guest for manual testing. It signs in automatically as `Tester` / `tester`. The desktop has `CrossFadePlayer.exe`, `test1.mp4`, and `test2.mp4`. Nested KVM faults on this host, so the guest runs under QEMU TCG:

```
/home/ubuntu/win-test/run-win10.sh
```
