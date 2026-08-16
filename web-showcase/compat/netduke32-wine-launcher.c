#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/*
 * Wine-side compatibility launcher for the exact user-supplied NetDuke32
 * v1.2.1 executable. The PE file is verified and never modified.
 *
 * That executable leaves its glad_glBindSampler slot NULL on an OpenGL 2.0
 * context, but unconditionally calls glBindSampler(unit, 0) while resetting
 * renderer state. Sampler 0 means the default/unbound sampler state, which is
 * already the only sampler state available through the WebGL 1 bridge.
 *
 * Start the exact PE suspended and populate only that one process-local slot
 * with a guarded stdcall stub. The stub returns for sampler 0 and executes UD2
 * for any nonzero sampler, so a future unsupported real sampler-object path is
 * a loud failure rather than silent corruption.
 */

static const BYTE expected_sha256[32] = {
    0x54, 0x7d, 0xea, 0x93, 0xd4, 0x01, 0x14, 0xde,
    0xe7, 0x75, 0x7a, 0x04, 0x9f, 0x20, 0xe0, 0xf7,
    0x65, 0x9c, 0xbd, 0x0c, 0x22, 0x1a, 0xe9, 0xcf,
    0x42, 0x58, 0x33, 0x8e, 0x94, 0xc3, 0x38, 0x78,
};

enum {
    GLAD_BIND_SAMPLER_RVA = 0x015c6164,
    GLAD_IS_SYNC_RVA = 0x0164a770,
    GLAD_VERTEX_POINTER_RVA = 0x015ea2f4,
    GLAD_TEXCOORD_POINTER_RVA = 0x015ea2f8,
    BIND_SAMPLER_CALL_RVA = 0x00129596,
    IS_SYNC_CALL_RVA = 0x00155a6c,
    VERTEX_POINTER_CALL_RVA = 0x00139b4c,
    TEXCOORD_POINTER_CALL_RVA = 0x00139b03,
    PEB32_IMAGE_BASE_OFFSET = 8,
};

typedef LONG NTSTATUS;
typedef NTSTATUS(WINAPI *nt_query_information_process_fn)(
    HANDLE process, ULONG info_class, void *info, ULONG size, ULONG *returned);

typedef struct process_basic_information32 {
    void *reserved1;
    void *peb_base_address;
    void *reserved2[2];
    ULONG_PTR unique_process_id;
    void *reserved3;
} process_basic_information32;

static int fail(const WCHAR *message, DWORD error)
{
    WCHAR text[512];
    _snwprintf(text, sizeof(text) / sizeof(text[0]) - 1,
               L"%ls\n\nWin32 error: %lu", message, (unsigned long)error);
    text[sizeof(text) / sizeof(text[0]) - 1] = 0;
    MessageBoxW(NULL, text, L"NetDuke32 Wine compatibility launcher", MB_OK | MB_ICONERROR);
    return 1;
}

static BOOL verify_target_sha256(const WCHAR *path)
{
    BYTE buffer[64 * 1024], digest[32];
    DWORD count, digest_size = sizeof(digest);
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    BOOL ok = FALSE;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) goto done;
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) goto done;

    for (;;) {
        if (!ReadFile(file, buffer, sizeof(buffer), &count, NULL)) goto done;
        if (!count) break;
        if (!CryptHashData(hash, buffer, count, 0)) goto done;
    }
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_size, 0)) goto done;
    ok = digest_size == sizeof(expected_sha256) &&
         !memcmp(digest, expected_sha256, sizeof(expected_sha256));

done:
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static WCHAR *make_child_command_line(const WCHAR *arguments)
{
    static const WCHAR prefix[] = L"\"netduke32.exe\"";
    SIZE_T prefix_length = wcslen(prefix);
    SIZE_T argument_length = arguments ? wcslen(arguments) : 0;
    SIZE_T length = prefix_length + (argument_length ? 1 + argument_length : 0) + 1;
    WCHAR *command = HeapAlloc(GetProcessHeap(), 0, length * sizeof(*command));

    if (!command) return NULL;
    memcpy(command, prefix, prefix_length * sizeof(*command));
    if (argument_length) {
        command[prefix_length] = L' ';
        memcpy(command + prefix_length + 1, arguments,
               (argument_length + 1) * sizeof(*command));
    } else {
        command[prefix_length] = 0;
    }
    return command;
}

static BOOL verify_remote_image(HANDLE process, BYTE *image_base)
{
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS32 nt;
    SIZE_T transferred;

    if (!ReadProcessMemory(process, image_base, &dos, sizeof(dos), &transferred) ||
        transferred != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000) return FALSE;
    if (!ReadProcessMemory(process, image_base + dos.e_lfanew, &nt, sizeof(nt), &transferred) ||
        transferred != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.OptionalHeader.ImageBase != 0x00400000 ||
        nt.OptionalHeader.SizeOfImage != 0x0251d000) return FALSE;
    return TRUE;
}

static BOOL verify_indirect_call(HANDLE process, BYTE *image_base,
                                 DWORD call_rva, DWORD slot_rva)
{
    BYTE instruction[6];
    DWORD operand;
    SIZE_T transferred;

    if (!ReadProcessMemory(process, image_base + call_rva, instruction,
                           sizeof(instruction), &transferred) ||
        transferred != sizeof(instruction) || instruction[0] != 0xff ||
        instruction[1] != 0x15) return FALSE;
    memcpy(&operand, instruction + 2, sizeof(operand));
    return operand == (DWORD)(uintptr_t)(image_base + slot_rva);
}

static BOOL install_guarded_slot(HANDLE process, BYTE *image_base, DWORD slot_rva,
                                 const BYTE *stub, SIZE_T stub_size)
{
    void *remote_stub;
    DWORD original_slot = 0, stub_address;
    SIZE_T transferred;

    if (!ReadProcessMemory(process, image_base + slot_rva, &original_slot,
                           sizeof(original_slot), &transferred) ||
        transferred != sizeof(original_slot) || original_slot != 0) return FALSE;

    remote_stub = VirtualAllocEx(process, NULL, stub_size, MEM_RESERVE | MEM_COMMIT,
                                 PAGE_EXECUTE_READWRITE);
    if (!remote_stub) return FALSE;
    if (!WriteProcessMemory(process, remote_stub, stub, stub_size, &transferred) ||
        transferred != stub_size) return FALSE;

    stub_address = (DWORD)(uintptr_t)remote_stub;
    if (!WriteProcessMemory(process, image_base + slot_rva, &stub_address,
                            sizeof(stub_address), &transferred) ||
        transferred != sizeof(stub_address)) return FALSE;
    return FlushInstructionCache(process, remote_stub, stub_size);
}

static BOOL install_call_redirect(HANDLE process, BYTE *image_base, DWORD call_rva,
                                  DWORD slot_rva, const BYTE *stub, SIZE_T stub_size)
{
    void *remote_stub;
    BYTE instruction[6], replacement[6];
    DWORD operand, relative;
    SIZE_T transferred;

    if (!ReadProcessMemory(process, image_base + call_rva, instruction,
                           sizeof(instruction), &transferred) ||
        transferred != sizeof(instruction) || instruction[0] != 0xff ||
        instruction[1] != 0x15) return FALSE;
    memcpy(&operand, instruction + 2, sizeof(operand));
    if (operand != (DWORD)(uintptr_t)(image_base + slot_rva)) return FALSE;
    remote_stub = VirtualAllocEx(process, NULL, stub_size, MEM_RESERVE | MEM_COMMIT,
                                 PAGE_EXECUTE_READWRITE);
    if (!remote_stub) return FALSE;
    if (!WriteProcessMemory(process, remote_stub, stub, stub_size, &transferred) ||
        transferred != stub_size) return FALSE;
    relative = (DWORD)((intptr_t)(uintptr_t)remote_stub -
                       ((intptr_t)(uintptr_t)(image_base + call_rva) + 5));
    replacement[0] = 0xe8;
    memcpy(replacement + 1, &relative, sizeof(relative));
    replacement[5] = 0x90;
    if (!WriteProcessMemory(process, image_base + call_rva, replacement,
                            sizeof(replacement), &transferred) ||
        transferred != sizeof(replacement)) return FALSE;
    return FlushInstructionCache(process, image_base + call_rva, sizeof(replacement));
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, WCHAR *arguments, int show)
{
    /* cmp dword ptr [esp+8],0; jne ud2; ret 8; ud2 */
    static const BYTE sampler_zero_stub[] = {
        0x83, 0x7c, 0x24, 0x08, 0x00,
        0x75, 0x03,
        0xc2, 0x08, 0x00,
        0x0f, 0x0b,
    };
    /* cmp dword ptr [esp+4],0; jne ud2; xor eax,eax; ret 4; ud2 */
    static const BYTE null_sync_stub[] = {
        0x83, 0x7c, 0x24, 0x04, 0x00,
        0x75, 0x05,
        0x31, 0xc0,
        0xc2, 0x04, 0x00,
        0x0f, 0x0b,
    };
    /* Translate legacy array pointers to GLES2 glVertexAttribPointer. The
     * cdecl caller keeps its original four arguments; this stub only removes
     * its six temporary bridge arguments before returning. */
    static const BYTE vertex_pointer_stub[] = {
        0xff, 0x74, 0x24, 0x10, /* pointer */
        0xff, 0x74, 0x24, 0x10, /* stride */
        0x6a, 0x00,             /* normalized = false */
        0xff, 0x74, 0x24, 0x10, /* type */
        0xff, 0x74, 0x24, 0x10, /* size */
        0x6a, 0x00,             /* attribute 0 */
        0x68, 0xbe, 0x0a, 0x00, 0x00, /* glVertexAttribPointer */
        0xcd, 0x99,
        0x83, 0xc4, 0x18,
        0xc3,
    };
    static const BYTE texcoord_pointer_stub[] = {
        0xff, 0x74, 0x24, 0x10,
        0xff, 0x74, 0x24, 0x10,
        0x6a, 0x00,
        0xff, 0x74, 0x24, 0x10,
        0xff, 0x74, 0x24, 0x10,
        0x6a, 0x01,             /* attribute 1 */
        0x68, 0xbe, 0x0a, 0x00, 0x00,
        0xcd, 0x99,
        0x83, 0xc4, 0x18,
        0xc3,
    };
    STARTUPINFOW startup = { 0 };
    PROCESS_INFORMATION process = { 0 };
    process_basic_information32 basic = { 0 };
    union {
        FARPROC generic;
        nt_query_information_process_fn query;
    } query_process;
    WCHAR *command = NULL;
    void *image_base = NULL;
    SIZE_T transferred = 0;
    DWORD exit_code = 1;
    NTSTATUS status;

    (void)instance;
    (void)previous;
    (void)show;
    startup.cb = sizeof(startup);

    if (!verify_target_sha256(L"netduke32.exe"))
        return fail(L"Refusing to launch: netduke32.exe is not the exact supported SHA-256.",
                    GetLastError());

    command = make_child_command_line(arguments);
    if (!command) return fail(L"Could not allocate the child command line.", ERROR_OUTOFMEMORY);

    if (!CreateProcessW(L"netduke32.exe", command, NULL, NULL, TRUE, CREATE_SUSPENDED,
                        NULL, NULL, &startup, &process)) {
        DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, command);
        return fail(L"CreateProcessW(netduke32.exe) failed.", error);
    }
    HeapFree(GetProcessHeap(), 0, command);

    query_process.generic = GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                           "NtQueryInformationProcess");
    if (!query_process.generic) goto child_error;
    status = query_process.query(process.hProcess, 0, &basic, sizeof(basic), NULL);
    if (status < 0) goto child_error;

    if (!ReadProcessMemory(process.hProcess,
                           (BYTE *)basic.peb_base_address + PEB32_IMAGE_BASE_OFFSET,
                           &image_base, sizeof(image_base), &transferred) ||
        transferred != sizeof(image_base)) goto child_error;

    if (!verify_remote_image(process.hProcess, image_base) ||
        !verify_indirect_call(process.hProcess, image_base,
                              BIND_SAMPLER_CALL_RVA, GLAD_BIND_SAMPLER_RVA) ||
        !verify_indirect_call(process.hProcess, image_base,
                              IS_SYNC_CALL_RVA, GLAD_IS_SYNC_RVA) ||
        !verify_indirect_call(process.hProcess, image_base,
                              VERTEX_POINTER_CALL_RVA, GLAD_VERTEX_POINTER_RVA) ||
        !verify_indirect_call(process.hProcess, image_base,
                              TEXCOORD_POINTER_CALL_RVA, GLAD_TEXCOORD_POINTER_RVA) ||
        !install_guarded_slot(process.hProcess, image_base, GLAD_BIND_SAMPLER_RVA,
                              sampler_zero_stub, sizeof(sampler_zero_stub)) ||
        !install_guarded_slot(process.hProcess, image_base, GLAD_IS_SYNC_RVA,
                              null_sync_stub, sizeof(null_sync_stub)) ||
        !install_guarded_slot(process.hProcess, image_base, GLAD_VERTEX_POINTER_RVA,
                              vertex_pointer_stub, sizeof(vertex_pointer_stub)) ||
        !install_guarded_slot(process.hProcess, image_base, GLAD_TEXCOORD_POINTER_RVA,
                              texcoord_pointer_stub, sizeof(texcoord_pointer_stub)) ||
        !install_call_redirect(process.hProcess, image_base, VERTEX_POINTER_CALL_RVA,
                               GLAD_VERTEX_POINTER_RVA, vertex_pointer_stub,
                               sizeof(vertex_pointer_stub)) ||
        !install_call_redirect(process.hProcess, image_base, TEXCOORD_POINTER_CALL_RVA,
                               GLAD_TEXCOORD_POINTER_RVA, texcoord_pointer_stub,
                               sizeof(texcoord_pointer_stub)))
        goto child_error;

    OutputDebugStringA(
        "NetDuke32 Wine compatibility: installed guarded sampler/sync and translated legacy pointer slots\n");
    if (ResumeThread(process.hThread) == (DWORD)-1) goto child_error;

    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    return (int)exit_code;

child_error:
    exit_code = GetLastError();
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return fail(L"Could not install the guarded process-local OpenGL slot.", exit_code);
}
