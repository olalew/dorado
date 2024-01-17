#include "thread_utils.h"

#include <algorithm>
#include <array>

#ifdef _WIN32
#include <Windows.h>

#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>

#endif
namespace dorado::utils {

void set_thread_name(const std::string& name) {
#if defined(_WIN32)
    // See: https://randomascii.wordpress.com/2015/10/26/thread-naming-in-windows-time-for-something-better/
    typedef HRESULT(WINAPI * SetThreadDescription)(HANDLE hThread, PCWSTR lpThreadDescription);

    // The SetThreadDescription API works even if no debugger is attached. It requires Windows 10
    // build 1607 or later. The thread name set this way will only be picked up by Visual Studio
    // 2017 version 15.6 or later. See
    // https://docs.microsoft.com/en-gb/visualstudio/debugger/how-to-set-a-thread-name-in-native-code
    auto set_thread_description_func = reinterpret_cast<SetThreadDescription>(
            ::GetProcAddress(::GetModuleHandleA("Kernel32.dll"), "SetThreadDescription"));

    if (set_thread_description_func) {
        std::array<wchar_t, 64> wide_name;
        std::size_t output_size = 0;
        mbstowcs_s(&output_size, wide_name.data(), wide_name.size(), name.data(), _TRUNCATE);
        set_thread_description_func(::GetCurrentThread(), wide_name.data());
    }

#else

    // Name is limited to 16 chars including null terminator.
    std::array<char, 16> limited_name;
    const auto name_len = std::min(std::size_t(15), name.size());
    std::copy_n(name.begin(), name_len, limited_name.begin());
    limited_name[name_len] = '\0';

#if defined(__APPLE__)
    pthread_setname_np(limited_name.data());
#else
    pthread_setname_np(pthread_self(), limited_name.data());
#endif
#endif
}

}  // namespace dorado::utils
