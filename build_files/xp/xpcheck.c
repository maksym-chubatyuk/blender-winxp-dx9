/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* xpcheck: run ON the Windows XP machine to list every import of the given PE files (EXE/DLL/PYD)
 * that this system cannot resolve, without loading them. A missing export otherwise only shows up
 * as a modal "procedure entry point not found" box naming the first one.
 *
 * Usage: xpcheck.exe [-L dir]... <file-or-dir>...  (a directory is scanned for *.exe, *.dll and
 * *.pyd, recursively). DLLs are resolved from the file's own folder, then each `-L` folder (for
 * modules whose DLLs the host process loads first, e.g. `.pyd` next to `libpython`), then the
 * system search path.
 * Exit code: number of files with problems.
 *
 * Build (plain C on XP's own msvcrt so the checker itself never depends on the UCRT):
 *   i686-w64-mingw32-gcc -O2 -std=c99 -mcrtdll=msvcrt-os xpcheck.c -o xpcheck.exe -static */

#include <stdio.h>
#include <string.h>
#include <windows.h>

static int g_bad_files = 0;
static const char *g_search_dirs[32];
static int g_search_dirs_num = 0;

static void *rva_to_ptr(unsigned char *base, IMAGE_NT_HEADERS32 *nt, DWORD rva)
{
  IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
    DWORD size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
    if (rva >= sec->VirtualAddress && rva < sec->VirtualAddress + size) {
      return base + sec->PointerToRawData + (rva - sec->VirtualAddress);
    }
  }
  return NULL;
}

/* Load a dependency the way the loader would for `dir`: local folder first, then the system. */
static HMODULE load_dep(const char *dir, const char *dll)
{
  char path[MAX_PATH * 2];
  HMODULE mod;
  _snprintf(path, sizeof(path), "%s\\%s", dir, dll);
  mod = LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
  for (int i = 0; !mod && i < g_search_dirs_num; i++) {
    _snprintf(path, sizeof(path), "%s\\%s", g_search_dirs[i], dll);
    mod = LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
  }
  if (!mod) {
    mod = LoadLibraryExA(dll, NULL, DONT_RESOLVE_DLL_REFERENCES);
  }
  return mod;
}

static void check_file(const char *path)
{
  HANDLE file, mapping;
  unsigned char *base;
  IMAGE_DOS_HEADER *dos;
  IMAGE_NT_HEADERS32 *nt;
  IMAGE_DATA_DIRECTORY *dirent;
  IMAGE_IMPORT_DESCRIPTOR *imp;
  char dir[MAX_PATH];
  char *slash;
  int problems = 0;

  strncpy(dir, path, sizeof(dir) - 1);
  dir[sizeof(dir) - 1] = 0;
  slash = strrchr(dir, '\\');
  if (!slash) {
    slash = strrchr(dir, '/');
  }
  if (slash) {
    *slash = 0;
  }
  else {
    strcpy(dir, ".");
  }

  file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    printf("%s: cannot open\n", path);
    g_bad_files++;
    return;
  }
  mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
  base = mapping ? (unsigned char *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : NULL;
  if (!base) {
    printf("%s: cannot map\n", path);
    goto done;
  }

  dos = (IMAGE_DOS_HEADER *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    printf("%s: not a PE file\n", path);
    problems++;
    goto done;
  }
  nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
  {
    printf("%s: not a 32-bit PE file\n", path);
    problems++;
    goto done;
  }
  /* XP refuses images whose subsystem/OS version is above 5.1 ("not a valid Win32 application"). */
  if (nt->OptionalHeader.MajorSubsystemVersion > 5 ||
      (nt->OptionalHeader.MajorSubsystemVersion == 5 &&
       nt->OptionalHeader.MinorSubsystemVersion > 1) ||
      nt->OptionalHeader.MajorOperatingSystemVersion > 5)
  {
    printf("%s: subsystem %u.%u / OS %u.%u is too new for XP (need <= 5.1)\n",
           path,
           nt->OptionalHeader.MajorSubsystemVersion,
           nt->OptionalHeader.MinorSubsystemVersion,
           nt->OptionalHeader.MajorOperatingSystemVersion,
           nt->OptionalHeader.MinorOperatingSystemVersion);
    problems++;
  }

  dirent = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  imp = dirent->VirtualAddress ?
            (IMAGE_IMPORT_DESCRIPTOR *)rva_to_ptr(base, nt, dirent->VirtualAddress) :
            NULL;
  for (; imp && imp->Name; imp++) {
    const char *dll = (const char *)rva_to_ptr(base, nt, imp->Name);
    DWORD thunk_rva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
    IMAGE_THUNK_DATA32 *thunk = (IMAGE_THUNK_DATA32 *)rva_to_ptr(base, nt, thunk_rva);
    HMODULE mod = load_dep(dir, dll);
    if (!mod) {
      printf("%s: missing DLL %s\n", path, dll);
      problems++;
      continue;
    }
    for (; thunk && thunk->u1.AddressOfData; thunk++) {
      if (IMAGE_SNAP_BY_ORDINAL32(thunk->u1.Ordinal)) {
        WORD ord = (WORD)IMAGE_ORDINAL32(thunk->u1.Ordinal);
        if (!GetProcAddress(mod, (LPCSTR)(ULONG_PTR)ord)) {
          printf("%s: %s!#%u\n", path, dll, ord);
          problems++;
        }
      }
      else {
        IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)rva_to_ptr(
            base, nt, thunk->u1.AddressOfData);
        if (ibn && !GetProcAddress(mod, (LPCSTR)ibn->Name)) {
          printf("%s: %s!%s\n", path, dll, (const char *)ibn->Name);
          problems++;
        }
      }
    }
    FreeLibrary(mod);
  }

done:
  if (base) {
    UnmapViewOfFile(base);
  }
  if (mapping) {
    CloseHandle(mapping);
  }
  CloseHandle(file);
  if (problems) {
    g_bad_files++;
  }
}

static int has_pe_ext(const char *name)
{
  const char *dot = strrchr(name, '.');
  return dot && (!_stricmp(dot, ".exe") || !_stricmp(dot, ".dll") || !_stricmp(dot, ".pyd"));
}

static void check_dir(const char *dir)
{
  char pattern[MAX_PATH * 2], sub[MAX_PATH * 2];
  WIN32_FIND_DATAA fd;
  HANDLE h;
  _snprintf(pattern, sizeof(pattern), "%s\\*", dir);
  h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) {
      continue;
    }
    _snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      check_dir(sub);
    }
    else if (has_pe_ext(fd.cFileName)) {
      check_file(sub);
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

int main(int argc, char **argv)
{
  /* Never let a failed LoadLibrary pop up a modal error box. */
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-L") && i + 1 < argc) {
      if (g_search_dirs_num < 32) {
        g_search_dirs[g_search_dirs_num++] = argv[i + 1];
      }
      i++;
      continue;
    }
    DWORD attr = GetFileAttributesA(argv[i]);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
      check_dir(argv[i]);
    }
    else {
      check_file(argv[i]);
    }
  }
  printf("xpcheck: %d file(s) with problems\n", g_bad_files);
  return g_bad_files;
}
