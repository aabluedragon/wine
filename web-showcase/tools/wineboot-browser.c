/*
 * Browser-only Wine bootstrap.
 *
 * Wine's Unix launcher starts wineboot before the first Windows process.  The
 * full service/device bootstrap cannot complete in BoxedWine's browser guest,
 * but applications still require its per-session completion event.  Signal
 * that event and exit; the packed prefix already contains the static registry
 * and drive setup.
 */
#include <windows.h>
#include <winternl.h>

/* Keep this executable NTDLL-only.  Importing Kernel32 invokes its process
 * attach handler before our entry point, which scans every timezone key and
 * prevents the launcher from observing the boot event for minutes. */
NTSYSAPI NTSTATUS NTAPI NtCreateEvent(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *, int, BOOLEAN);
NTSYSAPI NTSTATUS NTAPI NtOpenSection(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *);
NTSYSAPI NTSTATUS NTAPI NtMapViewOfSection(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T,
                                           PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG);
NTSYSAPI NTSTATUS NTAPI NtUnmapViewOfSection(HANDLE, PVOID);
NTSYSAPI NTSTATUS NTAPI NtSetEvent(HANDLE, LONG *);
NTSYSAPI NTSTATUS NTAPI NtClose(HANDLE);
NTSYSAPI NTSTATUS NTAPI NtTerminateProcess(HANDLE, NTSTATUS);

static void copy_wstr(WCHAR *dst, const WCHAR *src)
{
    while ((*dst++ = *src++));
}

void mainCRTStartup(void)
{
    static const WCHAR event_name[] = L"\\KernelObjects\\__wineboot_event";
    static const WCHAR shared_data_name[] = L"\\KernelObjects\\__wine_user_shared_data";
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    HANDLE event, section;
    BYTE *shared;
    SIZE_T view_size = 0x738;

    /* This named section is created by wineserver.  Wine's full wineboot
       populates it before user32 begins querying display/system settings. */
    RtlInitUnicodeString(&name, shared_data_name);
    InitializeObjectAttributes(&attributes, &name, 0, NULL, NULL);
    if (!NtOpenSection(&section, 0xf001f, &attributes))
    {
        shared = NULL;
        if (!NtMapViewOfSection(section, (HANDLE)-1, (PVOID *)&shared, 0, 0,
                                NULL, &view_size, 2 /* ViewUnmap */, 0, PAGE_READWRITE))
        {
            *(DWORD *)(shared + 0x260) = 19045; /* NtBuildNumber */
            *(BYTE  *)(shared + 0x268) = TRUE;  /* ProductTypeIsValid */
            *(DWORD *)(shared + 0x26c) = 10;    /* NtMajorVersion */
            *(DWORD *)(shared + 0x270) = 0;     /* NtMinorVersion */
            copy_wstr((WCHAR *)(shared + 0x030), L"C:\\windows");
            NtUnmapViewOfSection((HANDLE)-1, shared);
        }
        NtClose(section);
    }

    RtlInitUnicodeString(&name, event_name);
    InitializeObjectAttributes(&attributes, &name, 0, NULL, NULL);
    if (!NtCreateEvent(&event, EVENT_MODIFY_STATE, &attributes, 0, FALSE))
    {
        NtSetEvent(event, NULL);
        NtClose(event);
    }
    NtTerminateProcess((HANDLE)-1, 0);
}
