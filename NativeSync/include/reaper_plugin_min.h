/*
 * Minimal REAPER extension ABI declarations.
 * Based on the public REAPER plug-in SDK.
 */
#pragma once

#ifdef _WIN32
#  include <windows.h>
#  define REAPER_PLUGIN_DLL_EXPORT __declspec(dllexport)
#  define REAPER_PLUGIN_HINSTANCE HINSTANCE
#else
#  define REAPER_PLUGIN_DLL_EXPORT __attribute__((visibility("default")))
#  define REAPER_PLUGIN_HINSTANCE void *
#endif

#define REAPER_PLUGIN_ENTRYPOINT ReaperPluginEntry
#define REAPER_PLUGIN_VERSION 0x20E

struct reaper_plugin_info_t {
  int caller_version;
  void *hwnd_main;
  int (*Register)(const char *name, void *info);
  void *(*GetFunc)(const char *name);
};
