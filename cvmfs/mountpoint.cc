/**
 * This file is part of the CernVM File System.
 */
#include "cvmfs_config.h"
#include "mountpoint.h"

#include <errno.h>
#include <fcntl.h>
#ifndef CVMFS_LIBCVMFS
#include <fuse/fuse_lowlevel.h>
#endif
#include <inttypes.h>
#include <stdint.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

#ifndef CVMFS_LIBCVMFS
#ifdef FUSE_CAP_EXPORT_SUPPORT
#define CVMFS_NFS_SUPPORT
#else
#warning "No NFS support, Fuse too old"
#endif
#endif

#include "authz/authz_curl.h"
#include "authz/authz_fetch.h"
#include "authz/authz_session.h"
#include "backoff.h"
#include "cache.h"
#include "cache_extern.h"
#include "cache_posix.h"
#include "cache_ram.h"
#include "cache_tiered.h"
#include "catalog.h"
#include "catalog_mgr_client.h"
#include "clientctx.h"
#include "download.h"
#include "duplex_sqlite3.h"
#include "fetch.h"
#include "file_chunk.h"
#include "globals.h"
#include "glue_buffer.h"
#include "google/protobuf/stubs/common.h"
#include "history.h"
#include "history_sqlite.h"
#include "logging.h"
#include "lru_md.h"
#include "manifest.h"
#include "manifest_fetch.h"
#ifdef CVMFS_NFS_SUPPORT
#include "nfs_maps.h"
#endif
#include "options.h"
#include "platform.h"
#include "quota_posix.h"
#include "signature.h"
#include "sqlitemem.h"
#include "sqlitevfs.h"
#include "statistics.h"
#include "tracer.h"
#include "util/pointer.h"
#include "util/posix.h"
#include "util/string.h"
#include "util_concurrency.h"
#include "uuid.h"
#include "wpad.h"

using namespace std;  // NOLINT


bool FileSystem::g_alive = false;
const char *FileSystem::kDefaultCacheBase = "/var/lib/cvmfs";
const char *FileSystem::kDefaultCacheMgrInstance = "default";


/**
 * A cache instance name is part of a bash parameter and can only contain
 * certain characters.
 */
bool FileSystem::CheckInstanceName(const std::string &instance) {
  if (instance.length() > 24)
    return false;
  sanitizer::CacheInstanceSanitizer instance_sanitizer;
  if (!instance_sanitizer.IsValid(instance)) {
    boot_error_ = "invalid instance name (" + instance + "), " +
                  "only characters a-z, A-Z, 0-9, _ are allowed";
    boot_status_ = loader::kFailCacheDir;
    return false;
  }
  return true;
}


/**
 * Not all possible combinations of cache flags / modes are valid.
 */
bool FileSystem::CheckPosixCacheSettings(
  const FileSystem::PosixCacheSettings &settings)
{
  if (settings.is_alien && settings.is_shared) {
    boot_error_ = "Failure: shared local disk cache and alien cache mutually "
                  "exclusive. Please turn off shared local disk cache.";
    boot_status_ = loader::kFailOptions;
    return false;
  }
  if (settings.is_alien && settings.is_managed) {
    boot_error_ = "Failure: quota management and alien cache mutually "
                  "exclusive. Please turn off quota limit.";
    boot_status_ = loader::kFailOptions;
    return false;
  }

  if (type_ == kFsLibrary) {
    if (settings.is_shared || settings.is_managed) {
      boot_error_ = "Failure: libcvmfs supports only unmanaged exclusive cache "
                    "or alien cache.";
      boot_status_ = loader::kFailOptions;
      return false;
    }
  }

  if (settings.cache_base_defined && settings.cache_dir_defined) {
    boot_error_ =
      "'CVMFS_CACHE_BASE' and 'CVMFS_CACHE_DIR' are mutually exclusive";
    boot_status_ = loader::kFailOptions;
    return false;
  }

  return true;
}


/**
 * Creation of state and manager classes.  The destructor should mirror this
 * method.
 */
FileSystem *FileSystem::Create(const FileSystem::FileSystemInfo &fs_info) {
  UniquePtr<FileSystem>
    file_system(new FileSystem(fs_info));

  file_system->SetupLogging();
  LogCvmfs(kLogCvmfs, kLogDebug, "Options:\n%s",
           file_system->options_mgr()->Dump().c_str());

  file_system->CreateStatistics();
  file_system->SetupSqlite();
  if (!file_system->DetermineNfsMode())
    return file_system.Release();
  if (!file_system->SetupWorkspace())
    return file_system.Release();

  // Redirect SQlite temp directory to workspace (global variable)
  unsigned length_tempdir = file_system->workspace_.length() + 1;
  sqlite3_temp_directory = static_cast<char *>(sqlite3_malloc(length_tempdir));
  snprintf(sqlite3_temp_directory,
           length_tempdir,
           "%s",
           file_system->workspace_.c_str());

  if (!file_system->TriageCacheMgr())
    return file_system.Release();
  file_system->SetupUuid();
  if (!file_system->SetupNfsMaps())
    return file_system.Release();
  bool retval = sqlite::RegisterVfsRdOnly(
    file_system->cache_mgr_,
    file_system->statistics_,
    sqlite::kVfsOptDefault);
  assert(retval);
  file_system->has_custom_sqlitevfs_ = true;

  ClientCtx::GetInstance();

  file_system->boot_status_ = loader::kFailOk;
  return file_system.Release();
}


void FileSystem::CreateStatistics() {
  statistics_ = new perf::Statistics();

  // Register the ShortString's static counters
  statistics_->Register("pathstring.n_instances", "Number of instances");
  statistics_->Register("pathstring.n_overflows", "Number of overflows");
  statistics_->Register("namestring.n_instances", "Number of instances");
  statistics_->Register("namestring.n_overflows", "Number of overflows");
  statistics_->Register("linkstring.n_instances", "Number of instances");
  statistics_->Register("linkstring.n_overflows", "Number of overflows");

  // Callback counters
  n_fs_open_ = statistics_->Register("cvmfs.n_fs_open",
                                     "Overall number of file open operations");
  n_fs_dir_open_ = statistics_->Register("cvmfs.n_fs_dir_open",
                   "Overall number of directory open operations");
  n_fs_lookup_ = statistics_->Register("cvmfs.n_fs_lookup",
                                       "Number of lookups");
  n_fs_lookup_negative_ = statistics_->Register("cvmfs.n_fs_lookup_negative",
                                                "Number of negative lookups");
  n_fs_stat_ = statistics_->Register("cvmfs.n_fs_stat", "Number of stats");
  n_fs_read_ = statistics_->Register("cvmfs.n_fs_read", "Number of files read");
  n_fs_readlink_ = statistics_->Register("cvmfs.n_fs_readlink",
                                         "Number of links read");
  n_fs_forget_ = statistics_->Register("cvmfs.n_fs_forget",
                                       "Number of inode forgets");
  n_io_error_ = statistics_->Register("cvmfs.n_io_error",
                                      "Number of I/O errors");
  no_open_files_ = statistics_->Register("cvmfs.no_open_files",
                                         "Number of currently opened files");
  no_open_dirs_ = statistics_->Register("cvmfs.no_open_dirs",
                  "Number of currently opened directories");
}


/**
 * Figure out mode of operation and cache directory.  Checking options for
 * sanity is in a separate method.
 */
FileSystem::PosixCacheSettings FileSystem::DeterminePosixCacheSettings(
  const string &instance
) {
  string optarg;
  PosixCacheSettings settings;

  if (options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_SHARED", instance),
                             &optarg)
      && options_mgr_->IsOn(optarg))
  {
    settings.is_shared = true;
  }
  if (options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_SERVER_MODE", instance),
                             &optarg)
      && options_mgr_->IsOn(optarg))
  {
    settings.avoid_rename = true;
  }

  if (type_ == kFsFuse)
    settings.quota_limit = kDefaultQuotaLimit;
  if (options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_QUOTA_LIMIT", instance),
                             &optarg))
  {
    settings.quota_limit = String2Int64(optarg) * 1024 * 1024;
  }
  if (settings.quota_limit > 0)
    settings.is_managed = true;

  settings.cache_path = kDefaultCacheBase;
  if (options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_BASE", instance),
                             &optarg))
  {
    settings.cache_path = MakeCanonicalPath(optarg);
    settings.cache_base_defined = true;
  }
  if (settings.is_shared) {
    settings.cache_path += "/shared";
  } else {
    settings.cache_path += "/" + name_;
  }

  // CheckCacheMode makes sure that CVMFS_CACHE_DIR and CVMFS_CACHE_BASE are
  // not set at the same time.
  if (options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_DIR", instance),
                             &optarg))
  {
    settings.cache_dir_defined = true;
    settings.cache_path = optarg;
  }
  if (options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_ALIEN", instance),
                             &optarg))
  {
    settings.is_alien = true;
    settings.cache_path = optarg;
  }
  if (settings.cache_path == workspace_fullpath_)
    settings.cache_path = ".";

  return settings;
}


bool FileSystem::DetermineNfsMode() {
  string optarg;

  if (options_mgr_->GetValue("CVMFS_NFS_SOURCE", &optarg) &&
      options_mgr_->IsOn(optarg))
  {
    nfs_mode_ |= kNfsMaps;
    if (options_mgr_->GetValue("CVMFS_NFS_SHARED", &optarg)) {
      nfs_mode_ |= kNfsMapsHa;
      nfs_maps_dir_ = optarg;
    }
  }

  if ((type_ == kFsLibrary) && (nfs_mode_ != kNfsNone)) {
    boot_error_ = "Failure: libcvmfs does not support NFS export.";
    boot_status_ = loader::kFailOptions;
    return false;
  }
  return true;
}


FileSystem::FileSystem(const FileSystem::FileSystemInfo &fs_info)
  : name_(fs_info.name)
  , exe_path_(fs_info.exe_path)
  , type_(fs_info.type)
  , options_mgr_(fs_info.options_mgr)
  , wait_workspace_(fs_info.wait_workspace)
  , foreground_(fs_info.foreground)
  , n_fs_open_(NULL)
  , n_fs_dir_open_(NULL)
  , n_fs_lookup_(NULL)
  , n_fs_lookup_negative_(NULL)
  , n_fs_stat_(NULL)
  , n_fs_read_(NULL)
  , n_fs_readlink_(NULL)
  , n_fs_forget_(NULL)
  , n_io_error_(NULL)
  , no_open_files_(NULL)
  , no_open_dirs_(NULL)
  , statistics_(NULL)
  , fd_workspace_lock_(-1)
  , found_previous_crash_(false)
  , nfs_mode_(kNfsNone)
  , cache_mgr_(NULL)
  , uuid_cache_(NULL)
  , has_nfs_maps_(false)
  , has_custom_sqlitevfs_(false)
{
  assert(!g_alive);
  g_alive = true;
  g_uid = geteuid();
  g_gid = getegid();

  string optarg;
  if (options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_SERVER_CACHE",
                                         kDefaultCacheMgrInstance),
                             &optarg)
      && options_mgr_->IsOn(optarg))
  {
    g_raw_symlinks = true;
  }
}


FileSystem::~FileSystem() {
  ClientCtx::CleanupInstance();

  if (has_custom_sqlitevfs_)
    sqlite::UnregisterVfsRdOnly();

  delete uuid_cache_;
#ifdef CVMFS_NFS_SUPPORT
  if (has_nfs_maps_)
    nfs_maps::Fini();
#endif
  delete cache_mgr_;

  if (sqlite3_temp_directory) {
    sqlite3_free(sqlite3_temp_directory);
    sqlite3_temp_directory = NULL;
  }

  if (!path_crash_guard_.empty())
    unlink(path_crash_guard_.c_str());
  if (!path_workspace_lock_.empty())
    unlink(path_workspace_lock_.c_str());
  if (fd_workspace_lock_ >= 0)
    UnlockFile(fd_workspace_lock_);

  sqlite3_shutdown();
  SqliteMemoryManager::CleanupInstance();

  delete statistics_;

  SetLogSyslogPrefix("");
  SetLogMicroSyslog("");
  SetLogDebugFile("");
  google::protobuf::ShutdownProtobufLibrary();
  g_alive = false;
}


bool FileSystem::LockWorkspace() {
  path_workspace_lock_ = workspace_ + "/lock." + name_;
  fd_workspace_lock_ = TryLockFile(path_workspace_lock_);
  if (fd_workspace_lock_ >= 0)
    return true;

  if (fd_workspace_lock_ == -1) {
    boot_error_ = "could not acquire workspace lock (" +
                 StringifyInt(errno) + ")";
    boot_status_ = loader::kFailCacheDir;
    return false;
  }

  assert(fd_workspace_lock_ == -2);

  if (!wait_workspace_) {
    boot_status_ = loader::kFailLockWorkspace;
    return false;
  }

  fd_workspace_lock_ = LockFile(path_workspace_lock_);
  if (fd_workspace_lock_ < 0) {
    boot_error_ = "could not acquire workspace lock (" +
                   StringifyInt(errno) + ")";
    boot_status_ = loader::kFailCacheDir;
    return false;
  }
  return true;
}


void FileSystem::LogSqliteError(
  void *user_data __attribute__((unused)),
  int sqlite_extended_error,
  const char *message)
{
  int log_dest = kLogDebug;
  int sqlite_error = sqlite_extended_error & 0xFF;
  switch (sqlite_error) {
    case SQLITE_INTERNAL:
    case SQLITE_PERM:
    case SQLITE_NOMEM:
    case SQLITE_IOERR:
    case SQLITE_CORRUPT:
    case SQLITE_FULL:
    case SQLITE_CANTOPEN:
    case SQLITE_MISUSE:
    case SQLITE_FORMAT:
    case SQLITE_NOTADB:
      log_dest |= kLogSyslogErr;
      break;
    case SQLITE_WARNING:
    case SQLITE_NOTICE:
    default:
      break;
  }
  LogCvmfs(kLogCvmfs, log_dest, "SQlite3: %s (%d)",
           message, sqlite_extended_error);
}


/**
 * Creates the cache parameter for a specific instance of the cache.  Injects
 * the instance name such that CVMFS_CACHE_FOO_BAR becomes
 * CVMFS_CACHE_<INSTANCE>_FOO_BAR
 */
string FileSystem::MkCacheParm(
  const string &generic_parameter,
  const string &instance)
{
  assert(HasPrefix(generic_parameter, "CVMFS_CACHE_", false));

  if (instance == kDefaultCacheMgrInstance) {
    // Compatibility parameter names
    if ((generic_parameter == "CVMFS_CACHE_SHARED") &&
        !options_mgr_->IsDefined(generic_parameter))
    {
      return "CVMFS_SHARED_CACHE";
    }
    if ((generic_parameter == "CVMFS_CACHE_ALIEN") &&
        !options_mgr_->IsDefined(generic_parameter))
    {
      return "CVMFS_ALIEN_CACHE";
    }
    if ((generic_parameter == "CVMFS_CACHE_SERVER_MODE") &&
        !options_mgr_->IsDefined(generic_parameter))
    {
      return "CVMFS_SERVER_CACHE_MODE";
    }
    if ((generic_parameter == "CVMFS_CACHE_QUOTA_LIMIT") &&
        !options_mgr_->IsDefined(generic_parameter))
    {
      return "CVMFS_QUOTA_LIMIT";
    }
    return generic_parameter;
  }

  return "CVMFS_CACHE_" + instance + "_" + generic_parameter.substr(12);
}


void FileSystem::ResetErrorCounters() {
  n_io_error_->Set(0);
}


/**
 * Can be recursive for the tiered cache manager.
 */
CacheManager *FileSystem::SetupCacheMgr(const string &instance) {
  if (constructed_instances_.find(instance) != constructed_instances_.end()) {
    boot_error_ = "circular cache definition: " + instance;
    boot_status_ = loader::kFailCacheDir;
    return NULL;
  }
  constructed_instances_.insert(instance);

  LogCvmfs(kLogCvmfs, kLogDebug, "setting up cache manager instance %s",
           instance.c_str());
  string instance_type;
  if (instance == kDefaultCacheMgrInstance) {
    instance_type = "posix";
  } else {
    options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_TYPE", instance),
                           &instance_type);
  }
  if (instance_type == "posix") {
    return SetupPosixCacheMgr(instance);
  } else if (instance_type == "ram") {
    return SetupRamCacheMgr(instance);
  } else if (instance_type == "tiered") {
    return SetupTieredCacheMgr(instance);
  } else if (instance_type == "external") {
    return SetupExternalCacheMgr(instance);
  } else {
    boot_error_ = "invalid cache manager type for '" + instance +  "':" +
      instance_type;
    boot_status_ = loader::kFailCacheDir;
    return NULL;
  }
}


CacheManager *FileSystem::SetupExternalCacheMgr(const string &instance) {
  string optarg;
  unsigned nfiles = kDefaultNfiles;
  if (options_mgr_->GetValue("CVMFS_NFILES", &optarg))
    nfiles = String2Uint64(optarg);
  vector<string> cmd_line;
  if (options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_CMDLINE", instance),
      &optarg))
  {
    cmd_line = SplitString(optarg, ',');
  }

  if (!options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_LOCATOR", instance),
      &optarg))
  {
    boot_error_ = MkCacheParm("CVMFS_CACHE_LOCATOR", instance) + " missing";
    boot_status_ = loader::kFailCacheDir;
    return NULL;
  }

  UniquePtr<ExternalCacheManager::PluginHandle> plugin_handle(
    ExternalCacheManager::CreatePlugin(optarg, cmd_line));
  if (!plugin_handle->IsValid()) {
    boot_error_ = plugin_handle->error_msg();
    boot_status_ = loader::kFailCacheDir;
    return NULL;
  }
  ExternalCacheManager *cache_mgr = ExternalCacheManager::Create(
    plugin_handle->fd_connection(), nfiles, name_ + ":" + instance);
  if (cache_mgr == NULL) {
    boot_error_ = "failed to create external cache manager for " + instance;
    boot_status_ = loader::kFailCacheDir;
    return NULL;
  }
  cache_mgr->AcquireQuotaManager(ExternalQuotaManager::Create(cache_mgr));
  return cache_mgr;
}


CacheManager *FileSystem::SetupPosixCacheMgr(const string &instance) {
  PosixCacheSettings settings = DeterminePosixCacheSettings(instance);
  if (!CheckPosixCacheSettings(settings))
    return NULL;

  UniquePtr<PosixCacheManager> cache_mgr(PosixCacheManager::Create(
    settings.cache_path,
    settings.is_alien,
    settings.avoid_rename));
  if (!cache_mgr.IsValid()) {
    boot_error_ = "Failed to setup posix cache '" + instance + "' in " +
                  settings.cache_path + ": " + strerror(errno);
    boot_status_ = loader::kFailCacheDir;
    return NULL;
  }

  // Sentinel file for future use
  // Might be a read-only cache
  const bool ignore_failure = settings.is_alien;
  CreateFile(settings.cache_path + "/.cvmfscache", 0600, ignore_failure);

  if (settings.is_managed) {
    if (!SetupPosixQuotaMgr(settings, cache_mgr))
      return NULL;
  }
  return cache_mgr.Release();
}


CacheManager *FileSystem::SetupRamCacheMgr(const string &instance) {
  string optarg;
  unsigned nfiles = kDefaultNfiles;
  if (options_mgr_->GetValue("CVMFS_NFILES", &optarg)) {
    nfiles = String2Uint64(optarg);
  }
  uint64_t sz_cache_bytes;
  if (options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_SIZE", instance),
                             &optarg))
  {
    if (HasSuffix(optarg, "%", false)) {
      sz_cache_bytes = platform_memsize() * String2Uint64(optarg)/100;
    } else {
      sz_cache_bytes = String2Uint64(optarg) * 1024 * 1024;
    }
  } else {
    sz_cache_bytes = platform_memsize() >> 5;  // ~3%
  }
  MemoryKvStore::MemoryAllocator alloc = MemoryKvStore::kMallocHeap;
  if (options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_MALLOC", instance),
                             &optarg))
  {
    if (optarg == "libc") {
      alloc = MemoryKvStore::kMallocLibc;
    } else if (optarg == "heap") {
      alloc = MemoryKvStore::kMallocHeap;
    } else {
      boot_error_ = "Failure: unknown malloc " +
                    MkCacheParm("CVMFS_CACHE_MALLOC", instance) + "=" + optarg;
      boot_status_ = loader::kFailOptions;
      return NULL;
    }
  }
  sz_cache_bytes = RoundUp8(std::max(static_cast<uint64_t>(200 * 1024 * 1024),
                                     sz_cache_bytes));
  RamCacheManager *cache_mgr = new RamCacheManager(
        sz_cache_bytes,
        nfiles,
        alloc,
        perf::StatisticsTemplate("cache." + instance, statistics_));
  if (cache_mgr == NULL) {
    boot_error_ = "failed to create ram cache manager for " + instance;
    boot_status_ = loader::kFailCacheDir;
    return NULL;
  }
  cache_mgr->AcquireQuotaManager(new NoopQuotaManager());
  return cache_mgr;
}


CacheManager *FileSystem::SetupTieredCacheMgr(const string &instance) {
  string optarg;
  if (!options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_UPPER", instance),
                              &optarg))
  {
    boot_error_ = MkCacheParm("CVMFS_CACHE_UPPER", instance) + " missing";
    boot_status_ = loader::kFailOptions;
    return NULL;
  }
  UniquePtr<CacheManager> upper(SetupCacheMgr(optarg));
  if (!upper.IsValid())
    return NULL;

  if (!options_mgr_->GetValue(MkCacheParm("CVMFS_CACHE_LOWER", instance),
                              &optarg))
  {
    boot_error_ = MkCacheParm("CVMFS_CACHE_LOWER", instance) + " missing";
    boot_status_ = loader::kFailOptions;
    return NULL;
  }
  UniquePtr<CacheManager> lower(SetupCacheMgr(optarg));
  if (!lower.IsValid())
    return NULL;

  CacheManager *tiered =
    TieredCacheManager::Create(upper.Release(), lower.Release());
  if (tiered == NULL) {
    boot_error_ = "Failed to setup tiered cache manager " + instance;
    boot_status_ = loader::kFailCacheDir;
    return NULL;
  }
  return tiered;
}


bool FileSystem::SetupCrashGuard() {
  path_crash_guard_ = workspace_ + "/running." + name_;
  platform_stat64 info;
  int retval = platform_stat(path_crash_guard_.c_str(), &info);
  if (retval == 0) {
    found_previous_crash_ = true;
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
             "looks like cvmfs has been crashed previously");
  }
  retval = open(path_crash_guard_.c_str(), O_RDONLY | O_CREAT, 0600);
  if (retval < 0) {
    boot_error_ = "could not open running sentinel (" +
                  StringifyInt(errno) + ")";
    boot_status_ = loader::kFailCacheDir;
    return false;
  }
  close(retval);
  return true;
}


bool FileSystem::SetupCwd() {
  if (type_ == kFsFuse) {
    // Try to jump to workspace / cache directory.  This tests, if it is
    // accessible and it brings speed later on.
    int retval = chdir(workspace_.c_str());
    if (retval != 0) {
      boot_error_ = "workspace " + workspace_ + " is unavailable";
      boot_status_ = loader::kFailCacheDir;
      return false;
    }
    workspace_ = ".";
    return true;
  }

  // Note: as of version 2.4 support for CVMFS_CWD_CACHE is dropped due to
  // disproportionate large complexity to configure correctly.  This affects
  // only libcvmfs, mostly the legacy part.
  // string optarg;
  // if (options_mgr_->GetValue("CVMFS_CWD_CACHE", &optarg) &&
  //    options_mgr_->IsOn(optarg))
  // {
  //  ...
  // }
  return true;
}


void FileSystem::SetupLogging() {
  string optarg;
  if (options_mgr_->GetValue("CVMFS_SYSLOG_LEVEL", &optarg))
    SetLogSyslogLevel(String2Uint64(optarg));
  if (options_mgr_->GetValue("CVMFS_SYSLOG_FACILITY", &optarg))
    SetLogSyslogFacility(String2Int64(optarg));
  if (options_mgr_->GetValue("CVMFS_USYSLOG", &optarg))
    SetLogMicroSyslog(optarg);
  if (options_mgr_->GetValue("CVMFS_DEBUGLOG", &optarg))
    SetLogDebugFile(optarg);
  if (options_mgr_->GetValue("CVMFS_SYSLOG_PREFIX", &optarg)) {
    SetLogSyslogPrefix(optarg);
  } else {
    SetLogSyslogPrefix(name_);
  }
}


bool FileSystem::SetupNfsMaps() {
#ifdef CVMFS_NFS_SUPPORT
  if (!IsHaNfsSource())
    nfs_maps_dir_ = workspace_;

  string no_nfs_sentinel;
  if (cache_mgr_->id() == kPosixCacheManager) {
    PosixCacheManager *posix_cache_mgr =
        reinterpret_cast<PosixCacheManager *>(cache_mgr_);
    no_nfs_sentinel = posix_cache_mgr->cache_path() + "/no_nfs_maps." + name_;
    if (!IsNfsSource()) {
      // Might be a read-only cache
      const bool ignore_failure = posix_cache_mgr->alien_cache();
      CreateFile(no_nfs_sentinel, 0600, ignore_failure);
      return true;
    }
  } else {
    if (IsNfsSource()) {
      boot_error_ = "NFS source only works with POSIX cache manager.";
      boot_status_ = loader::kFailNfsMaps;
      return false;
    }
    return true;
  }

  assert(cache_mgr_->id() == kPosixCacheManager);
  assert(IsNfsSource());
  if (!no_nfs_sentinel.empty() && FileExists(no_nfs_sentinel)) {
    boot_error_ = "Cache was used without NFS maps before. "
                  "It has to be wiped out.";
    boot_status_ = loader::kFailNfsMaps;
    return false;
  }

  // nfs maps need to be protected by workspace lock
  PosixCacheManager *posix_cache_mgr =
        reinterpret_cast<PosixCacheManager *>(cache_mgr_);
  assert(posix_cache_mgr->cache_path() == workspace_);

  string inode_cache_dir = nfs_maps_dir_ + "/nfs_maps." + name_;
  if (!MkdirDeep(inode_cache_dir, 0700)) {
    boot_error_ = "Failed to initialize NFS maps";
    boot_status_ = loader::kFailNfsMaps;
    return false;
  }

  // TODO(jblomer): make this a manager class
  bool retval =
    nfs_maps::Init(inode_cache_dir,
                   catalog::ClientCatalogManager::kInodeOffset + 1,
                   found_previous_crash_,
                   IsHaNfsSource());
  if (!retval) {
    boot_error_ = "Failed to initialize NFS maps";
    boot_status_ = loader::kFailNfsMaps;
    return false;
  }

  return has_nfs_maps_ = true;
#else
  return true;
#endif
}


bool FileSystem::SetupPosixQuotaMgr(
  const FileSystem::PosixCacheSettings &settings,
  CacheManager *cache_mgr
) {
  assert(settings.quota_limit >= 0);
  int64_t quota_threshold = settings.quota_limit / 2;
  PosixQuotaManager *quota_mgr;

  if (settings.is_shared) {
    quota_mgr = PosixQuotaManager::CreateShared(
                  exe_path_,
                  settings.cache_path,
                  settings.quota_limit,
                  quota_threshold,
                  foreground_);
    if (quota_mgr == NULL) {
      boot_error_ = "Failed to initialize shared lru cache";
      boot_status_ = loader::kFailQuota;
      return false;
    }
  } else {
    // Cache database should to be protected by workspace lock
    assert(workspace_ == settings.cache_path);
    quota_mgr = PosixQuotaManager::Create(
                  settings.cache_path,
                  settings.quota_limit,
                  quota_threshold,
                  found_previous_crash_);
    if (quota_mgr == NULL) {
      boot_error_ = "Failed to initialize lru cache";
      boot_status_ = loader::kFailQuota;
      return false;
    }
  }

  if (quota_mgr->GetSize() > quota_mgr->GetCapacity()) {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslog,
             "cache is already beyond quota size "
             "(size: %" PRId64 ", capacity: %" PRId64 "), cleaning up",
             quota_mgr->GetSize(), quota_mgr->GetCapacity());
    if (!quota_mgr->Cleanup(quota_threshold)) {
      delete quota_mgr;
      boot_error_ = "Failed to clean up cache";
      boot_status_ = loader::kFailQuota;
      return false;
    }
  }

  int retval = cache_mgr->AcquireQuotaManager(quota_mgr);
  assert(retval);
  LogCvmfs(kLogCvmfs, kLogDebug,
           "CernVM-FS: quota initialized, current size %luMB",
           quota_mgr->GetSize() / (1024 * 1024));
  return true;
}


void FileSystem::SetupSqlite() {
  // Make sure SQlite starts clean after initialization
  sqlite3_shutdown();

  int retval;
  retval = sqlite3_config(SQLITE_CONFIG_LOG, FileSystem::LogSqliteError, NULL);
  assert(retval == SQLITE_OK);
  retval = sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
  assert(retval == SQLITE_OK);
  SqliteMemoryManager::GetInstance()->AssignGlobalArenas();

  // Disable SQlite3 file locking
  retval = sqlite3_vfs_register(sqlite3_vfs_find("unix-none"), 1);
  assert(retval == SQLITE_OK);
}


bool FileSystem::SetupWorkspace() {
  string optarg;
  // This is very similar to "determine cache dir".  It's for backward
  // compatibility with classic cache configuration where there was no
  // distinction between workspace and cache.
  // Complicated cache configurations should explicitly set CVMFS_WORKSPACE.
  workspace_ = kDefaultCacheBase;
  if (options_mgr_->GetValue("CVMFS_CACHE_BASE", &optarg))
    workspace_ = MakeCanonicalPath(optarg);
  if (options_mgr_->GetValue("CVMFS_SHARED_CACHE", &optarg) &&
      options_mgr_->IsOn(optarg))
  {
    workspace_ += "/shared";
  } else {
    workspace_ += "/" + name_;
  }
  if (options_mgr_->GetValue("CVMFS_CACHE_DIR", &optarg)) {
    if (options_mgr_->IsDefined("CVMFS_CACHE_BASE")) {
      boot_error_ =
        "'CVMFS_CACHE_BASE' and 'CVMFS_CACHE_DIR' are mutually exclusive";
      boot_status_ = loader::kFailOptions;
      return false;
    }
    workspace_ = optarg;
  }
  if (options_mgr_->GetValue("CVMFS_WORKSPACE", &optarg))
    workspace_ = optarg;
  workspace_fullpath_ = workspace_;

  // If workspace and alien cache are the same directory, we need to open
  // permission now to 0770 to avoid a race when fixing it later
  const int mode = 0770;
  if (!MkdirDeep(workspace_, mode, false)) {
    boot_error_ = "cannot create workspace directory " + workspace_;
    boot_status_ = loader::kFailCacheDir;
    return false;
  }

  if (!LockWorkspace())
    return false;
  if (!SetupCwd())
    return false;
  if (!SetupCrashGuard())
    return false;

  return true;
}


void FileSystem::SetupUuid() {
  uuid_cache_ = cvmfs::Uuid::Create(workspace_ + "/uuid");
  if (uuid_cache_ == NULL) {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
             "failed to load/store %s/uuid", workspace_.c_str());
    uuid_cache_ = cvmfs::Uuid::Create("");
    assert(uuid_cache_ != NULL);
  }
}


/**
 * Required by CernVM: the fuse module needs to free r/w file descriptor to the
 * cache in order to properly unravel the file system stack on shutdown.
 */
void FileSystem::TearDown2ReadOnly() {
  if ((cache_mgr_ != NULL) && (cache_mgr_->id() == kPosixCacheManager)) {
    PosixCacheManager *posix_cache_mgr =
      reinterpret_cast<PosixCacheManager *>(cache_mgr_);
    posix_cache_mgr->TearDown2ReadOnly();
  }

  unlink(path_crash_guard_.c_str());
  LogCvmfs(kLogCache, kLogSyslog, "switch to read-only cache mode");
  SetLogMicroSyslog("");
}


bool FileSystem::TriageCacheMgr() {
  cache_mgr_instance_ = kDefaultCacheMgrInstance;
  string instance;
  if (options_mgr_->GetValue("CVMFS_CACHE_PRIMARY", &instance) &&
      !instance.empty())
  {
    if (!CheckInstanceName(instance))
      return false;
    cache_mgr_instance_ = instance;
  }

  cache_mgr_ = SetupCacheMgr(cache_mgr_instance_);
  return cache_mgr_ != NULL;
}


//------------------------------------------------------------------------------


const char *MountPoint::kDefaultAuthzSearchPath = "/usr/libexec/cvmfs/authz";
const char *MountPoint::kDefaultBlacklist = "/etc/cvmfs/blacklist";

bool MountPoint::CheckBlacklists() {
  string blacklist;
  if (!options_mgr_->GetValue("CVMFS_BLACKLIST", &blacklist))
    blacklist = kDefaultBlacklist;
  if (FileExists(blacklist)) {
    const bool append = false;
    if (!signature_mgr_->LoadBlacklist(blacklist, append)) {
      boot_error_ = "failed to load blacklist " + blacklist;
      boot_status_ = loader::kFailSignature;
      return false;
    }
  }

  string config_repository_path;
  if (options_mgr_->HasConfigRepository(fqrn_, &config_repository_path)
      && FileExists(config_repository_path + "blacklist"))
  {
    const bool append = true;
    if (!signature_mgr_->LoadBlacklist(config_repository_path + "blacklist",
                                       append))
    {
      boot_error_ = "failed to load blacklist from config repository";
      boot_status_ = loader::kFailSignature;
      return false;
    }
  }

  return true;
}


/**
 * The option_mgr parameter can be NULL, in which case the global option manager
 * from the file system is used.
 */
MountPoint *MountPoint::Create(
  const string &fqrn,
  FileSystem *file_system,
  OptionsManager *options_mgr)
{
  if (options_mgr == NULL)
    options_mgr = file_system->options_mgr();
  UniquePtr<MountPoint> mountpoint(new MountPoint(
    fqrn, file_system, options_mgr));

  // At this point, we have a repository name, the type (fuse or library) and
  // an options manager (which can be the same than the FileSystem's one).

  mountpoint->CreateStatistics();
  mountpoint->CreateAuthz();
  mountpoint->backoff_throttle_ = new BackoffThrottle();

  if (!mountpoint->CreateSignatureManager() || !mountpoint->CheckBlacklists())
    return mountpoint.Release();
  if (!mountpoint->CreateDownloadManagers())
    return mountpoint.Release();
  mountpoint->CreateFetchers();
  if (!mountpoint->CreateCatalogManager())
    return mountpoint.Release();
  if (!mountpoint->CreateTracer())
    return mountpoint.Release();

  mountpoint->ReEvaluateAuthz();
  mountpoint->CreateTables();
  mountpoint->SetupBehavior();

  mountpoint->boot_status_ = loader::kFailOk;
  return mountpoint.Release();
}


void MountPoint::CreateAuthz() {
  string optarg;
  string authz_helper;
  if (options_mgr_->GetValue("CVMFS_AUTHZ_HELPER", &optarg))
    authz_helper = optarg;
  string authz_search_path(kDefaultAuthzSearchPath);
  if (options_mgr_->GetValue("CVMFS_AUTHZ_SEARCH_PATH", &optarg))
    authz_search_path = optarg;

  authz_fetcher_ = new AuthzExternalFetcher(
    fqrn_,
    authz_helper,
    authz_search_path,
    options_mgr_);
  assert(authz_fetcher_ != NULL);

  authz_session_mgr_ = AuthzSessionManager::Create(
    authz_fetcher_,
    statistics_);
  assert(authz_session_mgr_ != NULL);

  authz_attachment_ = new AuthzAttachment(authz_session_mgr_);
  assert(authz_attachment_ != NULL);
}


bool MountPoint::CreateCatalogManager() {
  string optarg;

  catalog_mgr_ = new catalog::ClientCatalogManager(
    fqrn_, fetcher_, signature_mgr_, statistics_);

  SetupInodeAnnotation();
  if (!SetupOwnerMaps())
    return false;
  shash::Any root_hash;
  if (!DetermineRootHash(&root_hash))
    return false;

  bool retval;
  if (root_hash.IsNull()) {
    retval = catalog_mgr_->Init();
  } else {
    fixed_catalog_ = true;
    bool alt_root_path =
      options_mgr_->GetValue("CVMFS_ALT_ROOT_PATH", &optarg) &&
      options_mgr_->IsOn(optarg);
    retval = catalog_mgr_->InitFixed(root_hash, alt_root_path);
  }
  if (!retval) {
    boot_error_ = "Failed to initialize root file catalog";
    boot_status_ = loader::kFailCatalog;
    return false;
  }

  if (options_mgr_->GetValue("CVMFS_AUTO_UPDATE", &optarg) &&
      !options_mgr_->IsOn(optarg))
  {
    fixed_catalog_ = true;
  }

  if (catalog_mgr_->volatile_flag()) {
    LogCvmfs(kLogCvmfs, kLogDebug, "content of repository flagged as VOLATILE");
  }

  return true;
}


bool MountPoint::CreateDownloadManagers() {
  string optarg;
  download_mgr_ = new download::DownloadManager();
  const bool use_system_proxy = false;
  download_mgr_->Init(kDefaultNumConnections, use_system_proxy,
                      perf::StatisticsTemplate("download", statistics_));
  download_mgr_->SetCredentialsAttachment(authz_attachment_);

  if (options_mgr_->GetValue("CVMFS_SERVER_URL", &optarg)) {
    download_mgr_->SetHostChain(ReplaceHosts(optarg));
  }

  SetupDnsTuning(download_mgr_);
  SetupHttpTuning();

  string forced_proxy_template;
  if (options_mgr_->GetValue("CVMFS_PROXY_TEMPLATE", &optarg))
    forced_proxy_template = optarg;
  download_mgr_->SetProxyTemplates(file_system_->uuid_cache()->uuid(),
                                   forced_proxy_template);

  string proxies;
  if (options_mgr_->GetValue("CVMFS_HTTP_PROXY", &optarg))
    proxies = optarg;
  proxies = download::ResolveProxyDescription(proxies, download_mgr_);
  if (proxies == "") {
    boot_error_ = "failed to discover HTTP proxy servers";
    boot_status_ = loader::kFailWpad;
    return false;
  }
  string fallback_proxies;
  if (options_mgr_->GetValue("CVMFS_FALLBACK_PROXY", &optarg))
    fallback_proxies = optarg;
  download_mgr_->SetProxyChain(proxies, fallback_proxies,
                               download::DownloadManager::kSetProxyBoth);

  if (options_mgr_->GetValue("CVMFS_USE_GEOAPI", &optarg) &&
      options_mgr_->IsOn(optarg))
  {
    download_mgr_->ProbeGeo();
  }

  return SetupExternalDownloadMgr();
}


void MountPoint::CreateFetchers() {
  fetcher_ = new cvmfs::Fetcher(
    file_system_->cache_mgr(),
    download_mgr_,
    backoff_throttle_,
    perf::StatisticsTemplate("fetch", statistics_));

  const bool is_external_data = true;
  external_fetcher_ = new cvmfs::Fetcher(
    file_system_->cache_mgr(),
    external_download_mgr_,
    backoff_throttle_,
    perf::StatisticsTemplate("fetch-external", statistics_),
    is_external_data);
}


bool MountPoint::CreateSignatureManager() {
  string optarg;
  signature_mgr_ = new signature::SignatureManager();
  signature_mgr_->Init();

  string public_keys;
  if (options_mgr_->GetValue("CVMFS_PUBLIC_KEY", &optarg)) {
    public_keys = optarg;
  } else if (options_mgr_->GetValue("CVMFS_KEYS_DIR", &optarg)) {
    // Collect .pub files from CVMFS_KEYS_DIR
    public_keys = JoinStrings(FindFiles(optarg, ".pub"), ":");
  } else {
    public_keys = JoinStrings(FindFiles("/etc/cvmfs/keys", ".pub"), ":");
  }

  if (!signature_mgr_->LoadPublicRsaKeys(public_keys)) {
    boot_error_ = "failed to load public key(s)";
    boot_status_ = loader::kFailSignature;
    return false;
  }
  LogCvmfs(kLogCvmfs, kLogDebug, "CernVM-FS: using public key(s) %s",
           public_keys.c_str());

  if (options_mgr_->GetValue("CVMFS_TRUSTED_CERTS", &optarg)) {
    if (!signature_mgr_->LoadTrustedCaCrl(optarg)) {
      boot_error_ = "failed to load trusted certificates";
      boot_status_ = loader::kFailSignature;
      return false;
    }
  }

  return true;
}


void MountPoint::CreateStatistics() {
  statistics_ = file_system_->statistics()->Fork();
  if (file_system_->type() != FileSystem::kFsFuse)
    return;

  // TODO(jblomer): this should be registered by the tracker
  statistics_->Register("inode_tracker.n_insert",
                        "overall number of accessed inodes");
  statistics_->Register("inode_tracker.n_remove",
                        "overall number of evicted inodes");
  statistics_->Register("inode_tracker.no_reference",
                        "currently active inodes");
  statistics_->Register("inode_tracker.n_hit_inode",
                        "overall number of inode lookups");
  statistics_->Register("inode_tracker.n_hit_path",
                        "overall number of successful path lookups");
  statistics_->Register("inode_tracker.n_miss_path",
                        "overall number of unsuccessful path lookups");
}


void MountPoint::CreateTables() {
  if (file_system_->type() != FileSystem::kFsFuse) {
    // Libcvmfs simplified tables
    md5path_cache_ = new lru::Md5PathCache(kLibPathCacheSize, statistics_);
    simple_chunk_tables_ = new SimpleChunkTables();
    return;
  }

  chunk_tables_ = new ChunkTables();

  string optarg;
  uint64_t mem_cache_size = kDefaultMemcacheSize;
  if (options_mgr_->GetValue("CVMFS_MEMCACHE_SIZE", &optarg))
    mem_cache_size = String2Uint64(optarg) * 1024 * 1024;

  const double memcache_unit_size =
    (static_cast<double>(kInodeCacheFactor) * lru::Md5PathCache::GetEntrySize())
    + lru::InodeCache::GetEntrySize() + lru::PathCache::GetEntrySize();
  const unsigned memcache_num_units =
    mem_cache_size / static_cast<unsigned>(memcache_unit_size);
  // Number of cache entries must be a multiple of 64
  const unsigned mask_64 = ~((1 << 6) - 1);
  inode_cache_ = new lru::InodeCache(memcache_num_units & mask_64, statistics_);
  path_cache_ = new lru::PathCache(memcache_num_units & mask_64, statistics_);
  md5path_cache_ = new lru::Md5PathCache((memcache_num_units * 7) & mask_64,
                                         statistics_);

  inode_tracker_ = new glue::InodeTracker();
}


bool MountPoint::CreateTracer() {
  string optarg;
  tracer_ = new Tracer();
  if (options_mgr_->GetValue("CVMFS_TRACEFILE", &optarg)) {
    if (file_system_->type() != FileSystem::kFsFuse) {
      boot_error_ = "tracer is only supported in the fuse module";
      boot_status_ = loader::kFailOptions;
      return false;
    }
    tracer_->Activate(kTracerBufferSize, kTracerFlushThreshold, optarg);
  }
  return true;
}


bool MountPoint::DetermineRootHash(shash::Any *root_hash) {
  string optarg;
  if (options_mgr_->GetValue("CVMFS_ROOT_HASH", &optarg)) {
    *root_hash = MkFromHexPtr(shash::HexPtr(optarg), shash::kSuffixCatalog);
    return true;
  }

  if (!options_mgr_->IsDefined("CVMFS_REPOSITORY_TAG") &&
      !options_mgr_->IsDefined("CVMFS_REPOSITORY_DATE"))
  {
    root_hash->SetNull();
    return true;
  }

  string history_path;
  if (!FetchHistory(&history_path))
    return false;
  UnlinkGuard history_file(history_path);
  UniquePtr<history::History> tag_db(
    history::SqliteHistory::Open(history_path));
  if (!tag_db) {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslog,
             "failed to open history database (%s)", history_path.c_str());
    boot_error_ = "failed to open history database";
    boot_status_ = loader::kFailHistory;
    return false;
  }

  history::History::Tag tag;
  bool retval;
  if (!options_mgr_->GetValue("CVMFS_REPOSITORY_TAG", &repository_tag_)) {
    string repository_date;
    // options_mgr_->IsDefined("CVMFS_REPOSITORY_DATE") must be true
    options_mgr_->GetValue("CVMFS_REPOSITORY_DATE", &repository_date);
    time_t repository_utctime = IsoTimestamp2UtcTime(repository_date);
    if (repository_utctime == 0) {
      boot_error_ = "invalid timestamp in CVMFS_REPOSITORY_DATE: " +
                    repository_date + ". Use YYYY-MM-DDTHH:MM:SSZ";
      boot_status_ = loader::kFailHistory;
      return false;
    }
    retval = tag_db->GetByDate(repository_utctime, &tag);
    if (!retval) {
      boot_error_ = "no repository state as early as utc timestamp " +
                     StringifyTime(repository_utctime, true);
      boot_status_ = loader::kFailHistory;
      return false;
    }
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslog,
             "time stamp %s UTC resolved to tag '%s'",
             StringifyTime(repository_utctime, true).c_str(),
             tag.name.c_str());
    repository_tag_ = tag.name;
  } else {
    retval = tag_db->GetByName(repository_tag_, &tag);
    if (!retval) {
      boot_error_ = "no such tag: " + repository_tag_;
      boot_status_ = loader::kFailHistory;
      return false;
    }
  }
  LogCvmfs(kLogCvmfs, kLogDebug, "mounting tag %s", tag.name.c_str());

  *root_hash = tag.root_hash;
  return true;
}


bool MountPoint::FetchHistory(std::string *history_path) {
  manifest::Failures retval_mf;
  manifest::ManifestEnsemble ensemble;
  retval_mf = manifest::Fetch("", fqrn_, 0, NULL, signature_mgr_, download_mgr_,
                              &ensemble);
  if (retval_mf != manifest::kFailOk) {
    boot_error_ = "Failed to fetch manifest";
    boot_status_ = loader::kFailHistory;
    return false;
  }
  shash::Any history_hash = ensemble.manifest->history();
  if (history_hash.IsNull()) {
    boot_error_ = "No history";
    boot_status_ = loader::kFailHistory;
    return false;
  }

  int fd = fetcher_->Fetch(
    history_hash,
    CacheManager::kSizeUnknown,
    "tag database for " + fqrn_,
    zlib::kZlibDefault,
    CacheManager::kTypeRegular);
  if (fd < 0) {
    boot_error_ = "failed to download history: " + StringifyInt(-fd);
    boot_status_ = loader::kFailHistory;
    return false;
  }
  // We have the custom sqlite vfs driver installed
  *history_path = "@" + StringifyInt(fd);
  return true;
}


unsigned MountPoint::GetEffectiveTtlSec() {
  unsigned max_ttl;
  {
    MutexLockGuard lock_guard(lock_max_ttl_);
    max_ttl = max_ttl_sec_;
  }
  const unsigned catalog_ttl_sec = catalog_mgr_->GetTTL();

  return max_ttl ? std::min(max_ttl, catalog_ttl_sec) : catalog_ttl_sec;
}


unsigned MountPoint::GetMaxTtlMn() {
  MutexLockGuard lock_guard(lock_max_ttl_);
  return max_ttl_sec_ / 60;
}


MountPoint::MountPoint(
  const string &fqrn,
  FileSystem *file_system,
  OptionsManager *options_mgr)
  : fqrn_(fqrn)
  , uuid_(cvmfs::Uuid::Create(""))
  , file_system_(file_system)
  , options_mgr_(options_mgr)
  , statistics_(NULL)
  , authz_fetcher_(NULL)
  , authz_session_mgr_(NULL)
  , authz_attachment_(NULL)
  , backoff_throttle_(NULL)
  , signature_mgr_(NULL)
  , download_mgr_(NULL)
  , external_download_mgr_(NULL)
  , fetcher_(NULL)
  , external_fetcher_(NULL)
  , inode_annotation_(NULL)
  , catalog_mgr_(NULL)
  , chunk_tables_(NULL)
  , simple_chunk_tables_(NULL)
  , inode_cache_(NULL)
  , path_cache_(NULL)
  , md5path_cache_(NULL)
  , tracer_(NULL)
  , inode_tracker_(NULL)
  , max_ttl_sec_(kDefaultMaxTtlSec)
  , kcache_timeout_sec_(static_cast<double>(kDefaultKCacheTtlSec))
  , fixed_catalog_(false)
  , hide_magic_xattrs_(false)
  , has_membership_req_(false)
{
  int retval = pthread_mutex_init(&lock_max_ttl_, NULL);
  assert(retval == 0);
}


MountPoint::~MountPoint() {
  pthread_mutex_destroy(&lock_max_ttl_);

  delete inode_tracker_;
  delete tracer_;
  delete md5path_cache_;
  delete path_cache_;
  delete inode_cache_;
  delete simple_chunk_tables_;
  delete chunk_tables_;

  delete catalog_mgr_;
  delete inode_annotation_;
  delete external_fetcher_;
  delete fetcher_;
  if (external_download_mgr_ != NULL) {
    external_download_mgr_->Fini();
    delete external_download_mgr_;
  }
  if (download_mgr_ != NULL) {
    download_mgr_->Fini();
    delete download_mgr_;
  }
  if (signature_mgr_ != NULL) {
    signature_mgr_->Fini();
    delete signature_mgr_;
  }

  delete backoff_throttle_;
  delete authz_attachment_;
  delete authz_session_mgr_;
  delete authz_fetcher_;
  delete statistics_;
  delete uuid_;
}


void MountPoint::ReEvaluateAuthz() {
  has_membership_req_ = catalog_mgr_->GetVOMSAuthz(&membership_req_);
  authz_attachment_->set_membership(membership_req_);
}


string MountPoint::ReplaceHosts(string hosts) {
  vector<string> tokens = SplitString(fqrn_, '.');
  const string org = tokens[0];
  hosts = ReplaceAll(hosts, "@org@", org);
  hosts = ReplaceAll(hosts, "@fqrn@", fqrn_);
  return hosts;
}


void MountPoint::SetMaxTtlMn(unsigned value_minutes) {
  MutexLockGuard lock_guard(lock_max_ttl_);
  max_ttl_sec_ = value_minutes * 60;
}


void MountPoint::SetupBehavior() {
  string optarg;

  if (options_mgr_->GetValue("CVMFS_MAX_TTL", &optarg))
    SetMaxTtlMn(String2Uint64(optarg));

  if (options_mgr_->GetValue("CVMFS_KCACHE_TIMEOUT", &optarg)) {
    // Can be negative and should then be interpreted as 0.0
    kcache_timeout_sec_ =
      std::max(0.0, static_cast<double>(String2Int64(optarg)));
  }
  LogCvmfs(kLogCvmfs, kLogDebug, "kernel caches expire after %d seconds",
           static_cast<int>(kcache_timeout_sec_));

  if (options_mgr_->GetValue("CVMFS_HIDE_MAGIC_XATTRS", &optarg)
      && options_mgr_->IsOn(optarg))
  {
    hide_magic_xattrs_ = true;
  }
}


/**
 * Called twice once for the regular download manager and once for the external
 * download manager.
 */
void MountPoint::SetupDnsTuning(download::DownloadManager *manager) {
  string optarg;
  unsigned dns_timeout_ms = download::DownloadManager::kDnsDefaultTimeoutMs;
  unsigned dns_retries = download::DownloadManager::kDnsDefaultRetries;
  if (options_mgr_->GetValue("CVMFS_DNS_TIMEOUT", &optarg))
    dns_timeout_ms = String2Uint64(optarg) * 1000;
  if (options_mgr_->GetValue("CVMFS_DNS_RETRIES", &optarg))
    dns_retries = String2Uint64(optarg);
  manager->SetDnsParameters(dns_retries, dns_timeout_ms);

  // Has to be after SetDnsParameters because SetDnsParameters might construct
  // a new resolver object
  if (options_mgr_->GetValue("CVMFS_DNS_SERVER", &optarg)) {
    download_mgr_->SetDnsServer(optarg);
  }

  if (options_mgr_->GetValue("CVMFS_IPFAMILY_PREFER", &optarg)) {
    switch (String2Int64(optarg)) {
      case 4:
        manager->SetIpPreference(dns::kIpPreferV4);
        break;
      case 6:
        manager->SetIpPreference(dns::kIpPreferV6);
        break;
    }
  }
  if (options_mgr_->GetValue("CVMFS_MAX_IPADDR_PER_PROXY", &optarg))
    manager->SetMaxIpaddrPerProxy(String2Uint64(optarg));
}


bool MountPoint::SetupExternalDownloadMgr() {
  string optarg;
  external_download_mgr_ =
    download_mgr_->Clone(perf::StatisticsTemplate("download-external",
      statistics_));

  unsigned timeout;
  unsigned timeout_direct;
  download_mgr_->GetTimeout(&timeout, &timeout_direct);
  if (options_mgr_->GetValue("CVMFS_EXTERNAL_TIMEOUT", &optarg)) {
    timeout = String2Uint64(optarg);
  }
  if (options_mgr_->GetValue("CVMFS_EXTERNAL_TIMEOUT_DIRECT", &optarg)) {
    timeout_direct = String2Uint64(optarg);
  }
  external_download_mgr_->SetTimeout(timeout, timeout_direct);

  if (options_mgr_->GetValue("CVMFS_EXTERNAL_URL", &optarg)) {
    external_download_mgr_->SetHostChain(ReplaceHosts(optarg));
    external_download_mgr_->ProbeGeo();
  }

  string proxies = "DIRECT";
  if (options_mgr_->GetValue("CVMFS_EXTERNAL_HTTP_PROXY", &optarg)) {
    proxies = download::ResolveProxyDescription(optarg, external_download_mgr_);
    if (proxies == "") {
      boot_error_ = "failed to discover external HTTP proxy servers";
      boot_status_ = loader::kFailWpad;
      return false;
    }
  }
  string fallback_proxies;
  if (options_mgr_->GetValue("CVMFS_EXTERNAL_FALLBACK_PROXY", &optarg))
    fallback_proxies = optarg;
  external_download_mgr_->SetProxyChain(
    proxies, fallback_proxies, download::DownloadManager::kSetProxyBoth);

  return true;
}


void MountPoint::SetupHttpTuning() {
  string optarg;

  // TODO(jblomer): avoid double default settings

  unsigned timeout = kDefaultTimeoutSec;
  unsigned timeout_direct = kDefaultTimeoutSec;
  if (options_mgr_->GetValue("CVMFS_TIMEOUT", &optarg))
    timeout = String2Uint64(optarg);
  if (options_mgr_->GetValue("CVMFS_TIMEOUT_DIRECT", &optarg))
    timeout_direct = String2Uint64(optarg);
  download_mgr_->SetTimeout(timeout, timeout_direct);

  unsigned max_retries = kDefaultRetries;
  unsigned backoff_init = kDefaultBackoffInitMs;
  unsigned backoff_max = kDefaultBackoffMaxMs;
  if (options_mgr_->GetValue("CVMFS_MAX_RETRIES", &optarg))
    max_retries = String2Uint64(optarg);
  if (options_mgr_->GetValue("CVMFS_BACKOFF_INIT", &optarg))
    backoff_init = String2Uint64(optarg) * 1000;
  if (options_mgr_->GetValue("CVMFS_BACKOFF_MAX", &optarg))
    backoff_max = String2Uint64(optarg) * 1000;
  download_mgr_->SetRetryParameters(max_retries, backoff_init, backoff_max);

  if (options_mgr_->GetValue("CVMFS_LOW_SPEED_LIMIT", &optarg))
    download_mgr_->SetLowSpeedLimit(String2Uint64(optarg));
  if (options_mgr_->GetValue("CVMFS_PROXY_RESET_AFTER", &optarg))
    download_mgr_->SetProxyGroupResetDelay(String2Uint64(optarg));
  if (options_mgr_->GetValue("CVMFS_HOST_RESET_AFTER", &optarg))
    download_mgr_->SetHostResetDelay(String2Uint64(optarg));

  if (options_mgr_->GetValue("CVMFS_FOLLOW_REDIRECTS", &optarg) &&
      options_mgr_->IsOn(optarg))
  {
    download_mgr_->EnableRedirects();
  }
  if (options_mgr_->GetValue("CVMFS_SEND_INFO_HEADER", &optarg) &&
      options_mgr_->IsOn(optarg))
  {
    download_mgr_->EnableInfoHeader();
  }
}


void MountPoint::SetupInodeAnnotation() {
  string optarg;

  inode_annotation_ = new catalog::InodeGenerationAnnotation();
  if (options_mgr_->GetValue("CVMFS_INITIAL_GENERATION", &optarg)) {
    inode_annotation_->IncGeneration(String2Uint64(optarg));
  }

  if ((file_system_->type() == FileSystem::kFsFuse) &&
      !file_system_->IsNfsSource())
  {
    catalog_mgr_->SetInodeAnnotation(inode_annotation_);
  }
}


bool MountPoint::SetupOwnerMaps() {
  string optarg;
  catalog::OwnerMap uid_map;
  catalog::OwnerMap gid_map;

  if (options_mgr_->GetValue("CVMFS_UID_MAP", &optarg)) {
    if (!uid_map.Read(optarg)) {
      boot_error_ = "failed to parse uid map " + optarg;
      boot_status_ = loader::kFailOptions;
      return false;
    }
  }
  if (options_mgr_->GetValue("CVMFS_GID_MAP", &optarg)) {
    if (!gid_map.Read(optarg)) {
      boot_error_ = "failed to parse gid map " + optarg;
      boot_status_ = loader::kFailOptions;
      return false;
    }
  }
  catalog_mgr_->SetOwnerMaps(uid_map, gid_map);

  // TODO(jblomer): make local to catalog manager
  if (options_mgr_->GetValue("CVMFS_CLAIM_OWNERSHIP", &optarg) &&
      options_mgr_->IsOn(optarg))
  {
    g_claim_ownership = true;
  }

  return true;
}