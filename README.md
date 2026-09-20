# Region Templates Fast Sync

This folder contains the native networking backend for the Region Templates REAPER script. It uses libcurl, six worker threads, compressed HTTP responses, shared DNS/SSL state, cancellation, and atomic cache-file replacement.
The update-region-templates-index.yml workflow also records each template's GitHub creation date, last modification date, file size, and content SHA. The Lua UI displays the dates in Online Library and shows them again in a local template tooltip after import.
The GitHub Actions workflow builds:
- RegionTemplatesNet.dll for Windows x64.

The Windows build statically links libcurl and targets Windows 7 or newer, so it does not depend on a separately installed curl.exe.
The extension exposes these ReaScript functions:
- reaper.RegionTemplates_HttpStart(url, target_path)
- reaper.RegionTemplates_HttpPoll(job_id)
- reaper.RegionTemplates_HttpWait(job_id)
- reaper.RegionTemplates_HttpError(job_id)
- reaper.RegionTemplates_HttpCancel(job_id)
- reaper.RegionTemplates_HttpRelease(job_id)

The existing Lua script can keep its curl fallback, so the script remains usable when the native module has not been installed yet.

## Installation

Copy the Lua file to the REAPER Scripts directory. Copy the Windows native artifact to REAPER UserPlugins, then restart REAPER. The script will use the native module automatically; without it, the existing curl fallback is retained.
