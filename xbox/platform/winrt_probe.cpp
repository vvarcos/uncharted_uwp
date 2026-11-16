// Include-only translation unit: pulls the same C++/WinRT headers the application uses, with no
// code, so a DLL load failure isolates header-driven static initializers from application code.
#include <windows.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

namespace {
int winrt_probe_anchor = 0;
}
