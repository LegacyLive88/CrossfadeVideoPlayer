#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0502 // Windows XP 64-bit (NT 5.2) compatibility target
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

/*
================================================================================
  CROSSFADE VIDEO PLAYER (64-BIT PORTABLE WINDOWS XP - WINDOWS 10/11)
================================================================================
  Features:
  - Portable single-executable: zero dependencies, zero installer.
  - Media Foundation playback on Windows 7+ so MP4/H.264/AAC plays without a codec pack.
  - DirectShow fallback for older systems and formats MF cannot open.
  - Dual-stream video and audio crossfade between playlist items.
  - Perceptual logarithmic audio fade-out on pause and fade-in on resume (DirectShow).
  - Linear audio fades when Media Foundation is driving playback.
  - Configurable crossfade time and pause/resume fade duration via Settings.
  - Drag-and-drop playlist management (reordering, removal, clearing).
  - Open and Save M3U playlists. Optional file paths on the command line.
  - Fullscreen the video display only (not the rest of the window): Fullscreen button,
    double-click the video, Esc to leave.
  - Keyboard: Spacebar toggles Play/Pause. Esc leaves fullscreen.
  - Win32 UI compatible across WinXP x64, Vista, 7, 8, 8.1, 10, and 11.

  Compilation (MinGW-w64):
    x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows -static crossfade_player.cpp \
      -o CrossFadePlayer.exe -lstrmiids -lole32 -loleaut32 -lcomctl32 -lcomdlg32 \
      -lshell32 -lshlwapi -lgdi32 -luser32 -luuid

  Compilation (Visual Studio x64 Native Tools Command Prompt):
    cl /O2 /std:c++17 /DUNICODE /D_UNICODE crossfade_player.cpp /Fe:CrossFadePlayer.exe \
      /link /SUBSYSTEM:WINDOWS,5.02 /MACHINE:X64 strmiids.lib ole32.lib \
      oleaut32.lib comctl32.lib comdlg32.lib shell32.lib shlwapi.lib gdi32.lib user32.lib
================================================================================
*/

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dshow.h>

// Media Foundation headers are Win7+. Raise the version only for these includes
// so the rest of the binary still loads on older Windows and falls back to DirectShow.
#pragma push_macro("_WIN32_WINNT")
#pragma push_macro("WINVER")
#undef _WIN32_WINNT
#undef WINVER
#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#include <mfapi.h>
#include <mfplay.h>
#pragma pop_macro("WINVER")
#pragma pop_macro("_WIN32_WINNT")

#include <cmath>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <iomanip>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define WM_DS_EVENT           (WM_APP + 101)
#define WM_PLAYBACK_ENDED     (WM_APP + 102)
#define TIMER_ENGINE_TICK     1001

#define IDC_BTN_PLAY          2001
#define IDC_BTN_PAUSE         2002
#define IDC_BTN_STOP          2003
#define IDC_BTN_PREV          2004
#define IDC_BTN_NEXT          2005
#define IDC_SEEKBAR           2006
#define IDC_VOLBAR            2007
#define IDC_PLAYLIST          2008
#define IDC_BTN_ADD           2009
#define IDC_BTN_REMOVE        2010
#define IDC_BTN_CLEAR         2011
#define IDC_BTN_OPEN_PL       2012
#define IDC_BTN_SAVE_PL       2013
#define IDC_BTN_SETTINGS      2014
#define IDC_TXT_STATUS        2015
#define IDC_TXT_TIME          2016
#define IDC_BTN_FULLSCREEN    2017

#define IDC_EDIT_CROSSFADE    3001
#define IDC_EDIT_FADEPAUSE    3002

enum PlaybackState {
    STATE_STOPPED,
    STATE_PLAYING,
    STATE_FADING_TO_PAUSE,
    STATE_PAUSED,
    STATE_FADING_FROM_PAUSE
};

struct PlayerSettings {
    double crossfadeDuration; // In seconds (e.g., 3.0s)
    double pauseFadeDuration; // In seconds (e.g., 1.5s)
    int masterVolume;         // 0 to 100
};

// Linear volume factor (0.0 to 1.0) to DirectShow logarithmic volume (-10000 to 0)
static long LinearToDirectShowVolume(double linearVol) {
    if (linearVol <= 0.0001) {
        return -10000;
    }
    // DirectShow volume is measured in hundredths of a decibel (dB * 100)
    double dB = 20.0 * log10(linearVol);
    long dsVol = (long)(dB * 100.0);
    if (dsVol < -10000) dsVol = -10000;
    if (dsVol > 0) dsVol = 0;
    return dsVol;
}

// Format seconds into MM:SS or HH:MM:SS
static std::wstring FormatTimeSeconds(double totalSeconds) {
    if (totalSeconds < 0.0) totalSeconds = 0.0;
    int secs = (int)totalSeconds;
    int hrs = secs / 3600;
    secs %= 3600;
    int mins = secs / 60;
    secs %= 60;

    wchar_t buf[64];
    if (hrs > 0) {
        swprintf(buf, 64, L"%02d:%02d:%02d", hrs, mins, secs);
    } else {
        swprintf(buf, 64, L"%02d:%02d", mins, secs);
    }
    return std::wstring(buf);
}

class DirectShowStream;

class MFPlayerCallback : public IMFPMediaPlayerCallback {
public:
    DirectShowStream* owner;
    LONG refCount;

    explicit MFPlayerCallback(DirectShowStream* streamOwner)
        : owner(streamOwner), refCount(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&refCount);
    }

    STDMETHODIMP_(ULONG) Release() {
        ULONG count = InterlockedDecrement(&refCount);
        if (count == 0) delete this;
        return count;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* header);
};

class DirectShowStream {
public:
    IGraphBuilder*   pGraph;
    IMediaControl*   pControl;
    IMediaSeeking*   pSeeking;
    IBasicAudio*     pAudio;
    IVideoWindow*    pVideo;
    IMFPMediaPlayer* pMF;
    MFPlayerCallback* pMFCallback;
    HWND             hContainerWnd;
    HWND             hNotifyWnd;
    std::wstring     currentFilePath;
    bool             isInitialized;
    bool             hasVideo;
    bool             useMF;
    HRESULT          lastError;

    DirectShowStream() 
        : pGraph(NULL), pControl(NULL), pSeeking(NULL), pAudio(NULL), 
          pVideo(NULL), pMF(NULL), pMFCallback(NULL), hContainerWnd(NULL),
          hNotifyWnd(NULL), isInitialized(false), hasVideo(false), useMF(false),
          lastError(S_OK) {}

    ~DirectShowStream() {
        Release();
    }

    void NotifyPlaybackEnded() {
        if (hNotifyWnd) {
            PostMessage(hNotifyWnd, WM_PLAYBACK_ENDED, 0, (LPARAM)this);
        }
    }

    void Release() {
        if (pMFCallback) pMFCallback->owner = NULL;
        if (pMF) {
            pMF->Shutdown();
            pMF->Release();
            pMF = NULL;
        }
        if (pMFCallback) {
            pMFCallback->Release();
            pMFCallback = NULL;
        }
        useMF = false;

        if (pControl) {
            pControl->Stop();
        }
        if (pVideo) {
            pVideo->put_Visible(OAFALSE);
            pVideo->put_MessageDrain((OAHWND)NULL);
            pVideo->put_Owner((OAHWND)NULL);
            pVideo->Release();
            pVideo = NULL;
        }
        if (pAudio)   { pAudio->Release();   pAudio = NULL; }
        if (pSeeking) { pSeeking->Release(); pSeeking = NULL; }
        if (pControl) { pControl->Release(); pControl = NULL; }
        if (pGraph)   { pGraph->Release();   pGraph = NULL; }
        isInitialized = false;
        hasVideo = false;
        currentFilePath.clear();
    }

    bool TryLoadMediaFoundation(const std::wstring& filePath, HWND hWndTarget, HWND notifyWnd) {
        typedef HRESULT (WINAPI *PFN_MFPCreate)(
            LPCWSTR, BOOL, MFP_CREATION_OPTIONS, IMFPMediaPlayerCallback*, HWND, IMFPMediaPlayer**);
        typedef HRESULT (WINAPI *PFN_MFStartup)(ULONG, DWORD);

        static PFN_MFPCreate pfnCreate = NULL;
        static bool resolved = false;
        if (!resolved) {
            resolved = true;
            HMODULE hPlat = LoadLibraryW(L"mfplat.dll");
            HMODULE hPlay = LoadLibraryW(L"mfplay.dll");
            if (hPlat && hPlay) {
                PFN_MFStartup pfnStartup = (PFN_MFStartup)GetProcAddress(hPlat, "MFStartup");
                if (pfnStartup) {
                    pfnStartup(MF_VERSION, MFSTARTUP_FULL);
                }
                pfnCreate = (PFN_MFPCreate)GetProcAddress(hPlay, "MFPCreateMediaPlayer");
            }
        }
        if (!pfnCreate) {
            lastError = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
            return false;
        }

        MFPlayerCallback* callback = new MFPlayerCallback(this);
        IMFPMediaPlayer* player = NULL;
        HRESULT hr = pfnCreate(NULL, FALSE, MFP_OPTION_NONE, callback, hWndTarget, &player);
        if (FAILED(hr) || !player) {
            lastError = FAILED(hr) ? hr : E_FAIL;
            callback->owner = NULL;
            callback->Release();
            return false;
        }

        IMFPMediaItem* item = NULL;
        hr = player->CreateMediaItemFromURL(filePath.c_str(), TRUE, 0, &item);
        if (FAILED(hr) || !item) {
            lastError = FAILED(hr) ? hr : E_FAIL;
            callback->owner = NULL;
            player->Shutdown();
            player->Release();
            callback->Release();
            return false;
        }

        hr = player->SetMediaItem(item);
        item->Release();
        if (FAILED(hr)) {
            lastError = hr;
            callback->owner = NULL;
            player->Shutdown();
            player->Release();
            callback->Release();
            return false;
        }

        pMF = player;
        pMFCallback = callback;
        useMF = true;
        hContainerWnd = hWndTarget;
        hNotifyWnd = notifyWnd;
        currentFilePath = filePath;
        isInitialized = true;
        lastError = S_OK;

        SIZE nativeSize = { 0, 0 };
        if (SUCCEEDED(player->GetNativeVideoSize(&nativeSize, NULL)) && nativeSize.cx > 0 && nativeSize.cy > 0) {
            hasVideo = true;
        }
        player->SetAspectRatioMode(MFVideoARMode_PreservePicture);
        player->SetBorderColor(RGB(0, 0, 0));
        player->UpdateVideo();
        return true;
    }

    bool Load(const std::wstring& filePath, HWND hWndTarget, HWND notifyWnd) {
        Release();
        hContainerWnd = hWndTarget;
        hNotifyWnd = notifyWnd;
        lastError = S_OK;

        if (TryLoadMediaFoundation(filePath, hWndTarget, notifyWnd)) {
            return true;
        }

        HRESULT mfError = lastError;

        HRESULT hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
                                      IID_IGraphBuilder, (void**)&pGraph);
        if (FAILED(hr) || !pGraph) return false;

        pGraph->QueryInterface(IID_IMediaControl, (void**)&pControl);
        pGraph->QueryInterface(IID_IMediaSeeking, (void**)&pSeeking);
        pGraph->QueryInterface(IID_IBasicAudio,   (void**)&pAudio);
        pGraph->QueryInterface(IID_IVideoWindow,  (void**)&pVideo);

        hr = pGraph->RenderFile(filePath.c_str(), NULL);
        if (FAILED(hr)) {
            lastError = FAILED(mfError) ? mfError : hr;
            Release();
            lastError = FAILED(mfError) ? mfError : hr;
            return false;
        }

        // Setup notification routing
        IMediaEventEx* pEvent = NULL;
        if (SUCCEEDED(pGraph->QueryInterface(IID_IMediaEventEx, (void**)&pEvent)) && pEvent) {
            pEvent->SetNotifyWindow((OAHWND)notifyWnd, WM_DS_EVENT, (LONG_PTR)this);
            pEvent->Release();
        }

        // Setup video output window
        if (pVideo) {
            long lVisible = 0;
            if (SUCCEEDED(pVideo->get_Visible(&lVisible))) {
                hasVideo = true;
                pVideo->put_Owner((OAHWND)hContainerWnd);
                pVideo->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
                // Mouse input on the renderer belongs to the video surface so a
                // double-click there can toggle fullscreen.
                pVideo->put_MessageDrain((OAHWND)hContainerWnd);
                ResizeToWindow();
                pVideo->put_Visible(OATRUE);
            }
        }

        currentFilePath = filePath;
        isInitialized = true;
        return true;
    }

    void RefreshVideo() {
        // MFPlay does not keep painting on its own. UpdateVideo blits the
        // current frame; without it the window stays on the first picture
        // while the clock keeps advancing.
        if (useMF && pMF && hasVideo) {
            pMF->UpdateVideo();
        }
    }

    void ResizeToWindow() {
        if (useMF && pMF) {
            pMF->UpdateVideo();
            return;
        }
        if (pVideo && hContainerWnd) {
            RECT rc;
            GetClientRect(hContainerWnd, &rc);
            pVideo->SetWindowPosition(0, 0, rc.right - rc.left, rc.bottom - rc.top);
        }
    }

    void Play() {
        if (useMF && pMF) {
            pMF->Play();
            return;
        }
        if (pControl) pControl->Run();
    }

    void Pause() {
        if (useMF && pMF) {
            pMF->Pause();
            return;
        }
        if (pControl) pControl->Pause();
    }

    void Stop() {
        if (useMF && pMF) {
            pMF->Stop();
            SetPosition(0.0);
            return;
        }
        if (pControl) {
            pControl->Stop();
            LONGLONG pos = 0;
            if (pSeeking) {
                pSeeking->SetPositions(&pos, AM_SEEKING_AbsolutePositioning, NULL, AM_SEEKING_NoPositioning);
            }
        }
    }

    void SetVolume(double volumeFactor) {
        if (volumeFactor < 0.0) volumeFactor = 0.0;
        if (volumeFactor > 1.0) volumeFactor = 1.0;
        if (useMF && pMF) {
            pMF->SetVolume((float)volumeFactor);
            return;
        }
        if (pAudio) {
            pAudio->put_Volume(LinearToDirectShowVolume(volumeFactor));
        }
    }

    static double HundredNsToSeconds(const PROPVARIANT& value) {
        if (value.vt == VT_I8) return (double)value.hVal.QuadPart / 10000000.0;
        if (value.vt == VT_UI8) return (double)value.uhVal.QuadPart / 10000000.0;
        return 0.0;
    }

    double GetDuration() {
        if (useMF && pMF) {
            PROPVARIANT value;
            PropVariantInit(&value);
            double seconds = 0.0;
            if (SUCCEEDED(pMF->GetDuration(MFP_POSITIONTYPE_100NS, &value))) {
                seconds = HundredNsToSeconds(value);
            }
            PropVariantClear(&value);
            return seconds;
        }
        if (!pSeeking) return 0.0;
        LONGLONG duration = 0;
        if (SUCCEEDED(pSeeking->GetDuration(&duration))) {
            return (double)duration / 10000000.0; // 100ns units to seconds
        }
        return 0.0;
    }

    double GetPosition() {
        if (useMF && pMF) {
            PROPVARIANT value;
            PropVariantInit(&value);
            double seconds = 0.0;
            if (SUCCEEDED(pMF->GetPosition(MFP_POSITIONTYPE_100NS, &value))) {
                seconds = HundredNsToSeconds(value);
            }
            PropVariantClear(&value);
            return seconds;
        }
        if (!pSeeking) return 0.0;
        LONGLONG curPos = 0;
        if (SUCCEEDED(pSeeking->GetCurrentPosition(&curPos))) {
            return (double)curPos / 10000000.0;
        }
        return 0.0;
    }

    void SetPosition(double seconds) {
        if (seconds < 0.0) seconds = 0.0;
        if (useMF && pMF) {
            PROPVARIANT value;
            PropVariantInit(&value);
            value.vt = VT_I8;
            value.hVal.QuadPart = (LONGLONG)(seconds * 10000000.0);
            pMF->SetPosition(MFP_POSITIONTYPE_100NS, &value);
            PropVariantClear(&value);
            return;
        }
        if (!pSeeking) return;
        LONGLONG pos = (LONGLONG)(seconds * 10000000.0);
        pSeeking->SetPositions(&pos, AM_SEEKING_AbsolutePositioning, NULL, AM_SEEKING_NoPositioning);
    }
};

void STDMETHODCALLTYPE MFPlayerCallback::OnMediaPlayerEvent(MFP_EVENT_HEADER* header) {
    if (!header || !owner) return;
    if (header->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
        owner->NotifyPlaybackEnded();
    } else if (header->eEventType == MFP_EVENT_TYPE_ERROR) {
        owner->lastError = header->hrEvent;
    }
}

static LRESULT CALLBACK VideoHostProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

class VideoPlayerApp {
public:
    HWND m_hWndMain;
    HWND m_hWndVideoBase;
    HWND m_hWndVideoOverlay;
    HWND m_hWndFullscreen;

    // UI Controls
    HWND m_hBtnPlay;
    HWND m_hBtnPause;
    HWND m_hBtnStop;
    HWND m_hBtnPrev;
    HWND m_hBtnNext;
    HWND m_hBtnFull;
    HWND m_hSeekBar;
    HWND m_hVolBar;
    HWND m_hLblVol;
    HWND m_hPlaylist;
    HWND m_hBtnAdd;
    HWND m_hBtnRemove;
    HWND m_hBtnClear;
    HWND m_hBtnOpenPL;
    HWND m_hBtnSavePL;
    HWND m_hBtnSettings;
    HWND m_hTxtStatus;
    HWND m_hTxtTime;

    // Playback and Crossfade Streams
    DirectShowStream m_streamA;
    DirectShowStream m_streamB;
    bool             m_isStreamBActive; // When true, stream B is main, A is secondary

    // Playlist Data
    std::vector<std::wstring> m_playlistFiles;
    int                       m_currentTrackIndex;

    // Settings
    PlayerSettings m_settings;

    // Fade State Machine
    PlaybackState m_state;
    double        m_fadeTimerSeconds;
    bool          m_isCrossfading;
    double        m_crossfadeTimer;
    bool          m_userIsDraggingSeek;
    bool          m_isFullscreen;

    VideoPlayerApp()
        : m_hWndMain(NULL), m_hWndVideoBase(NULL), m_hWndVideoOverlay(NULL),
          m_hWndFullscreen(NULL),
          m_hBtnPlay(NULL), m_hBtnPause(NULL), m_hBtnStop(NULL), m_hBtnPrev(NULL),
          m_hBtnNext(NULL), m_hBtnFull(NULL), m_hSeekBar(NULL), m_hVolBar(NULL),
          m_hLblVol(NULL), m_hPlaylist(NULL),
          m_hBtnAdd(NULL), m_hBtnRemove(NULL), m_hBtnClear(NULL), m_hBtnOpenPL(NULL),
          m_hBtnSavePL(NULL), m_hBtnSettings(NULL), m_hTxtStatus(NULL), m_hTxtTime(NULL),
          m_isStreamBActive(false), m_currentTrackIndex(-1), m_state(STATE_STOPPED),
          m_fadeTimerSeconds(0.0), m_isCrossfading(false), m_crossfadeTimer(0.0),
          m_userIsDraggingSeek(false), m_isFullscreen(false)
    {
        m_settings.crossfadeDuration = 3.0; // Default 3s crossfade
        m_settings.pauseFadeDuration = 1.5; // Default 1.5s audio fade on pause/play
        m_settings.masterVolume = 85;       // 85% volume
    }

    DirectShowStream* GetActiveStream() {
        return m_isStreamBActive ? &m_streamB : &m_streamA;
    }

    DirectShowStream* GetIncomingStream() {
        return m_isStreamBActive ? &m_streamA : &m_streamB;
    }

    HWND GetActiveVideoHWND() {
        return m_isStreamBActive ? m_hWndVideoOverlay : m_hWndVideoBase;
    }

    HWND GetIncomingVideoHWND() {
        return m_isStreamBActive ? m_hWndVideoBase : m_hWndVideoOverlay;
    }

    void CreateControls(HWND hWndParent, HINSTANCE hInstance) {
        m_hWndMain = hWndParent;

        // Dedicated viewport. A STATIC black rect paints over Media Foundation video.
        WNDCLASS wcHost = { 0 };
        wcHost.style = CS_DBLCLKS;
        wcHost.lpfnWndProc = VideoHostProc;
        wcHost.hInstance = hInstance;
        wcHost.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcHost.lpszClassName = L"CrossfadeVideoHost";
        wcHost.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClass(&wcHost);

        m_hWndVideoBase = CreateWindowEx(
            0, L"CrossfadeVideoHost", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            10, 10, 640, 360,
            hWndParent, NULL, hInstance, NULL
        );

        // Overlay window for crossfading. Uses WS_POPUP and WS_EX_LAYERED for XP+ crossfade support.
        WNDCLASS wcOverlay = { 0 };
        wcOverlay.style = CS_DBLCLKS;
        wcOverlay.lpfnWndProc = VideoHostProc;
        wcOverlay.hInstance = hInstance;
        wcOverlay.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcOverlay.lpszClassName = L"CrossfadeOverlayClass";
        wcOverlay.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClass(&wcOverlay);

        m_hWndVideoOverlay = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW,
            L"CrossfadeOverlayClass", L"",
            WS_POPUP | WS_CLIPCHILDREN,
            0, 0, 100, 100,
            hWndParent, NULL, hInstance, NULL
        );
        SetLayeredWindowAttributes(m_hWndVideoOverlay, 0, 0, LWA_ALPHA);
        ShowWindow(m_hWndVideoOverlay, SW_HIDE);

        // Seek Bar (Trackbar)
        m_hSeekBar = CreateWindowEx(
            0, TRACKBAR_CLASS, L"",
            WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_NOTICKS,
            10, 375, 640, 25,
            hWndParent, (HMENU)IDC_SEEKBAR, hInstance, NULL
        );
        SendMessage(m_hSeekBar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));

        // Time Label
        m_hTxtTime = CreateWindowEx(
            0, L"STATIC", L"00:00 / 00:00",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            500, 405, 150, 20,
            hWndParent, (HMENU)IDC_TXT_TIME, hInstance, NULL
        );

        // Status Label (e.g. Playing, Crossfading, Pausing...)
        m_hTxtStatus = CreateWindowEx(
            0, L"STATIC", L"Ready. Drag & Drop video files here.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 405, 480, 20,
            hWndParent, (HMENU)IDC_TXT_STATUS, hInstance, NULL
        );

        // Control Buttons
        m_hBtnPrev = CreateWindow(L"BUTTON", L"|<< Prev", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 430, 75, 30, hWndParent, (HMENU)IDC_BTN_PREV, hInstance, NULL);

        m_hBtnPlay = CreateWindow(L"BUTTON", L"> Play", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            90, 430, 75, 30, hWndParent, (HMENU)IDC_BTN_PLAY, hInstance, NULL);

        m_hBtnPause = CreateWindow(L"BUTTON", L"|| Pause", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            170, 430, 75, 30, hWndParent, (HMENU)IDC_BTN_PAUSE, hInstance, NULL);

        m_hBtnStop = CreateWindow(L"BUTTON", L"[] Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            250, 430, 75, 30, hWndParent, (HMENU)IDC_BTN_STOP, hInstance, NULL);

        m_hBtnNext = CreateWindow(L"BUTTON", L"Next >>|", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            330, 430, 75, 30, hWndParent, (HMENU)IDC_BTN_NEXT, hInstance, NULL);

        m_hBtnFull = CreateWindow(L"BUTTON", L"Fullscreen", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            410, 430, 92, 30, hWndParent, (HMENU)IDC_BTN_FULLSCREEN, hInstance, NULL);

        // Volume Bar
        m_hLblVol = CreateWindow(L"STATIC", L"Volume:", WS_CHILD | WS_VISIBLE,
            508, 435, 52, 20, hWndParent, NULL, hInstance, NULL);

        m_hVolBar = CreateWindowEx(
            0, TRACKBAR_CLASS, L"",
            WS_CHILD | WS_VISIBLE | TBS_NOTICKS,
            562, 430, 88, 30,
            hWndParent, (HMENU)IDC_VOLBAR, hInstance, NULL
        );
        SendMessage(m_hVolBar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessage(m_hVolBar, TBM_SETPOS, TRUE, m_settings.masterVolume);

        // Playlist View (Listbox)
        HWND hLblPL = CreateWindow(L"STATIC", L"Playlist (Drag & drop videos here):", WS_CHILD | WS_VISIBLE,
            665, 10, 280, 20, hWndParent, NULL, hInstance, NULL);

        m_hPlaylist = CreateWindowEx(
            WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            665, 32, 280, 395,
            hWndParent, (HMENU)IDC_PLAYLIST, hInstance, NULL
        );

        // Playlist & Settings Action Buttons
        m_hBtnAdd = CreateWindow(L"BUTTON", L"+ Add", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            665, 430, 52, 30, hWndParent, (HMENU)IDC_BTN_ADD, hInstance, NULL);

        m_hBtnRemove = CreateWindow(L"BUTTON", L"- Del", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            721, 430, 52, 30, hWndParent, (HMENU)IDC_BTN_REMOVE, hInstance, NULL);

        m_hBtnClear = CreateWindow(L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            777, 430, 52, 30, hWndParent, (HMENU)IDC_BTN_CLEAR, hInstance, NULL);

        m_hBtnOpenPL = CreateWindow(L"BUTTON", L"Open", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            833, 430, 52, 30, hWndParent, (HMENU)IDC_BTN_OPEN_PL, hInstance, NULL);

        m_hBtnSavePL = CreateWindow(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            889, 430, 56, 30, hWndParent, (HMENU)IDC_BTN_SAVE_PL, hInstance, NULL);

        m_hBtnSettings = CreateWindow(L"BUTTON", L"Settings...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            665, 465, 280, 26, hWndParent, (HMENU)IDC_BTN_SETTINGS, hInstance, NULL);

        // Set modern font on all controls
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        EnumChildWindows(hWndParent, [](HWND hWnd, LPARAM lParam) -> BOOL {
            SendMessage(hWnd, WM_SETFONT, lParam, TRUE);
            return TRUE;
        }, (LPARAM)hFont);

        // Enable Drag and Drop
        DragAcceptFiles(hWndParent, TRUE);
        DragAcceptFiles(m_hPlaylist, TRUE);

        UpdateSyncOverlayPosition();
    }

    void UpdateSyncOverlayPosition() {
        if (!m_hWndVideoBase || !m_hWndVideoOverlay) return;
        RECT rcBase;
        GetWindowRect(m_hWndVideoBase, &rcBase);
        // During fullscreen the video shell is topmost; the crossfade overlay has to
        // sit above it, and must drop back out of the topmost band afterwards.
        SetWindowPos(m_hWndVideoOverlay, m_isFullscreen ? HWND_TOPMOST : HWND_NOTOPMOST,
                     rcBase.left, rcBase.top,
                     rcBase.right - rcBase.left, rcBase.bottom - rcBase.top,
                     SWP_NOACTIVATE | (m_isCrossfading ? SWP_SHOWWINDOW : 0));

        m_streamA.ResizeToWindow();
        m_streamB.ResizeToWindow();
    }

    void OnResize() {
        RECT rcClient;
        GetClientRect(m_hWndMain, &rcClient);
        int clientW = rcClient.right - rcClient.left;
        int clientH = rcClient.bottom - rcClient.top;

        int plWidth = 280;
        int bottomBarH = 120;
        int videoW = (clientW - plWidth - 30);
        int videoH = clientH - bottomBarH - 20;

        if (videoW < 200) videoW = 200;
        if (videoH < 150) videoH = 150;

        // While fullscreen the video host is a child of the monitor-sized shell.
        if (!m_isFullscreen) {
            MoveWindow(m_hWndVideoBase, 10, 10, videoW, videoH, TRUE);
        }

        // Position seekbar and time labels
        int seekY = videoH + 15;
        MoveWindow(m_hSeekBar, 10, seekY, videoW, 25, TRUE);
        MoveWindow(m_hTxtStatus, 10, seekY + 28, videoW - 160, 20, TRUE);
        MoveWindow(m_hTxtTime, videoW - 150, seekY + 28, 150, 20, TRUE);

        // Playback control buttons
        int btnY = seekY + 52;
        MoveWindow(m_hBtnPrev, 10, btnY, 70, 30, TRUE);
        MoveWindow(m_hBtnPlay, 85, btnY, 70, 30, TRUE);
        MoveWindow(m_hBtnPause, 160, btnY, 70, 30, TRUE);
        MoveWindow(m_hBtnStop, 235, btnY, 70, 30, TRUE);
        MoveWindow(m_hBtnNext, 310, btnY, 70, 30, TRUE);

        int fullW = 92;
        int fullX = 385;
        MoveWindow(m_hBtnFull, fullX, btnY, fullW, 30, TRUE);
        int labelW = 52;
        int labelX = fullX + fullW + 6;
        if (m_hLblVol) MoveWindow(m_hLblVol, labelX, btnY + 6, labelW, 18, TRUE);
        int volX = labelX + labelW + 2;
        int volW = videoW - volX;
        if (volW < 40) volW = 40;
        MoveWindow(m_hVolBar, volX, btnY, volW, 30, TRUE);

        // Playlist section
        int plX = videoW + 20;
        MoveWindow(m_hPlaylist, plX, 30, plWidth, clientH - 110, TRUE);
        int plBtnY = clientH - 72;
        int bW = plWidth / 5;
        MoveWindow(m_hBtnAdd,    plX,             plBtnY, bW - 2, 28, TRUE);
        MoveWindow(m_hBtnRemove, plX + bW,        plBtnY, bW - 2, 28, TRUE);
        MoveWindow(m_hBtnClear,  plX + bW * 2,    plBtnY, bW - 2, 28, TRUE);
        MoveWindow(m_hBtnOpenPL, plX + bW * 3,    plBtnY, bW - 2, 28, TRUE);
        MoveWindow(m_hBtnSavePL, plX + bW * 4,    plBtnY, plWidth - (bW * 4), 28, TRUE);
        MoveWindow(m_hBtnSettings, plX, plBtnY + 32, plWidth, 26, TRUE);

        UpdateSyncOverlayPosition();
    }

    void UpdateFullscreenButton() {
        if (!m_hBtnFull) return;
        SetWindowText(m_hBtnFull, m_isFullscreen ? L"Exit Full" : L"Fullscreen");
    }

    bool MonitorRectForVideo(RECT* outRect) {
        if (!outRect || !m_hWndVideoBase) return false;
        HMONITOR monitor = MonitorFromWindow(m_hWndVideoBase, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info;
        info.cbSize = sizeof(info);
        if (GetMonitorInfo(monitor, &info)) {
            *outRect = info.rcMonitor;
            return true;
        }
        outRect->left = 0;
        outRect->top = 0;
        outRect->right = GetSystemMetrics(SM_CXSCREEN);
        outRect->bottom = GetSystemMetrics(SM_CYSCREEN);
        return true;
    }

    void EnterFullscreen() {
        if (m_isFullscreen || !m_hWndVideoBase || !m_hWndMain) return;

        RECT rc;
        if (!MonitorRectForVideo(&rc)) return;
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;
        if (width < 1 || height < 1) return;

        HINSTANCE inst = (HINSTANCE)GetWindowLongPtr(m_hWndMain, GWLP_HINSTANCE);
        HWND shell = CreateWindowEx(
            WS_EX_TOPMOST,
            L"CrossfadeVideoHost",
            L"",
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            rc.left, rc.top, width, height,
            m_hWndMain, NULL, inst, NULL
        );
        if (!shell) return;

        m_hWndFullscreen = shell;
        m_isFullscreen = true;
        SetParent(m_hWndVideoBase, shell);
        MoveWindow(m_hWndVideoBase, 0, 0, width, height, TRUE);
        ShowWindow(shell, SW_SHOW);
        UpdateFullscreenButton();
        UpdateSyncOverlayPosition();
        SetForegroundWindow(shell);
        SetFocus(m_hWndVideoBase);
    }

    void ExitFullscreen() {
        if (!m_isFullscreen) return;
        m_isFullscreen = false;

        if (m_hWndVideoBase && m_hWndMain) {
            ShowWindow(m_hWndVideoBase, SW_HIDE);
            SetParent(m_hWndVideoBase, m_hWndMain);
        }
        if (m_hWndFullscreen && GetParent(m_hWndVideoBase) == m_hWndMain) {
            HWND shell = m_hWndFullscreen;
            m_hWndFullscreen = NULL;
            DestroyWindow(shell);
        }
        UpdateFullscreenButton();
        if (m_hWndMain) {
            OnResize();
            ShowWindow(m_hWndVideoBase, SW_SHOW);
            SetForegroundWindow(m_hWndMain);
        }
    }

    void ToggleFullscreen() {
        if (m_isFullscreen) ExitFullscreen();
        else EnterFullscreen();
    }

    void AddFileToPlaylist(const std::wstring& path) {
        m_playlistFiles.push_back(path);
        std::wstring fileName = PathFindFileName(path.c_str());
        SendMessage(m_hPlaylist, LB_ADDSTRING, 0, (LPARAM)fileName.c_str());
    }

    void HandleDropFiles(HDROP hDrop) {
        UINT count = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < count; ++i) {
            wchar_t filePath[MAX_PATH];
            if (DragQueryFile(hDrop, i, filePath, MAX_PATH)) {
                AddFileToPlaylist(filePath);
            }
        }
        DragFinish(hDrop);

        if (m_state == STATE_STOPPED && m_currentTrackIndex == -1 && !m_playlistFiles.empty()) {
            PlayTrackIndex(0);
        }
    }

    void RemoveSelectedTrack() {
        int sel = (int)SendMessage(m_hPlaylist, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < (int)m_playlistFiles.size()) {
            if (sel == m_currentTrackIndex) {
                StopPlayback();
            }
            m_playlistFiles.erase(m_playlistFiles.begin() + sel);
            SendMessage(m_hPlaylist, LB_DELETESTRING, sel, 0);
            if (sel < m_currentTrackIndex) {
                m_currentTrackIndex--;
            }
        }
    }

    void ClearPlaylist() {
        StopPlayback();
        m_playlistFiles.clear();
        SendMessage(m_hPlaylist, LB_RESETCONTENT, 0, 0);
        m_currentTrackIndex = -1;
    }

    void SavePlaylist() {
        wchar_t szFile[MAX_PATH] = L"playlist.m3u";
        OPENFILENAME ofn = { 0 };
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hWndMain;
        ofn.lpstrFilter = L"M3U Playlist (*.m3u)\0*.m3u\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        ofn.lpstrDefExt = L"m3u";

        if (GetSaveFileName(&ofn)) {
            std::wofstream out(szFile);
            if (out.is_open()) {
                out << L"#EXTM3U\n";
                for (const auto& path : m_playlistFiles) {
                    out << path << L"\n";
                }
                SetStatusText(L"Playlist saved.");
            }
        }
    }

    void OpenPlaylist() {
        wchar_t szFile[MAX_PATH] = { 0 };
        OPENFILENAME ofn = { 0 };
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hWndMain;
        ofn.lpstrFilter = L"Playlists (*.m3u)\0*.m3u\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (GetOpenFileName(&ofn)) {
            std::wifstream in(szFile);
            if (in.is_open()) {
                ClearPlaylist();
                std::wstring line;
                while (std::getline(in, line)) {
                    if (line.empty() || line[0] == L'#') continue;
                    AddFileToPlaylist(line);
                }
                SetStatusText(L"Playlist loaded.");
                if (!m_playlistFiles.empty()) {
                    PlayTrackIndex(0);
                }
            }
        }
    }

    void SetStatusText(const std::wstring& status) {
        SetWindowText(m_hTxtStatus, status.c_str());
    }

    double GetNormalizedMasterVolume() {
        return (double)m_settings.masterVolume / 100.0;
    }

    void PlayTrackIndex(int index) {
        if (index < 0 || index >= (int)m_playlistFiles.size()) return;

        // Cancel any existing crossfade
        if (m_isCrossfading) {
            CompleteCrossfade();
        }

        m_currentTrackIndex = index;
        SendMessage(m_hPlaylist, LB_SETCURSEL, index, 0);

        DirectShowStream* pActive = GetActiveStream();
        HWND hBase = GetActiveVideoHWND();

        if (pActive->Load(m_playlistFiles[index], hBase, m_hWndMain)) {
            pActive->SetVolume(GetNormalizedMasterVolume());
            pActive->Play();
            m_state = STATE_PLAYING;
            SetStatusText(L"Playing: " + std::wstring(PathFindFileName(m_playlistFiles[index].c_str())));
            UpdateSyncOverlayPosition();
        } else {
            wchar_t errBuf[160];
            swprintf(errBuf, 160, L"Error: Could not render media file (%08X).",
                     (unsigned)pActive->lastError);
            SetStatusText(errBuf);
            m_state = STATE_STOPPED;
        }
    }

    void StartAudioPauseFade() {
        if (m_state == STATE_PLAYING) {
            m_state = STATE_FADING_TO_PAUSE;
            m_fadeTimerSeconds = 0.0;
            SetStatusText(L"Pausing (Fading audio out)...");
        }
    }

    void StartAudioResumeFade() {
        if (m_state == STATE_PAUSED || m_state == STATE_FADING_TO_PAUSE) {
            DirectShowStream* pActive = GetActiveStream();
            pActive->SetVolume(0.0);
            pActive->Play();
            m_state = STATE_FADING_FROM_PAUSE;
            m_fadeTimerSeconds = 0.0;
            SetStatusText(L"Resuming (Fading audio in)...");
        }
    }

    void TogglePlayPause() {
        if (m_state == STATE_PLAYING) {
            StartAudioPauseFade();
        } else if (m_state == STATE_PAUSED || m_state == STATE_FADING_TO_PAUSE) {
            StartAudioResumeFade();
        } else if (m_state == STATE_STOPPED) {
            if (m_currentTrackIndex >= 0) {
                PlayTrackIndex(m_currentTrackIndex);
            } else if (!m_playlistFiles.empty()) {
                PlayTrackIndex(0);
            }
        }
    }

    void StopPlayback() {
        if (m_isCrossfading) {
            CompleteCrossfade();
        }
        m_streamA.Stop();
        m_streamB.Stop();
        m_streamA.Release();
        m_streamB.Release();
        m_state = STATE_STOPPED;
        ShowWindow(m_hWndVideoOverlay, SW_HIDE);
        SetStatusText(L"Stopped.");
        SetWindowText(m_hTxtTime, L"00:00 / 00:00");
        SendMessage(m_hSeekBar, TBM_SETPOS, TRUE, 0);
    }

    void NextTrack() {
        if (m_playlistFiles.empty()) return;
        int nextIndex = m_currentTrackIndex + 1;
        if (nextIndex < (int)m_playlistFiles.size()) {
            PlayTrackIndex(nextIndex);
        } else {
            StopPlayback();
        }
    }

    void PrevTrack() {
        if (m_playlistFiles.empty()) return;
        int prevIndex = m_currentTrackIndex - 1;
        if (prevIndex >= 0) {
            PlayTrackIndex(prevIndex);
        }
    }

    void TriggerCrossfadeToNext() {
        if (m_isCrossfading) return;
        int nextIndex = m_currentTrackIndex + 1;
        if (nextIndex >= (int)m_playlistFiles.size()) return; // End of playlist

        DirectShowStream* pIncoming = GetIncomingStream();
        HWND hTargetWnd = GetIncomingVideoHWND();

        if (pIncoming->Load(m_playlistFiles[nextIndex], hTargetWnd, m_hWndMain)) {
            m_isCrossfading = true;
            m_crossfadeTimer = 0.0;

            // Prepare initial crossfade state
            pIncoming->SetVolume(0.0);
            pIncoming->Play();

            // Overlay display preparation
            UpdateSyncOverlayPosition();
            ShowWindow(m_hWndVideoOverlay, SW_SHOWNA);
            SetLayeredWindowAttributes(m_hWndVideoOverlay, 0, 0, LWA_ALPHA);

            SetStatusText(L"Crossfading into: " + std::wstring(PathFindFileName(m_playlistFiles[nextIndex].c_str())));
        }
    }

    void CompleteCrossfade() {
        if (!m_isCrossfading) return;
        m_isCrossfading = false;

        DirectShowStream* pOutgoing = GetActiveStream();
        DirectShowStream* pIncoming = GetIncomingStream();

        pOutgoing->Stop();
        pOutgoing->Release();

        // Swap roles
        m_isStreamBActive = !m_isStreamBActive;
        m_currentTrackIndex++;
        SendMessage(m_hPlaylist, LB_SETCURSEL, m_currentTrackIndex, 0);

        // Normalize final volumes
        pIncoming->SetVolume(GetNormalizedMasterVolume());

        ShowWindow(m_hWndVideoOverlay, SW_HIDE);
        SetStatusText(L"Playing: " + std::wstring(PathFindFileName(m_playlistFiles[m_currentTrackIndex].c_str())));
    }

    void OnTimerTick(double deltaTime) {
        DirectShowStream* pActive = GetActiveStream();

        // 1. Audio Fade out on Pause
        if (m_state == STATE_FADING_TO_PAUSE) {
            m_fadeTimerSeconds += deltaTime;
            double progress = m_fadeTimerSeconds / m_settings.pauseFadeDuration;
            if (progress >= 1.0) {
                pActive->SetVolume(0.0);
                pActive->Pause();
                m_state = STATE_PAUSED;
                SetStatusText(L"Paused.");
            } else {
                double linear = (1.0 - progress) * GetNormalizedMasterVolume();
                pActive->SetVolume(linear);
            }
        }

        // 2. Audio Fade in on Resume
        if (m_state == STATE_FADING_FROM_PAUSE) {
            m_fadeTimerSeconds += deltaTime;
            double progress = m_fadeTimerSeconds / m_settings.pauseFadeDuration;
            if (progress >= 1.0) {
                pActive->SetVolume(GetNormalizedMasterVolume());
                m_state = STATE_PLAYING;
                SetStatusText(L"Playing.");
            } else {
                double linear = progress * GetNormalizedMasterVolume();
                pActive->SetVolume(linear);
            }
        }

        // 3. Playback position, seek bar updates and crossfade triggers
        if (m_state == STATE_PLAYING || m_state == STATE_FADING_TO_PAUSE || m_state == STATE_FADING_FROM_PAUSE) {
            double curPos = pActive->GetPosition();
            double duration = pActive->GetDuration();

            if (duration > 0.0) {
                if (!m_userIsDraggingSeek) {
                    int tickPos = (int)((curPos / duration) * 1000.0);
                    SendMessage(m_hSeekBar, TBM_SETPOS, TRUE, tickPos);
                }

                std::wstring timeStr = FormatTimeSeconds(curPos) + L" / " + FormatTimeSeconds(duration);
                SetWindowText(m_hTxtTime, timeStr.c_str());

                // Check crossfade lookahead threshold
                double remaining = duration - curPos;
                if (!m_isCrossfading && remaining <= m_settings.crossfadeDuration && remaining > 0.1) {
                    if (m_currentTrackIndex + 1 < (int)m_playlistFiles.size()) {
                        TriggerCrossfadeToNext();
                    }
                }
            }
        }

        if (m_state == STATE_PLAYING || m_state == STATE_FADING_TO_PAUSE ||
            m_state == STATE_FADING_FROM_PAUSE || m_state == STATE_PAUSED || m_isCrossfading) {
            m_streamA.RefreshVideo();
            m_streamB.RefreshVideo();
        }

        // 4. Update crossfade progression
        if (m_isCrossfading) {
            m_crossfadeTimer += deltaTime;
            double progress = m_crossfadeTimer / m_settings.crossfadeDuration;
            if (progress >= 1.0) {
                CompleteCrossfade();
            } else {
                // Smooth cosine / linear blending
                double blend = progress;
                BYTE overlayAlpha = (BYTE)(blend * 255.0);
                SetLayeredWindowAttributes(m_hWndVideoOverlay, 0, overlayAlpha, LWA_ALPHA);

                // Crossfade Audio levels
                DirectShowStream* pOutgoing = GetActiveStream();
                DirectShowStream* pIncoming = GetIncomingStream();

                double master = GetNormalizedMasterVolume();
                pOutgoing->SetVolume((1.0 - blend) * master);
                pIncoming->SetVolume(blend * master);
            }
        }
    }
};

static VideoPlayerApp* g_pApp = NULL;

// DirectShow's video window often lacks CS_DBLCLKS, so a second click arrives as
// WM_LBUTTONDOWN instead of WM_LBUTTONDBLCLK. Track that case with the system
// double-click interval. Windows that do send DBLCLK are handled separately.
struct VideoDoubleClickTracker {
    HWND hwnd;
    DWORD time;
    int x;
    int y;

    VideoDoubleClickTracker() : hwnd(NULL), time(0), x(0), y(0) {}

    void Reset() {
        hwnd = NULL;
        time = 0;
    }

    bool OnButtonDown(HWND target, LPARAM lParam) {
        int cx = (int)(short)LOWORD(lParam);
        int cy = (int)(short)HIWORD(lParam);
        DWORD now = (DWORD)GetMessageTime();
        int slopX = GetSystemMetrics(SM_CXDOUBLECLK);
        int slopY = GetSystemMetrics(SM_CYDOUBLECLK);
        if (slopX < 1) slopX = 1;
        if (slopY < 1) slopY = 1;
        int dx = cx - x;
        int dy = cy - y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        bool match = hwnd == target
            && (now - time) <= (DWORD)GetDoubleClickTime()
            && dx <= slopX
            && dy <= slopY;
        if (match) {
            Reset();
            return true;
        }
        hwnd = target;
        time = now;
        x = cx;
        y = cy;
        return false;
    }
};

static VideoDoubleClickTracker g_videoDblClick;

static LRESULT CALLBACK VideoHostProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        // The fullscreen shell and the video host are not the app window.
        // Alt+F4 on the shell leaves fullscreen instead of destroying playback.
        if (g_pApp && hWnd == g_pApp->m_hWndFullscreen) {
            g_pApp->ExitFullscreen();
        }
        return 0;
    case WM_SIZE:
        if (g_pApp && hWnd == g_pApp->m_hWndFullscreen && g_pApp->m_hWndVideoBase) {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            MoveWindow(g_pApp->m_hWndVideoBase, 0, 0, width, height, TRUE);
            g_pApp->UpdateSyncOverlayPosition();
        } else if (g_pApp && (hWnd == g_pApp->m_hWndVideoBase || hWnd == g_pApp->m_hWndVideoOverlay)) {
            g_pApp->m_streamA.ResizeToWindow();
            g_pApp->m_streamB.ResizeToWindow();
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        g_videoDblClick.Reset();
        if (g_pApp && (hWnd == g_pApp->m_hWndVideoBase || hWnd == g_pApp->m_hWndVideoOverlay
                       || hWnd == g_pApp->m_hWndFullscreen)) {
            g_pApp->ToggleFullscreen();
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (g_pApp && (hWnd == g_pApp->m_hWndVideoBase || hWnd == g_pApp->m_hWndVideoOverlay)
            && g_videoDblClick.OnButtonDown(hWnd, lParam)) {
            g_pApp->ToggleFullscreen();
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        // UpdateVideo can itself generate WM_PAINT. Guard against re-entry.
        static int refreshing = 0;
        if (g_pApp && !refreshing && hWnd != g_pApp->m_hWndFullscreen) {
            refreshing++;
            g_pApp->m_streamA.RefreshVideo();
            g_pApp->m_streamB.RefreshVideo();
            refreshing--;
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        wchar_t buf[32];
        swprintf(buf, 32, L"%.1f", g_pApp->m_settings.crossfadeDuration);
        SetDlgItemText(hDlg, IDC_EDIT_CROSSFADE, buf);
        swprintf(buf, 32, L"%.1f", g_pApp->m_settings.pauseFadeDuration);
        SetDlgItemText(hDlg, IDC_EDIT_FADEPAUSE, buf);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            wchar_t buf1[32], buf2[32];
            GetDlgItemText(hDlg, IDC_EDIT_CROSSFADE, buf1, 32);
            GetDlgItemText(hDlg, IDC_EDIT_FADEPAUSE, buf2, 32);

            double cf = _wtof(buf1);
            double fp = _wtof(buf2);

            if (cf >= 0.5 && cf <= 20.0) g_pApp->m_settings.crossfadeDuration = cf;
            if (fp >= 0.1 && fp <= 10.0) g_pApp->m_settings.pauseFadeDuration = fp;

            EndDialog(hDlg, IDOK);
            return TRUE;
        } else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowSettingsDialog(HWND hWndParent, HINSTANCE hInstance) {
    // Construct in-memory dialog template compatible with WinXP through Win10
    #pragma pack(push, 4)
    struct DLGTEMPLATEEX {
        WORD dlgVer;
        WORD signature;
        DWORD helpID;
        DWORD exStyle;
        DWORD style;
        WORD cDlgItems;
        short x;
        short y;
        short cx;
        short cy;
    };
    #pragma pack(pop)

    // Fallback: Create simple dynamic modal dialog window
    HWND hDlg = CreateWindowEx(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"#32770", L"Settings - CrossFade Player",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        150, 150, 360, 210,
        hWndParent, NULL, hInstance, NULL
    );

    CreateWindow(L"STATIC", L"Crossfade Duration (seconds):", WS_CHILD | WS_VISIBLE,
        20, 20, 220, 20, hDlg, NULL, hInstance, NULL);

    HWND hEd1 = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        240, 18, 80, 22, hDlg, (HMENU)IDC_EDIT_CROSSFADE, hInstance, NULL);

    CreateWindow(L"STATIC", L"Pause Fade Duration (seconds):", WS_CHILD | WS_VISIBLE,
        20, 60, 220, 20, hDlg, NULL, hInstance, NULL);

    HWND hEd2 = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        240, 58, 80, 22, hDlg, (HMENU)IDC_EDIT_FADEPAUSE, hInstance, NULL);

    HWND hBtnOk = CreateWindow(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        150, 120, 80, 28, hDlg, (HMENU)IDOK, hInstance, NULL);

    HWND hBtnCan = CreateWindow(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        240, 120, 80, 28, hDlg, (HMENU)IDCANCEL, hInstance, NULL);

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    EnumChildWindows(hDlg, [](HWND hW, LPARAM lP) -> BOOL {
        SendMessage(hW, WM_SETFONT, lP, TRUE);
        return TRUE;
    }, (LPARAM)hFont);

    wchar_t b1[32], b2[32];
    swprintf(b1, 32, L"%.1f", g_pApp->m_settings.crossfadeDuration);
    swprintf(b2, 32, L"%.1f", g_pApp->m_settings.pauseFadeDuration);
    SetWindowText(hEd1, b1);
    SetWindowText(hEd2, b2);

    // Center dialog
    RECT rcP, rcD;
    GetWindowRect(hWndParent, &rcP);
    GetWindowRect(hDlg, &rcD);
    int dW = rcD.right - rcD.left;
    int dH = rcD.bottom - rcD.top;
    int x = rcP.left + ((rcP.right - rcP.left) - dW) / 2;
    int y = rcP.top + ((rcP.bottom - rcP.top) - dH) / 2;
    SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(hWndParent, FALSE);
    ShowWindow(hDlg, SW_SHOW);

    MSG msg;
    BOOL bRunning = TRUE;
    while (bRunning && GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_COMMAND) {
            if (LOWORD(msg.wParam) == IDOK) {
                GetWindowText(hEd1, b1, 32);
                GetWindowText(hEd2, b2, 32);
                double cf = _wtof(b1);
                double fp = _wtof(b2);
                if (cf >= 0.5 && cf <= 30.0) g_pApp->m_settings.crossfadeDuration = cf;
                if (fp >= 0.1 && fp <= 10.0) g_pApp->m_settings.pauseFadeDuration = fp;
                bRunning = FALSE;
            } else if (LOWORD(msg.wParam) == IDCANCEL) {
                bRunning = FALSE;
            }
        }
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    EnableWindow(hWndParent, TRUE);
    DestroyWindow(hDlg);
    SetActiveWindow(hWndParent);
}

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_SIZE:
        if (g_pApp) g_pApp->OnResize();
        return 0;

    case WM_MOVE:
        if (g_pApp) g_pApp->UpdateSyncOverlayPosition();
        return 0;

    case WM_DROPFILES:
        if (g_pApp) g_pApp->HandleDropFiles((HDROP)wParam);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_SPACE) {
            if (g_pApp) g_pApp->TogglePlayPause();
            return 0;
        }
        break;

    case WM_HSCROLL: {
        HWND hScroll = (HWND)lParam;
        if (hScroll == g_pApp->m_hSeekBar) {
            int request = LOWORD(wParam);
            if (request == TB_THUMBTRACK || request == TB_THUMBPOSITION) {
                g_pApp->m_userIsDraggingSeek = true;
            } else if (request == TB_ENDTRACK) {
                g_pApp->m_userIsDraggingSeek = false;
                int pos = (int)SendMessage(g_pApp->m_hSeekBar, TBM_GETPOS, 0, 0);
                DirectShowStream* pActive = g_pApp->GetActiveStream();
                double dur = pActive->GetDuration();
                if (dur > 0.0) {
                    pActive->SetPosition((double)pos / 1000.0 * dur);
                }
            }
        } else if (hScroll == g_pApp->m_hVolBar) {
            int vol = (int)SendMessage(g_pApp->m_hVolBar, TBM_GETPOS, 0, 0);
            g_pApp->m_settings.masterVolume = vol;
            if (g_pApp->m_state == STATE_PLAYING && !g_pApp->m_isCrossfading) {
                g_pApp->GetActiveStream()->SetVolume((double)vol / 100.0);
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int notif = HIWORD(wParam);

        switch (id) {
        case IDC_BTN_PLAY:
            g_pApp->TogglePlayPause();
            break;
        case IDC_BTN_PAUSE:
            g_pApp->StartAudioPauseFade();
            break;
        case IDC_BTN_STOP:
            g_pApp->StopPlayback();
            break;
        case IDC_BTN_PREV:
            g_pApp->PrevTrack();
            break;
        case IDC_BTN_NEXT:
            g_pApp->NextTrack();
            break;
        case IDC_BTN_FULLSCREEN:
            g_pApp->ToggleFullscreen();
            break;
        case IDC_BTN_ADD: {
            wchar_t szFile[MAX_PATH] = { 0 };
            OPENFILENAME ofn = { 0 };
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"Video & Audio Files\0*.mp4;*.avi;*.mkv;*.wmv;*.mov;*.mpg;*.mp3;*.wav\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileName(&ofn)) {
                g_pApp->AddFileToPlaylist(szFile);
            }
            break;
        }
        case IDC_BTN_REMOVE:
            g_pApp->RemoveSelectedTrack();
            break;
        case IDC_BTN_CLEAR:
            g_pApp->ClearPlaylist();
            break;
        case IDC_BTN_OPEN_PL:
            g_pApp->OpenPlaylist();
            break;
        case IDC_BTN_SAVE_PL:
            g_pApp->SavePlaylist();
            break;
        case IDC_BTN_SETTINGS:
            ShowSettingsDialog(hWnd, (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE));
            break;
        case IDC_PLAYLIST:
            if (notif == LBN_DBLCLK) {
                int sel = (int)SendMessage(g_pApp->m_hPlaylist, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    g_pApp->PlayTrackIndex(sel);
                }
            }
            break;
        }
        return 0;
    }

    case WM_PLAYBACK_ENDED: {
        DirectShowStream* pEnded = (DirectShowStream*)lParam;
        if (g_pApp && pEnded == g_pApp->GetActiveStream() && !g_pApp->m_isCrossfading) {
            g_pApp->NextTrack();
        }
        return 0;
    }

    case WM_DS_EVENT: {
        // DirectShow Media Event notification
        DirectShowStream* pStream = (DirectShowStream*)lParam;
        if (pStream && pStream->pGraph) {
            IMediaEvent* pEv = NULL;
            if (SUCCEEDED(pStream->pGraph->QueryInterface(IID_IMediaEvent, (void**)&pEv)) && pEv) {
                long evCode;
                LONG_PTR param1, param2;
                while (SUCCEEDED(pEv->GetEvent(&evCode, &param1, &param2, 0))) {
                    pEv->FreeEventParams(evCode, param1, param2);
                    if (evCode == EC_COMPLETE) {
                        if (pStream == g_pApp->GetActiveStream() && !g_pApp->m_isCrossfading) {
                            g_pApp->NextTrack();
                        }
                    }
                }
                pEv->Release();
            }
        }
        return 0;
    }

    case WM_CLOSE:
        if (g_pApp) g_pApp->ExitFullscreen();
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // Initialize COM for DirectShow on apartment thread
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return -1;

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // Register Main Window Class
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"CrossFadePlayerWndClass";
    RegisterClassEx(&wc);

    HWND hWnd = CreateWindowEx(
        WS_EX_ACCEPTFILES,
        L"CrossFadePlayerWndClass",
        L"CrossFade Video Player (XP / Vista / 7 / 8 / 10)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 560,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) {
        CoUninitialize();
        return -1;
    }

    g_pApp = new VideoPlayerApp();
    g_pApp->CreateControls(hWnd, hInstance);

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    int argCount = 0;
    LPWSTR* argList = CommandLineToArgvW(GetCommandLineW(), &argCount);
    if (argList) {
        for (int i = 1; i < argCount; ++i) {
            if (argList[i] && argList[i][0] != L'\0') {
                g_pApp->AddFileToPlaylist(argList[i]);
            }
        }
        LocalFree(argList);
        if (!g_pApp->m_playlistFiles.empty()) {
            g_pApp->PlayTrackIndex(0);
        }
    }

    // High frequency clock tick timer for smooth 60fps crossfade and audio ramping
    SetTimer(hWnd, TIMER_ENGINE_TICK, 16, NULL);

    DWORD lastTick = GetTickCount();

    // Standard message pump with Spacebar accelerator hook
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TIMER && msg.wParam == TIMER_ENGINE_TICK) {
            DWORD currentTick = GetTickCount();
            DWORD deltaMs = currentTick - lastTick;
            lastTick = currentTick;
            if (deltaMs > 200) deltaMs = 200; // Cap large spikes
            g_pApp->OnTimerTick((double)deltaMs / 1000.0);
        }

        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            if (g_pApp && g_pApp->m_isFullscreen) {
                g_pApp->ExitFullscreen();
                continue;
            }
        }

        // Intercept Spacebar globally when focus is on non-edit controls
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_SPACE) {
            HWND hFocus = GetFocus();
            wchar_t clsName[32];
            GetClassName(hFocus, clsName, 32);
            if (_wcsicmp(clsName, L"Edit") != 0) {
                g_pApp->TogglePlayPause();
                continue;
            }
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    KillTimer(hWnd, TIMER_ENGINE_TICK);
    delete g_pApp;
    g_pApp = NULL;

    CoUninitialize();
    return (int)msg.wParam;
}