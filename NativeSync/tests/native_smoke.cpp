#include "../include/reaper_plugin_min.h"
#include <map>
#include <string>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

std::map<std::string, void *> api;
int Register(const char *name, void *fn) { api[name] = fn; return 1; }
void Require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
std::string Read(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  auto module = LoadLibraryA(argv[1]);
  if (!module) { std::cerr << "LoadLibrary failed: " << GetLastError(); return 3; }
  auto entry = reinterpret_cast<int(*)(HINSTANCE, reaper_plugin_info_t *)>(GetProcAddress(module, "ReaperPluginEntry"));
  if (!entry) return 4;
  reaper_plugin_info_t host{REAPER_PLUGIN_VERSION, nullptr, Register, nullptr};
  if (!entry(module, &host)) return 5;
  int result = 0;
  try {
    auto start = reinterpret_cast<double(*)(char*,char*)>(api.at("API_RegionTemplates_HttpStart"));
    auto wait = reinterpret_cast<double(*)(double)>(api.at("API_RegionTemplates_HttpWait"));
    auto error = reinterpret_cast<char*(*)(double)>(api.at("API_RegionTemplates_HttpError"));
    auto release = reinterpret_cast<double(*)(double)>(api.at("API_RegionTemplates_HttpRelease"));
    auto version = reinterpret_cast<char*(*)()>(api.at("API_RegionTemplates_HttpVersion"));
    auto varstart = reinterpret_cast<void*(*)(void**,int)>(api.at("APIvararg_RegionTemplates_HttpStart"));
    auto varwait = reinterpret_cast<void*(*)(void**,int)>(api.at("APIvararg_RegionTemplates_HttpWait"));
    auto varerror = reinterpret_cast<void*(*)(void**,int)>(api.at("APIvararg_RegionTemplates_HttpError"));
    std::cout << version() << std::endl;
    std::string url = "https://raw.githubusercontent.com/forsemusic1996-cyber/Region-Templates-Library/main/RegionTemplates/index.json";
    std::string target = "native-test/catalog.json";
    void *args[] = {&url[0], &target[0]};
    double job = *static_cast<double*>(varstart(args, 2));
    Require(job > 0, "ReaScript ABI start failed");
    void *waitargs[] = {&job};
    const double status = *static_cast<double*>(varwait(waitargs, 1));
    if (status != 1) throw std::runtime_error(static_cast<char*>(varerror(waitargs, 1)));
    Require(Read(target).find(".rgti") != std::string::npos, "Invalid downloaded index");
    release(job);
    std::cout << "PASS: native HTTPS + ReaScript vararg ABI + catalog content" << std::endl;
    const auto original = Read(target);
    // UTF-8 output path checks the filesystem layer independently of ASCII paths.
    std::string unicode = "native-test/\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82/catalog.json";
    job = start(&url[0], &unicode[0]);
    Require(wait(job) == 1, "Unicode download failed");
    release(job);
    Require(GetFileAttributesW(L"native-test/\u0422\u0435\u0441\u0442/catalog.json") != INVALID_FILE_ATTRIBUTES,
        "Unicode output path missing");
    std::vector<double> batch;
    for (int i = 0; i < 12; ++i) {
      std::string path = "native-test/batch-" + std::to_string(i) + ".json";
      batch.push_back(start(&url[0], &path[0]));
    }
    for (auto id : batch) { if (wait(id) != 1) throw std::runtime_error(error(id)); release(id); }
    std::cout << "PASS: UTF-8 paths + 12 queued native downloads" << std::endl;
    std::string missing = url + ".definitely-missing-native-test";
    job = start(&missing[0], &target[0]);
    Require(wait(job) == -1, "HTTP error not reported");
    Require(error(job)[0] != 0, "Missing native error text");
    Require(Read(target) == original, "Failed request overwrote existing target");
    std::cout << "PASS: HTTP error reported and previous target preserved: " << error(job) << std::endl;
    release(job);
  } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << std::endl; result = 1; }
  entry(module, nullptr);
  FreeLibrary(module);
  return result;
}
