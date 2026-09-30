#include <windows.h>
#include <cstdio>

static bool testModules (const wchar_t* firstPath, const wchar_t* secondPath, bool closeFirst)
{
    auto first = LoadLibraryW (firstPath);
    auto second = LoadLibraryW (secondPath);

    if (first == nullptr || second == nullptr)
    {
        std::printf ("LoadLibrary failed: %lu\n", GetLastError());
        return false;
    }

    using OpenWindow = HWND (*)();
    using PostCounter = void (*)(int*);
    auto openFirst = reinterpret_cast<OpenWindow> (GetProcAddress (first, "openMessageWindow"));
    auto openSecond = reinterpret_cast<OpenWindow> (GetProcAddress (second, "openMessageWindow"));
    auto postFirst = reinterpret_cast<PostCounter> (GetProcAddress (first, "postCounter"));
    auto postSecond = reinterpret_cast<PostCounter> (GetProcAddress (second, "postCounter"));

    if (openFirst == nullptr || openSecond == nullptr || postFirst == nullptr || postSecond == nullptr)
        return false;

    auto firstWindow = openFirst();
    auto secondWindow = openSecond();

    if (firstWindow == nullptr || secondWindow == nullptr || firstWindow == secondWindow
        || reinterpret_cast<HMODULE> (GetClassLongPtrW (firstWindow, GCLP_HMODULE)) != first
        || reinterpret_cast<HMODULE> (GetClassLongPtrW (secondWindow, GCLP_HMODULE)) != second)
    {
        std::puts ("Message windows must have separate classes owned by their DLLs");
        return false;
    }

    if (! FreeLibrary (closeFirst ? first : second))
        return false;

    int callbacks = 0;
    (closeFirst ? postSecond : postFirst) (&callbacks);
    auto deadline = GetTickCount64() + 2000;

    while (callbacks == 0 && GetTickCount64() < deadline)
    {
        MSG message {};

        while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage (&message);
            DispatchMessageW (&message);
        }

        if (callbacks == 0)
            Sleep (1);
    }

    if (callbacks != 1 || ! FreeLibrary (closeFirst ? second : first)
        || IsWindow (firstWindow) || IsWindow (secondWindow))
    {
        std::puts ("Surviving module dispatch or window teardown failed");
        return false;
    }

    return true;
}

int wmain (int argc, wchar_t** argv)
{
    if (argc != 3 || ! testModules (argv[1], argv[2], true)
                  || ! testModules (argv[1], argv[2], false))
        return 1;

    std::puts ("Separate DLL message loops dispatch and unload in both orders");
    return 0;
}
