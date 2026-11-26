// Proxy definition for dinput8.dll (GTA V imports DirectInput8Create).
// Order must match dinput8.def. Regenerate with: shvi_pick_proxy --emit dinput8 <dir>
#include <cstddef>

extern const wchar_t* const kProxyDll = L"dinput8.dll";
extern const char* const kProxyExports[] = {
    "DirectInput8Create", "DllCanUnloadNow", "DllGetClassObject",
    "DllRegisterServer",  "DllUnregisterServer", "GetdfDIJoystick",
};
extern const std::size_t kProxyExportCount = sizeof(kProxyExports) / sizeof(kProxyExports[0]);
