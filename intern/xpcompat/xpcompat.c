/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_xpcompat
 *
 * Windows XP compatibility thunks for the 32-bit MinGW build.
 *
 * Blender, its libraries and the C++ runtime call Windows API functions added after XP. A PE
 * image importing even one export the system lacks doesn't load at all, so for each such function
 * this file defines both symbols the import library would provide:
 * - `__imp__Name@N`: the import table slot that `dllimport` calls go through.
 * - `_Name@N`: the stub that plain calls jump to.
 * The definitions win over the import libraries when this library is linked as a whole archive
 * ahead of them (see `platform_win32_mingw.cmake`), so the function is never imported. At run
 * time the real function is used when the system has it (looked up once with `GetProcAddress`),
 * otherwise an XP fallback with the closest behavior.
 *
 * `build_files/xp/xpcheck.c` lists the imports a build still can't resolve on the XP machine.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <commctrl.h>
#include <objbase.h>
#include <shlobj.h>
#include <string.h>

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

/**
 * Define the import slot `__imp__<name>@<bytes>` and the jump stub `_<name>@<bytes>` (stdcall
 * decoration: underscore prefix, argument bytes suffix) for `xp_<name>`.
 */
#define XP_STR_(x) #x
#define XP_STR(x) XP_STR_(x)
#define XP_EXPORT(name, bytes) \
  void *xp_imp_##name __asm__("__imp__" #name "@" XP_STR(bytes)) = (void *)&xp_##name; \
  __asm__(".globl _" #name "@" XP_STR(bytes) "\n" \
          "_" #name "@" XP_STR(bytes) ":\n" \
          "  jmp *__imp__" #name "@" XP_STR(bytes) "\n");

/** The real export when this Windows version has it, otherwise null. Not cached on failure,
 * callers cache the result themselves. */
static FARPROC xp_lookup(const char *dll, const char *name)
{
  HMODULE module = GetModuleHandleA(dll);
  if (!module) {
    module = LoadLibraryA(dll);
  }
  return module ? GetProcAddress(module, name) : NULL;
}

/** Resolve once: `real` is a `static` function pointer, `(void *)1` marks "not looked up yet". */
#define XP_RESOLVE(real, dll, name) \
  if ((void *)(real) == (void *)1) { \
    *(FARPROC *)&(real) = xp_lookup(dll, name); \
  } \
  ((void)0)

/** \} */

/* -------------------------------------------------------------------- */
/** \name kernel32.dll
 * \{ */

/* Vista+. */
static ULONGLONG WINAPI xp_GetTickCount64(void)
{
  static ULONGLONG(WINAPI * real)(void) = (ULONGLONG(WINAPI *)(void))1;
  XP_RESOLVE(real, "kernel32.dll", "GetTickCount64");
  if (real) {
    return real();
  }
  /* Extend the 32-bit tick count, which wraps every 49.7 days. Callers poll far more often than
   * that, so tracking the wrap from the last value seen is enough. */
  static volatile LONG lock = 0;
  static DWORD last = 0;
  static ULONGLONG high = 0;
  while (InterlockedExchange(&lock, 1)) {
    Sleep(0);
  }
  const DWORD now = GetTickCount();
  if (now < last) {
    high += 0x100000000ULL;
  }
  last = now;
  const ULONGLONG result = high | now;
  InterlockedExchange(&lock, 0);
  return result;
}
XP_EXPORT(GetTickCount64, 0)

/* Windows 8+. The flags only concern Windows 8 features (write watch, placeholders). */
static BOOL WINAPI xp_UnmapViewOfFileEx(PVOID base_address, ULONG unmap_flags)
{
  static BOOL(WINAPI * real)(PVOID, ULONG) = (BOOL(WINAPI *)(PVOID, ULONG))1;
  XP_RESOLVE(real, "kernel32.dll", "UnmapViewOfFileEx");
  if (real) {
    return real(base_address, unmap_flags);
  }
  return UnmapViewOfFile(base_address);
}
XP_EXPORT(UnmapViewOfFileEx, 8)

/** \} */

/* -------------------------------------------------------------------- */
/** \name shell32.dll
 * \{ */

typedef struct XPKnownFolder {
  const KNOWNFOLDERID *id;
  int csidl;
} XPKnownFolder;

/* The known folders Blender asks for, mapped to XP's CSIDL equivalents. */
static const XPKnownFolder xp_known_folders[] = {
    {&FOLDERID_RoamingAppData, CSIDL_APPDATA},
    {&FOLDERID_LocalAppData, CSIDL_LOCAL_APPDATA},
    {&FOLDERID_ProgramData, CSIDL_COMMON_APPDATA},
    {&FOLDERID_Documents, CSIDL_PERSONAL},
    {&FOLDERID_Desktop, CSIDL_DESKTOPDIRECTORY},
    {&FOLDERID_Profile, CSIDL_PROFILE},
    {&FOLDERID_Pictures, CSIDL_MYPICTURES},
    {&FOLDERID_Music, CSIDL_MYMUSIC},
    {&FOLDERID_Videos, CSIDL_MYVIDEO},
    {&FOLDERID_Fonts, CSIDL_FONTS},
    {&FOLDERID_ProgramFiles, CSIDL_PROGRAM_FILES},
    {&FOLDERID_ProgramFilesCommon, CSIDL_PROGRAM_FILES_COMMON},
    {&FOLDERID_Windows, CSIDL_WINDOWS},
    {&FOLDERID_System, CSIDL_SYSTEM},
    {&FOLDERID_Recent, CSIDL_RECENT},
    {&FOLDERID_SendTo, CSIDL_SENDTO},
    {&FOLDERID_Templates, CSIDL_TEMPLATES},
    {&FOLDERID_Startup, CSIDL_STARTUP},
    {&FOLDERID_Programs, CSIDL_PROGRAMS},
    {&FOLDERID_StartMenu, CSIDL_STARTMENU},
    {&FOLDERID_Favorites, CSIDL_FAVORITES},
};

/* Vista+. */
static HRESULT WINAPI xp_SHGetKnownFolderPath(REFKNOWNFOLDERID rfid,
                                              DWORD flags,
                                              HANDLE token,
                                              PWSTR *r_path)
{
  static HRESULT(WINAPI * real)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *) =
      (HRESULT(WINAPI *)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *))1;
  XP_RESOLVE(real, "shell32.dll", "SHGetKnownFolderPath");
  if (real) {
    return real(rfid, flags, token, r_path);
  }

  *r_path = NULL;
  int csidl = -1;
  for (int i = 0; i < (int)(sizeof(xp_known_folders) / sizeof(*xp_known_folders)); i++) {
    if (IsEqualGUID(rfid, xp_known_folders[i].id)) {
      csidl = xp_known_folders[i].csidl;
      break;
    }
  }
  WCHAR path[MAX_PATH];
  if (csidl == -1) {
    /* XP has no Downloads folder, use the one modern Windows would create in the profile. */
    if (!IsEqualGUID(rfid, &FOLDERID_Downloads) ||
        FAILED(SHGetFolderPathW(NULL, CSIDL_PROFILE, token, SHGFP_TYPE_CURRENT, path)))
    {
      return E_INVALIDARG;
    }
    lstrcatW(path, L"\\Downloads");
  }
  else {
    if (flags & KF_FLAG_CREATE) {
      csidl |= CSIDL_FLAG_CREATE;
    }
    const HRESULT result = SHGetFolderPathW(NULL, csidl, token, SHGFP_TYPE_CURRENT, path);
    if (FAILED(result)) {
      return result;
    }
    if (result == S_FALSE) {
      /* The folder doesn't exist. */
      return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
  }
  const size_t size = (lstrlenW(path) + 1) * sizeof(WCHAR);
  *r_path = (PWSTR)CoTaskMemAlloc(size);
  if (!*r_path) {
    return E_OUTOFMEMORY;
  }
  memcpy(*r_path, path, size);
  return S_OK;
}
XP_EXPORT(SHGetKnownFolderPath, 16)

/** \} */

/* -------------------------------------------------------------------- */
/** \name comctl32.dll
 * \{ */

/* Vista+ (common controls 6). On XP a message box with the same text, without custom buttons. */
static HRESULT WINAPI xp_TaskDialogIndirect(const TASKDIALOGCONFIG *config,
                                            int *r_button,
                                            int *r_radio_button,
                                            BOOL *r_verification_checked)
{
  static HRESULT(WINAPI * real)(const TASKDIALOGCONFIG *, int *, int *, BOOL *) =
      (HRESULT(WINAPI *)(const TASKDIALOGCONFIG *, int *, int *, BOOL *))1;
  XP_RESOLVE(real, "comctl32.dll", "TaskDialogIndirect");
  if (real) {
    return real(config, r_button, r_radio_button, r_verification_checked);
  }

  /* Text members may be resource IDs (`MAKEINTRESOURCE`), only strings are shown. */
  const WCHAR *instruction = IS_INTRESOURCE(config->pszMainInstruction) ?
                                 NULL :
                                 config->pszMainInstruction;
  const WCHAR *content = IS_INTRESOURCE(config->pszContent) ? NULL : config->pszContent;
  const WCHAR *title = IS_INTRESOURCE(config->pszWindowTitle) ? NULL : config->pszWindowTitle;
  WCHAR text[4096];
  text[0] = 0;
  if (instruction) {
    lstrcpynW(text, instruction, 4096);
  }
  if (content) {
    const int len = lstrlenW(text);
    if (len && len < 4094) {
      lstrcatW(text, L"\n\n");
    }
    lstrcpynW(text + lstrlenW(text), content, 4096 - lstrlenW(text));
  }
  const UINT icon = (config->pszMainIcon == TD_ERROR_ICON) ? MB_ICONERROR :
                    (config->pszMainIcon == TD_WARNING_ICON) ? MB_ICONWARNING :
                                                                MB_ICONINFORMATION;
  MessageBoxW(config->hwndParent, text, title ? title : L"", MB_OK | icon);
  if (r_button) {
    *r_button = IDOK;
  }
  if (r_radio_button) {
    *r_radio_button = 0;
  }
  if (r_verification_checked) {
    *r_verification_checked = FALSE;
  }
  return S_OK;
}
XP_EXPORT(TaskDialogIndirect, 16)

/** \} */
