/* Browser-only services.exe for the prebuilt Wine prefix.
 * Wineboot waits only for this named readiness event.  The browser guest does
 * not support the hardware services normally hosted by services.exe. */
#include <windows.h>

void mainCRTStartup(void)
{
    static const WCHAR event_name[] = L"__wine_SvcctlStarted";
    HANDLE event;

    event = CreateEventW(NULL, TRUE, FALSE, event_name);
    if (event)
    {
        SetEvent(event);
    }
    /* Keep the process alive while wineboot observes the readiness event. */
    Sleep(60000);
    ExitProcess(0);
}
