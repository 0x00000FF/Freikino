#pragma once

// Centralised re-export of the COM smart pointer we use everywhere. Keeping it
// in one header means no file reaches directly into <wrl/client.h>, so a
// future swap (e.g. winrt::com_ptr) is a one-header edit.

#include <Unknwn.h>
#include <wrl/client.h>

namespace freikino {

using Microsoft::WRL::ComPtr;

// Convenience: call IID_PPV_ARGS-style QueryInterface into a ComPtr.
template <class Target, class Source>
ComPtr<Target> query_interface(const ComPtr<Source>& src)
{
    ComPtr<Target> out;
    if (src) {
        // HRESULT intentionally ignored: the ComPtr is null on failure and
        // callers are expected to check. For error-propagating use cases call
        // As() directly and wrap with check_hr.
        (void)src.As(&out);
    }
    return out;
}

} // namespace freikino
