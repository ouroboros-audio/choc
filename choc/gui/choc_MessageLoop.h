//
//    ██████ ██   ██  ██████   ██████
//   ██      ██   ██ ██    ██ ██            ** Classy Header-Only Classes **
//   ██      ███████ ██    ██ ██
//   ██      ██   ██ ██    ██ ██           https://github.com/Tracktion/choc
//    ██████ ██   ██  ██████   ██████
//
//   CHOC is (C)2022 Tracktion Corporation, and is offered under the terms of the ISC license:
//
//   Permission to use, copy, modify, and/or distribute this software for any purpose with or
//   without fee is hereby granted, provided that the above copyright notice and this permission
//   notice appear in all copies. THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
//   WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
//   AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
//   CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
//   WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
//   CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#ifndef CHOC_MESSAGELOOP_HEADER_INCLUDED
#define CHOC_MESSAGELOOP_HEADER_INCLUDED

#include <memory>
#include <string>
#include <functional>
#include <mutex>
#include <chrono>
#include <atomic>
#include <unordered_map>
#include <stdexcept>
#include "../platform/choc_Platform.h"
#include "../platform/choc_Assert.h"


//==============================================================================
/**
    This namespace provides some bare-minimum event loop and message dispatch
    functions.

    Note that on Linux this uses GTK, so to build it you'll need to:
       1. Install the libgtk-3-dev package.
       2. Link the gtk+3.0 library in your build.
          You might want to have a look inside choc/tests/CMakeLists.txt for
          an example of how to add this packages to your build without too
          much fuss.
*/
namespace choc::messageloop
{
    /// On some platforms (yep, I mean Windows), it's only possible to send
    /// messages after some initialisation has been run on the message thread
    /// itself.
    /// If you call choc::messageloop::run(), then that will automatically
    /// set things up correctly and you don't need to call this function.
    /// But if you're running in a program where there's a 3rd-party event
    /// loop, you'll need to manually call choc::messageloop::initialise() on
    /// your application's message thread at the start of your program to make
    /// sure that any threaded calls to postMessage() work correctly.
    void initialise();

    /// Releases this module's native message-loop resources on their owner thread.
    /// On Windows this stops timers and discards queued callbacks before closing
    /// the message window. Further posts are discarded until initialise() is called.
    /// Returns false if called from a different thread. Other platforms have no
    /// module-owned native window to release.
    bool shutdown();

    /// Synchronously runs the system message loop.
    void run();

    /// Posts a message to make the message loop exit and terminate the app.
    void stop();

    /// Posts a function be invoked asynchronously by the message thread.
    ///
    /// Tip: if you might need to cancel the callback after posting it,
    /// you could use a `choc::threading::ThreadSafeFunctor` to wrap your
    /// target function, which lets you safely nullify it.
    void postMessage (std::function<void()>&&);

    /// Returns true if the current thread is the message thread.
    bool callerIsOnMessageThread();

    //==============================================================================
    /// Manages a periodic timer whose callbacks happen on the message loop.
    ///
    /// You can create a Timer with a callback function and interval, and the
    /// function will be repeatedly called (on the message thread) until the Timer
    /// is deleted, or clear() is called, or the callback function returns false.
    ///
    struct Timer
    {
        Timer() = default;
        Timer (const Timer&) = delete;
        Timer (Timer&&) = default;
        Timer& operator= (Timer&&) = default;
        ~Timer() = default;

        /// The callback function should return true to keep the
        /// timer running, or false to stop it.
        using Callback = std::function<bool()>;

        /// Creates and starts a Timer for a particular interval (in
        /// milliseconds) and a callback.
        /// The callback will continue to be called at this interval
        /// until either the Timer object is deleted, or clear() is
        /// called, or the callback returns `false`.
        Timer (uint32_t intervalMillisecs,
               Callback&& callbackFunction);

        /// Stops and clears the timer. (You can also clear a Timer
        /// by assigning an empty Timer to it).
        void clear();

        /// Returns true if the Timer has been initialised with
        /// a callback, or false if it's just an empty object.
        operator bool() const           { return pimpl != nullptr; }

    private:
        struct Pimpl;
        std::unique_ptr<Pimpl> pimpl;
    };

    //==============================================================================
    /// Triggers a one-shot timer callback of a given lambda function after a given
    /// interval.
    /// This uses the Timer class, but saves you needing to keep a Timer object alive,
    /// if you just need a fire-and-forget event.
    /// The callback function should just be a void lambda.
    template <typename Callback>
    void setTimeout (uint32_t intervalMillisecs, Callback&& callbackFunction);

}


//==============================================================================
//        _        _           _  _
//     __| |  ___ | |_   __ _ (_)| | ___
//    / _` | / _ \| __| / _` || || |/ __|
//   | (_| ||  __/| |_ | (_| || || |\__ \ _  _  _
//    \__,_| \___| \__| \__,_||_||_||___/(_)(_)(_)
//
//   Code beyond this point is implementation detail...
//
//==============================================================================



#if CHOC_LINUX

#include <thread>
#include "../platform/choc_DisableAllWarnings.h"
#include <gtk/gtk.h>
#include "../platform/choc_ReenableAllWarnings.h"

namespace choc::messageloop
{

inline std::thread::id& getMainThreadIDRef()
{
    static std::thread::id i;
    return i;
}

inline void initialise()
{
    getMainThreadIDRef() = std::this_thread::get_id();
}

inline bool shutdown() { return true; }

inline void run()
{
    initialise();
    gtk_main();
}

inline void stop()
{
    gtk_main_quit();
}

inline void postMessage (std::function<void()>&& fn)
{
    g_idle_add_full (G_PRIORITY_HIGH_IDLE,
                     (GSourceFunc) ([](void* f) -> int
                     {
                         (*static_cast<std::function<void()>*>(f))();
                         return G_SOURCE_REMOVE;
                     }),
                     new std::function<void()> (std::move (fn)),
                     [] (void* f) { delete static_cast<std::function<void()>*>(f); });
}

inline bool callerIsOnMessageThread()
{
    return getMainThreadIDRef() == std::this_thread::get_id();
}

struct Timer::Pimpl
{
    Pimpl (Callback&& c, uint32_t interval)
    {
        sharedState = std::make_shared<SharedState>();
        sharedState->callback = std::move (c);
        handle = g_timeout_add (interval, staticCallback, this);
    }

    ~Pimpl()
    {
        if (sharedState->isInCallback)
            sharedState->isRunning = false;
        else if (sharedState->isRunning)
            g_source_remove (handle);
    }

    static gboolean staticCallback (gpointer context)
    {
        auto state = static_cast<Pimpl*> (context)->sharedState; // keep a local shared_ptr
        return state->handleCallback();
    }

    struct SharedState  : public std::enable_shared_from_this<SharedState>
    {
        Callback callback;
        bool isInCallback = false, isRunning = true;

        bool handleCallback()
        {
            isInCallback = true;
            bool result = callback();
            isInCallback = false;

            if (! result)
                isRunning = false;

            return isRunning;
        }
    };

    std::shared_ptr<SharedState> sharedState;
    guint handle;
};

//==============================================================================
#elif CHOC_APPLE

#include <thread>
#include <unordered_set>
#include <dispatch/dispatch.h>
#include <type_traits>
#include "../platform/choc_ObjectiveCHelpers.h"

namespace choc::messageloop
{

inline std::thread::id& getMainThreadIDRef()
{
    static std::thread::id i;
    return i;
}

inline void initialise()
{
    getMainThreadIDRef() = std::this_thread::get_id();
}

inline bool shutdown() { return true; }

inline void run()
{
    CHOC_AUTORELEASE_BEGIN
    initialise();
    objc::call<void> (objc::getSharedNSApplication(), "run");
    CHOC_AUTORELEASE_END
}

inline void stop()
{
    postMessage ([]
    {
        using namespace choc::objc;
        static constexpr long NSEventTypeApplicationDefined = 15;

        CHOC_AUTORELEASE_BEGIN
        call<void> (getSharedNSApplication(), "stop:", (id) nullptr);

        // After sending the stop message, we need to post a dummy event to
        // kick the message loop, otherwise it can just sit there and hang
        struct NSPoint { double x = 0, y = 0; };
        id dummyEvent = callClass<id> ("NSEvent", "otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:",
                                       NSEventTypeApplicationDefined, NSPoint(), 0, 0, 0, nullptr, (short) 0, 0, 0);
        call<void> (getSharedNSApplication(), "postEvent:atStart:", dummyEvent, YES);
        CHOC_AUTORELEASE_END
    });
}

inline void postMessage (std::function<void()>&& fn)
{
    dispatch_async_f (dispatch_get_main_queue(),
                      new std::function<void()> (std::move (fn)),
                      (dispatch_function_t) (+[](void* arg)
                      {
                          CHOC_AUTORELEASE_BEGIN
                          std::unique_ptr<std::function<void()>> f (static_cast<std::function<void()>*> (arg));
                          (*f)();
                          CHOC_AUTORELEASE_END
                      }));
}

inline bool callerIsOnMessageThread()
{
    return getMainThreadIDRef() == std::this_thread::get_id();
}

struct Timer::Pimpl
{
    Pimpl (Callback&& c, uint32_t i) : callback (std::move (c)), interval (i)
    {
        getList().add (this);
        dispatch();
    }

    ~Pimpl()
    {
        getList().remove (this);
    }

    static void staticCallback (void* context)
    {
        if (getList().invokeIfStillAlive (static_cast<Pimpl*> (context)))
        {
            CHOC_AUTORELEASE_BEGIN
            static_cast<Pimpl*> (context)->dispatch();
            CHOC_AUTORELEASE_END
        }
    }

    void dispatch()
    {
        dispatch_after_f (dispatch_time (DISPATCH_TIME_NOW, interval * 1000000),
                          dispatch_get_main_queue(), this, staticCallback);
    }

    Callback callback;
    const int64_t interval;

    struct TimerList
    {
        std::recursive_mutex lock;
        std::unordered_set<Pimpl*> timers;

        bool invokeIfStillAlive (Pimpl* p)
        {
            std::scoped_lock l (lock);

            // must check before AND after the call because the Pimpl
            // may be deleted during the callback
            return timers.find (p) != timers.end()
                    && p->callback()
                    && timers.find (p) != timers.end();
        }

        void add (Pimpl* p)
        {
            std::scoped_lock l (lock);
            timers.insert (p);
        }

        void remove (Pimpl* p)
        {
            std::scoped_lock l (lock);
            timers.erase (p);
        }
    };

    static TimerList& getList()
    {
        static TimerList list;
        return list;
    }
};

//==============================================================================
#elif CHOC_WINDOWS

#undef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#undef  NOMINMAX
#define NOMINMAX
#define Rectangle Rectangle_renamed_to_avoid_name_collisions
#include <windows.h>
#undef Rectangle

namespace choc::messageloop
{

struct MessageWindow
{
    MessageWindow()
    {
        className = "choc_" + std::to_string (reinterpret_cast<uintptr_t> (this));

        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof (wc);
        GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR> (&windowProc), &module);
        wc.hInstance = module;
        wc.lpszClassName = className.c_str();
        wc.lpfnWndProc = windowProc;

        RegisterClassExA (std::addressof (wc));

        hwnd = CreateWindowA (className.c_str(), "choc", 0, 0, 0, 0, 0,
                              nullptr, nullptr, wc.hInstance, nullptr);
    }

    ~MessageWindow()
    {
        if (hwnd != nullptr)
        {
            MSG message {};
            while (PeekMessageA (&message, hwnd, WM_APP, WM_APP, PM_REMOVE))
                if (message.wParam == magicWParam)
                    delete reinterpret_cast<std::function<void()>*> (message.lParam);
            while (PeekMessageA (&message, hwnd, WM_TIMER, WM_TIMER, PM_REMOVE)) {}
            DestroyWindow (hwnd);
        }
        UnregisterClassA (className.c_str(), module);
    }

    static LRESULT CALLBACK windowProc (HWND h, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_APP && wParam == magicWParam)
        {
            std::unique_ptr<std::function<void()>> f (reinterpret_cast<std::function<void()>*> (lParam));
            (*f)();
        }

        return DefWindowProc (h, message, wParam, lParam);
    }

    static inline constexpr WPARAM magicWParam = 0xc40cc40c;

    HMODULE module = nullptr;
    HWND hwnd = nullptr;
    std::string className;
    DWORD threadID = GetCurrentThreadId();
};

struct LockedMessageWindow
{
    MessageWindow& window;
    std::unique_lock<std::mutex> lock;
};

struct NativeTimerHandle
{
    HWND window = nullptr;
    std::atomic<UINT_PTR> id { 0 };
    std::weak_ptr<void> callbackState;
};

struct SharedMessageWindow
{
    ~SharedMessageWindow()
    {
        // Explicit shutdown is required before unloading a DLL. This fallback
        // keeps the state alive while an application's queued owners are freed.
        accepting = false;
        closing = true;
        for (auto& timer : timers)
        {
            KillTimer (timer.second->window, timer.second->id);
            timer.second->id = 0;
            timer.second->window = nullptr;
        }
        timers.clear();
        window.reset();
    }

    std::unique_ptr<MessageWindow> window;
    std::mutex mutex;
    bool accepting = true;
    bool closing = false;
    UINT_PTR nextTimerID = 1;
    std::unordered_map<UINT_PTR, std::shared_ptr<NativeTimerHandle>> timers;
};

inline SharedMessageWindow& getMessageWindowState()
{
    static SharedMessageWindow shared;
    return shared;
}

inline LockedMessageWindow getSharedMessageWindow (bool recreateIfWrongThread = false)
{
    auto& shared = getMessageWindowState();
    std::unique_lock<std::mutex> l (shared.mutex);
    if (shared.closing || ! shared.accepting)
        throw std::runtime_error ("The message loop is closed");
    auto& window = shared.window;

    if (window == nullptr || (recreateIfWrongThread && window->threadID != GetCurrentThreadId()))
        window = std::make_unique<MessageWindow>();

    return LockedMessageWindow { *window, std::move (l) };
}

inline void initialise()
{
    {
        auto& shared = getMessageWindowState();
        std::lock_guard<std::mutex> lock (shared.mutex);
        if (shared.closing)
            return;
        shared.accepting = true;
    }
    getSharedMessageWindow (true);
}

inline bool shutdown()
{
    auto& shared = getMessageWindowState();
    std::unique_ptr<MessageWindow> closing;
    {
        std::lock_guard<std::mutex> lock (shared.mutex);
        if (shared.closing)
            return true;
        if (shared.window != nullptr && shared.window->threadID != GetCurrentThreadId())
            return false;
        shared.accepting = false;
        shared.closing = true;
        for (auto& timer : shared.timers)
        {
            KillTimer (timer.second->window, timer.second->id);
            timer.second->id = 0;
            timer.second->window = nullptr;
        }
        shared.timers.clear();
        closing = std::move (shared.window);
    }
    // Callback destructors may call back into the message loop.
    closing.reset();
    {
        std::lock_guard<std::mutex> lock (shared.mutex);
        shared.closing = false;
    }
    return true;
}

inline void run()
{
    initialise();

    for (;;)
    {
        MSG msg;

        if (GetMessage (std::addressof (msg), nullptr, 0, 0) == -1)
            break;

        if (msg.message == WM_QUIT)
            break;

        if (msg.hwnd)
        {
            TranslateMessage (std::addressof (msg));
            DispatchMessage (std::addressof (msg));
        }
    }
}

inline void stop()
{
    postMessage ([] { PostQuitMessage (0); });
}

inline void postMessage (std::function<void()>&& fn)
{
    auto callback = std::make_unique<std::function<void()>> (std::move (fn));
    {
        auto& shared = getMessageWindowState();
        std::lock_guard<std::mutex> lock (shared.mutex);
        if (shared.accepting && ! shared.closing)
        {
            if (shared.window == nullptr)
                shared.window = std::make_unique<MessageWindow>();
            if (PostMessageA (shared.window->hwnd, WM_APP, MessageWindow::magicWParam,
                              reinterpret_cast<LPARAM> (callback.get())))
                callback.release();
        }
    }
}

inline bool callerIsOnMessageThread()
{
    auto& shared = getMessageWindowState();
    std::lock_guard<std::mutex> lock (shared.mutex);
    return shared.window != nullptr && shared.window->threadID == GetCurrentThreadId();
}

struct Timer::Pimpl
{
    Pimpl (Callback&& c, uint32_t interval)
    {
        sharedState = std::make_shared<SharedState>();
        sharedState->callback = std::move (c);

        auto& shared = getMessageWindowState();
        std::lock_guard<std::mutex> lock (shared.mutex);
        if (shared.accepting && ! shared.closing)
        {
            if (shared.window == nullptr)
                shared.window = std::make_unique<MessageWindow>();
            auto handle = std::make_shared<NativeTimerHandle>();
            handle->window = shared.window->hwnd;
            handle->callbackState = sharedState;
            sharedState->timer = handle;
            handle->id = SetTimer (handle->window, shared.nextTimerID++, interval, staticCallback);
            if (handle->id != 0)
            {
                const auto id = handle->id.load();
                shared.timers.emplace (id, std::move (handle));
            }
        }
    }

    ~Pimpl() { sharedState->killTimer(); }

    static void CALLBACK staticCallback (HWND, UINT, UINT_PTR p, DWORD) noexcept
    {
        std::shared_ptr<void> state;
        {
            auto& shared = getMessageWindowState();
            std::lock_guard<std::mutex> lock (shared.mutex);
            if (auto timer = shared.timers.find (p); timer != shared.timers.end())
                state = timer->second->callbackState.lock();
        }
        if (state != nullptr)
            std::static_pointer_cast<SharedState> (state)->handleCallback();
    }

    struct SharedState  : public std::enable_shared_from_this<SharedState>
    {
        ~SharedState()
        {
            killTimer();
        }

        void killTimer()
        {
            if (timer != nullptr && timer->id.load() != 0)
            {
                auto& shared = getMessageWindowState();
                std::lock_guard<std::mutex> lock (shared.mutex);
                if (timer->id != 0)
                {
                    KillTimer (timer->window, timer->id);
                    shared.timers.erase (timer->id);
                    timer->id = 0;
                }
            }
        }

        void handleCallback()
        {
            if (! callback())
                killTimer();
        }

        Callback callback;
        std::shared_ptr<NativeTimerHandle> timer;
    };

    std::shared_ptr<SharedState> sharedState;
};

#else
 #error "choc::messageloop only supports OSX, Windows or Linux!"
#endif

inline Timer::Timer (uint32_t interval, Callback&& cb)
{
    CHOC_ASSERT (cb != nullptr); // The callback must be a valid function!
    pimpl = std::make_unique<Pimpl> (std::move (cb), interval);
}

template <typename Callback>
void setTimeout (uint32_t intervalMillisecs, Callback&& callback)
{
    auto t = new Timer();

    *t = Timer (intervalMillisecs, [t, c = std::move (callback)]
    {
        c();
        postMessage ([t] { delete t; });
        return false;
    });
}

inline void Timer::clear()   { pimpl.reset(); }

} // namespace choc::messageloop


#endif // CHOC_MESSAGELOOP_HEADER_INCLUDED
