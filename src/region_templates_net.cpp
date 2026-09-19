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
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <chrono>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

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
  auto *stream = static_cast<std::ofstream *>(userdata);
  const size_t bytes = size * count;
  stream->write(data, static_cast<std::streamsize>(bytes));
  return stream->good() ? bytes : 0;
}

void SetError(const std::shared_ptr<Job> &job, std::string message)
{
  std::lock_guard<std::mutex> lock(job->mutex);
  job->error = std::move(message);
}

void RunJob(const std::shared_ptr<Job> &job)
{
  if (IsCancelled(job)) {
    job->state = kFailure;
    SetError(job, "cancelled");
    return;
  }

  const fs::path target(job->target);
  const fs::path parent = target.parent_path();
  std::error_code ec;
  if (!parent.empty())
    fs::create_directories(parent, ec);
  if (ec) {
    job->state = kFailure;
    SetError(job, "cannot create cache directory: " + ec.message());
    return;
  }

  const fs::path temporary = target.string() + ".part." + std::to_string(job->id);
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) {
    job->state = kFailure;
    SetError(job, "cannot open temporary download file");
    return;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    output.close();
    fs::remove(temporary, ec);
    job->state = kFailure;
    SetError(job, "curl initialization failed");
    return;
  }

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
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "RegionTemplates/2.0");
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(curl, CURLOPT_SHARE, g_share);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, job.get());

  const CURLcode result = curl_easy_perform(curl);
  long response_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  curl_easy_cleanup(curl);
  output.close();

  if (result != CURLE_OK) {
    fs::remove(temporary, ec);
    std::string message = error_buffer[0] ? error_buffer : curl_easy_strerror(result);
    if (response_code)
      message += " (HTTP " + std::to_string(response_code) + ")";
    job->state = kFailure;
    SetError(job, std::move(message));
    return;
  }

  if (IsCancelled(job)) {
    fs::remove(temporary, ec);
    job->state = kFailure;
    SetError(job, "cancelled");
    return;
  }

  fs::remove(target, ec);
  ec.clear();
  fs::rename(temporary, target, ec);
  if (ec) {
    fs::remove(temporary, ec);
    job->state = kFailure;
    SetError(job, "cannot commit downloaded file: " + ec.message());
    return;
  }

  job->state = kSuccess;
}

void WorkerLoop()
{
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
    RunJob(job);
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

int (*g_register)(const char *, void *) = nullptr;

void RegisterApi(bool add)
{
  if (add) {
    g_register("API_RegionTemplates_HttpStart", reinterpret_cast<void *>(&HttpStart));
    g_register("APIdef_RegionTemplates_HttpStart", const_cast<char *>(kDefHttpStart));
    g_register("API_RegionTemplates_HttpPoll", reinterpret_cast<void *>(&HttpPoll));
    g_register("APIdef_RegionTemplates_HttpPoll", const_cast<char *>(kDefHttpPoll));
    g_register("API_RegionTemplates_HttpError", reinterpret_cast<void *>(&HttpError));
    g_register("APIdef_RegionTemplates_HttpError", const_cast<char *>(kDefHttpError));
    g_register("API_RegionTemplates_HttpWait", reinterpret_cast<void *>(&HttpWait));
    g_register("APIdef_RegionTemplates_HttpWait", const_cast<char *>(kDefHttpWait));
    g_register("API_RegionTemplates_HttpCancel", reinterpret_cast<void *>(&HttpCancel));
    g_register("APIdef_RegionTemplates_HttpCancel", const_cast<char *>(kDefHttpCancel));
    g_register("API_RegionTemplates_HttpRelease", reinterpret_cast<void *>(&HttpRelease));
    g_register("APIdef_RegionTemplates_HttpRelease", const_cast<char *>(kDefHttpRelease));
  } else {
    g_register("-API_RegionTemplates_HttpStart", reinterpret_cast<void *>(&HttpStart));
    g_register("-APIdef_RegionTemplates_HttpStart", const_cast<char *>(kDefHttpStart));
    g_register("-API_RegionTemplates_HttpPoll", reinterpret_cast<void *>(&HttpPoll));
    g_register("-APIdef_RegionTemplates_HttpPoll", const_cast<char *>(kDefHttpPoll));
    g_register("-API_RegionTemplates_HttpError", reinterpret_cast<void *>(&HttpError));
    g_register("-APIdef_RegionTemplates_HttpError", const_cast<char *>(kDefHttpError));
    g_register("-API_RegionTemplates_HttpWait", reinterpret_cast<void *>(&HttpWait));
    g_register("-APIdef_RegionTemplates_HttpWait", const_cast<char *>(kDefHttpWait));
    g_register("-API_RegionTemplates_HttpCancel", reinterpret_cast<void *>(&HttpCancel));
    g_register("-APIdef_RegionTemplates_HttpCancel", const_cast<char *>(kDefHttpCancel));
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
