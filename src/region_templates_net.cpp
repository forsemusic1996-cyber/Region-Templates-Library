/*
 * Region Templates Fast Sync
 *
 * A small REAPER extension that exposes asynchronous HTTP downloads to Lua.
 * The Lua UI remains in the Region Templates script; this module owns the
 * network threads, libcurl handles, retries/cancellation, and atomic files.
 */

#include "reaper_plugin_min.h"

#include <curl/curl.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#ifndef _WIN32
#include <filesystem>
#endif
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <chrono>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
namespace fs = std::filesystem;
#endif

namespace {

constexpr int kPending = 0;
constexpr int kSuccess = 1;
constexpr int kFailure = -1;
constexpr size_t kWorkerCount = 6;

struct Job {
  int id = 0;
  std::string url;
  std::string target;
  std::atomic<int> state{kPending};
  std::atomic_bool cancel{false};
  std::mutex mutex;
  std::string error;
};

std::mutex g_jobs_mutex;
std::unordered_map<int, std::shared_ptr<Job>> g_jobs;
std::mutex g_queue_mutex;
std::condition_variable g_queue_wake;
std::queue<std::shared_ptr<Job>> g_queue;
std::vector<std::thread> g_workers;
std::atomic_bool g_stopping{false};
std::atomic_int g_next_id{1};

CURLSH *g_share = nullptr;
std::mutex g_curl_mutex;

void CurlLock(CURL *, curl_lock_data, curl_lock_access, void *)
{
  g_curl_mutex.lock();
}

void CurlUnlock(CURL *, curl_lock_data, curl_lock_access, void *)
{
  g_curl_mutex.unlock();
}

bool IsCancelled(const std::shared_ptr<Job> &job)
{
  return g_stopping.load() || job->cancel.load();
}

int ProgressCallback(void *userdata, curl_off_t, curl_off_t,
    curl_off_t, curl_off_t)
{
  auto *job = static_cast<Job *>(userdata);
  return (g_stopping.load() || job->cancel.load()) ? 1 : 0;
}

size_t WriteCallback(char *data, size_t size, size_t count, void *userdata)
{
  return std::fwrite(data, 1, size * count, static_cast<FILE *>(userdata));
}

void SetError(const std::shared_ptr<Job> &job, std::string message)
{
  std::lock_guard<std::mutex> lock(job->mutex);
  job->error = std::move(message);
}

void FailJob(const std::shared_ptr<Job> &job, const std::string &error)
{
  SetError(job, error);
  job->state = kFailure; // Publish error text before publishing completion.
}

#ifdef _WIN32
std::wstring WidePath(const std::string &path)
{
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      path.c_str(), -1, nullptr, 0);
  if (!size) return {};
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, &result[0], size);
  result.pop_back();
  return result;
}

bool MakeDirectory(const std::wstring &path)
{
  if (path.empty() || (path.size() == 2 && path[1] == L':')) return true;
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES)
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const auto slash = path.find_last_of(L"/\\");
  if (slash != std::wstring::npos && !MakeDirectory(path.substr(0, slash))) return false;
  return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
#endif

bool PrepareDirectory(const std::string &path)
{
#ifdef _WIN32
  const auto wide = WidePath(path);
  if (wide.empty()) return false;
  const auto slash = wide.find_last_of(L"/\\");
  return slash == std::wstring::npos || MakeDirectory(wide.substr(0, slash));
#else
  std::error_code ec;
  const auto parent = fs::u8path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);
  return !ec;
#endif
}

FILE *OpenOutput(const std::string &path)
{
#ifdef _WIN32
  return _wfopen(WidePath(path).c_str(), L"wb");
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

void RemoveOutput(const std::string &path)
{
#ifdef _WIN32
  DeleteFileW(WidePath(path).c_str());
#else
  std::remove(path.c_str());
#endif
}

bool CommitOutput(const std::string &temporary, const std::string &target)
{
#ifdef _WIN32
  return MoveFileExW(WidePath(temporary).c_str(), WidePath(target).c_str(),
      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return std::rename(temporary.c_str(), target.c_str()) == 0;
#endif
}

void RunJob(const std::shared_ptr<Job> &job, CURL *curl)
{
  if (IsCancelled(job)) {
    FailJob(job, "cancelled");
    return;
  }

  if (!PrepareDirectory(job->target)) {
    FailJob(job, "cannot create cache directory");
    return;
  }

  const std::string temporary = job->target + ".part." + std::to_string(job->id);
  FILE *output = OpenOutput(temporary);
  if (!output) {
    FailJob(job, "cannot open temporary download file");
    return;
  }

  if (!curl) {
    std::fclose(output);
    RemoveOutput(temporary);
    FailJob(job, "curl initialization failed");
    return;
  }

  // Keep one easy handle per worker, as ReaPack does. Reset options, retain
  // cached live connections, TLS sessions and DNS information.
  curl_easy_reset(curl);
  char error_buffer[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_URL, job->url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "RegionTemplates/2.1-win7");
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_REVOKE_BEST_EFFORT);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https,http");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https,http");
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(curl, CURLOPT_SHARE, g_share);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, output);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, job.get());

  const CURLcode result = curl_easy_perform(curl);
  long response_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  const int close_result = std::fclose(output);

  if (result != CURLE_OK) {
    RemoveOutput(temporary);
    std::string message = error_buffer[0] ? error_buffer : curl_easy_strerror(result);
    if (response_code)
      message += " (HTTP " + std::to_string(response_code) + ")";
    FailJob(job, message);
    return;
  }

  if (IsCancelled(job) || close_result != 0) {
    RemoveOutput(temporary);
    FailJob(job, IsCancelled(job) ? "cancelled" : "cannot flush downloaded file");
    return;
  }

  if (!CommitOutput(temporary, job->target)) {
    RemoveOutput(temporary);
    FailJob(job, "cannot commit downloaded file");
    return;
  }

  job->state = kSuccess;
}

void WorkerLoop()
{
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  while (true) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock<std::mutex> lock(g_queue_mutex);
      g_queue_wake.wait(lock, [] {
        return g_stopping.load() || !g_queue.empty();
      });
      if (g_stopping.load() && g_queue.empty())
        return;
      job = std::move(g_queue.front());
      g_queue.pop();
    }
    RunJob(job, curl.get());
  }
}

void StartWorkers()
{
  g_stopping = false;
  for (size_t i = 0; i < kWorkerCount; ++i)
    g_workers.emplace_back(WorkerLoop);
}

void StopWorkers()
{
  g_stopping = true;
  {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    for (auto &[id, job] : g_jobs)
      job->cancel = true;
  }
  g_queue_wake.notify_all();
  for (auto &worker : g_workers)
    if (worker.joinable()) worker.join();
  g_workers.clear();
  std::queue<std::shared_ptr<Job>> empty;
  std::swap(g_queue, empty);
}

std::shared_ptr<Job> FindJob(int id)
{
  std::lock_guard<std::mutex> lock(g_jobs_mutex);
  const auto it = g_jobs.find(id);
  return it == g_jobs.end() ? nullptr : it->second;
}

double HttpStart(char *url, char *target)
{
  if (!url || !target || !*url || !*target || g_stopping.load())
    return -1;

  auto job = std::make_shared<Job>();
  job->id = g_next_id.fetch_add(1);
  job->url = url;
  job->target = target;
  {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    g_jobs.emplace(job->id, job);
  }
  {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_queue.push(job);
  }
  g_queue_wake.notify_one();
  return static_cast<double>(job->id);
}

double HttpPoll(double id)
{
  const auto job = FindJob(static_cast<int>(id));
  return job ? static_cast<double>(job->state.load()) : -1;
}

double HttpWait(double id)
{
  const auto job = FindJob(static_cast<int>(id));
  if (!job) return -1;
  while (job->state.load() == kPending) {
    if (g_stopping.load()) return -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return static_cast<double>(job->state.load());
}

char *HttpError(double id)
{
  static thread_local std::string result;
  const auto job = FindJob(static_cast<int>(id));
  if (!job) {
    result = "unknown job";
    return result.data();
  }
  std::lock_guard<std::mutex> lock(job->mutex);
  result = job->error;
  return result.data();
}

double HttpCancel(double id)
{
  const auto job = FindJob(static_cast<int>(id));
  if (!job) return 0;
  job->cancel = true;
  return 1;
}

double HttpRelease(double id)
{
  std::lock_guard<std::mutex> lock(g_jobs_mutex);
  return g_jobs.erase(static_cast<int>(id)) ? 1 : 0;
}

const char kDefHttpStart[] =
  "double\0char*,char*\0url,target\0Start an asynchronous Region Templates HTTP download\0";
const char kDefHttpPoll[] =
  "double\0double\0job\0Return 0 while pending, 1 on success, -1 on failure\0";
const char kDefHttpError[] =
  "char*\0double\0job\0Return the error text for a Region Templates download\0";
const char kDefHttpWait[] =
  "double\0double\0job\0Wait for a Region Templates HTTP download to finish\0";
const char kDefHttpCancel[] =
  "double\0double\0job\0Cancel a Region Templates HTTP download\0";
const char kDefHttpRelease[] =
  "double\0double\0job\0Release a completed Region Templates HTTP job\0";

// REAPER needs an APIvararg adapter in addition to API_ and APIdef_ for
// functions that should be callable from Lua/EEL/Python ReaScripts.
thread_local double g_vararg_result = 0.0;

void *VarargHttpStart(void **args, int count)
{
  g_vararg_result = (args && count >= 2 && args[0] && args[1])
      ? HttpStart(static_cast<char *>(args[0]), static_cast<char *>(args[1]))
      : -1.0;
  return &g_vararg_result;
}

void *VarargHttpPoll(void **args, int count)
{
  g_vararg_result = (args && count >= 1 && args[0])
      ? HttpPoll(*static_cast<double *>(args[0])) : -1.0;
  return &g_vararg_result;
}

void *VarargHttpError(void **args, int count)
{
  return (args && count >= 1 && args[0])
      ? HttpError(*static_cast<double *>(args[0]))
      : const_cast<char *>("invalid Region Templates HTTP job");
}

void *VarargHttpWait(void **args, int count)
{
  g_vararg_result = (args && count >= 1 && args[0])
      ? HttpWait(*static_cast<double *>(args[0])) : -1.0;
  return &g_vararg_result;
}

void *VarargHttpCancel(void **args, int count)
{
  g_vararg_result = (args && count >= 1 && args[0])
      ? HttpCancel(*static_cast<double *>(args[0])) : 0.0;
  return &g_vararg_result;
}

void *VarargHttpRelease(void **args, int count)
{
  g_vararg_result = (args && count >= 1 && args[0])
      ? HttpRelease(*static_cast<double *>(args[0])) : 0.0;
  return &g_vararg_result;
}

int (*g_register)(const char *, void *) = nullptr;

char *HttpVersion()
{
  static std::string version = std::string("RegionTemplatesNet 2.1 / ") + curl_version();
  return &version[0];
}
void *VarargHttpVersion(void **, int) { return HttpVersion(); }
const char kDefHttpVersion[] = "char*\0\0\0Native network build and TLS backend\0";

void RegisterApi(bool add)
{
  if (add) {
    g_register("API_RegionTemplates_HttpVersion", reinterpret_cast<void *>(&HttpVersion));
    g_register("APIvararg_RegionTemplates_HttpVersion", reinterpret_cast<void *>(&VarargHttpVersion));
    g_register("APIdef_RegionTemplates_HttpVersion", const_cast<char *>(kDefHttpVersion));
    g_register("API_RegionTemplates_HttpStart", reinterpret_cast<void *>(&HttpStart));
    g_register("APIvararg_RegionTemplates_HttpStart", reinterpret_cast<void *>(&VarargHttpStart));
    g_register("APIdef_RegionTemplates_HttpStart", const_cast<char *>(kDefHttpStart));
    g_register("API_RegionTemplates_HttpPoll", reinterpret_cast<void *>(&HttpPoll));
    g_register("APIvararg_RegionTemplates_HttpPoll", reinterpret_cast<void *>(&VarargHttpPoll));
    g_register("APIdef_RegionTemplates_HttpPoll", const_cast<char *>(kDefHttpPoll));
    g_register("API_RegionTemplates_HttpError", reinterpret_cast<void *>(&HttpError));
    g_register("APIvararg_RegionTemplates_HttpError", reinterpret_cast<void *>(&VarargHttpError));
    g_register("APIdef_RegionTemplates_HttpError", const_cast<char *>(kDefHttpError));
    g_register("API_RegionTemplates_HttpWait", reinterpret_cast<void *>(&HttpWait));
    g_register("APIvararg_RegionTemplates_HttpWait", reinterpret_cast<void *>(&VarargHttpWait));
    g_register("APIdef_RegionTemplates_HttpWait", const_cast<char *>(kDefHttpWait));
    g_register("API_RegionTemplates_HttpCancel", reinterpret_cast<void *>(&HttpCancel));
    g_register("APIvararg_RegionTemplates_HttpCancel", reinterpret_cast<void *>(&VarargHttpCancel));
    g_register("APIdef_RegionTemplates_HttpCancel", const_cast<char *>(kDefHttpCancel));
    g_register("API_RegionTemplates_HttpRelease", reinterpret_cast<void *>(&HttpRelease));
    g_register("APIvararg_RegionTemplates_HttpRelease", reinterpret_cast<void *>(&VarargHttpRelease));
    g_register("APIdef_RegionTemplates_HttpRelease", const_cast<char *>(kDefHttpRelease));
  } else {
    g_register("-API_RegionTemplates_HttpVersion", reinterpret_cast<void *>(&HttpVersion));
    g_register("-APIvararg_RegionTemplates_HttpVersion", reinterpret_cast<void *>(&VarargHttpVersion));
    g_register("-APIdef_RegionTemplates_HttpVersion", const_cast<char *>(kDefHttpVersion));
    g_register("-APIvararg_RegionTemplates_HttpStart", reinterpret_cast<void *>(&VarargHttpStart));
    g_register("-API_RegionTemplates_HttpStart", reinterpret_cast<void *>(&HttpStart));
    g_register("-APIdef_RegionTemplates_HttpStart", const_cast<char *>(kDefHttpStart));
    g_register("-APIvararg_RegionTemplates_HttpPoll", reinterpret_cast<void *>(&VarargHttpPoll));
    g_register("-API_RegionTemplates_HttpPoll", reinterpret_cast<void *>(&HttpPoll));
    g_register("-APIdef_RegionTemplates_HttpPoll", const_cast<char *>(kDefHttpPoll));
    g_register("-APIvararg_RegionTemplates_HttpError", reinterpret_cast<void *>(&VarargHttpError));
    g_register("-API_RegionTemplates_HttpError", reinterpret_cast<void *>(&HttpError));
    g_register("-APIdef_RegionTemplates_HttpError", const_cast<char *>(kDefHttpError));
    g_register("-APIvararg_RegionTemplates_HttpWait", reinterpret_cast<void *>(&VarargHttpWait));
    g_register("-API_RegionTemplates_HttpWait", reinterpret_cast<void *>(&HttpWait));
    g_register("-APIdef_RegionTemplates_HttpWait", const_cast<char *>(kDefHttpWait));
    g_register("-APIvararg_RegionTemplates_HttpCancel", reinterpret_cast<void *>(&VarargHttpCancel));
    g_register("-API_RegionTemplates_HttpCancel", reinterpret_cast<void *>(&HttpCancel));
    g_register("-APIdef_RegionTemplates_HttpCancel", const_cast<char *>(kDefHttpCancel));
    g_register("-APIvararg_RegionTemplates_HttpRelease", reinterpret_cast<void *>(&VarargHttpRelease));
    g_register("-API_RegionTemplates_HttpRelease", reinterpret_cast<void *>(&HttpRelease));
    g_register("-APIdef_RegionTemplates_HttpRelease", const_cast<char *>(kDefHttpRelease));
  }
}

} // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t *rec)
{
  if (rec) {
    if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->Register)
      return 0;
    g_register = rec->Register;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      return 0;
    g_share = curl_share_init();
    if (!g_share) {
      curl_global_cleanup();
      return 0;
    }
    curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, CurlLock);
    curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, CurlUnlock);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    StartWorkers();
    RegisterApi(true);
    return 1;
  }

  if (g_register)
    RegisterApi(false);
  StopWorkers();
  if (g_share) {
    curl_share_cleanup(g_share);
    g_share = nullptr;
  }
  curl_global_cleanup();
  g_register = nullptr;
  return 0;
}
