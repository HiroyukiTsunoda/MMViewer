#pragma once
#include <windows.h>
#include <commctrl.h>

namespace mm {
// GDI+ must already be running. The caller destroys this private two-image
// list with ImageList_Destroy. Index 0 is closed; index 1 is open.
HIMAGELIST CreateFolderImages(UINT dpi, bool dark);
}
