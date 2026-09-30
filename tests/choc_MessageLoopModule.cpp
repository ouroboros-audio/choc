#include "../choc/gui/choc_MessageLoop.h"

extern "C" __declspec(dllexport) HWND openMessageWindow()
{
    choc::messageloop::initialise();
    return choc::messageloop::getSharedMessageWindow().window.hwnd;
}

extern "C" __declspec(dllexport) void postCounter (int* counter)
{
    choc::messageloop::postMessage ([counter] { ++*counter; });
}
