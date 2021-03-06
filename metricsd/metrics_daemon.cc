/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "metrics_daemon.h"

#include <sysexits.h>
#include <time.h>

#include <base/bind.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/hash.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <brillo/osrelease_reader.h>
#include <dbus/dbus.h>
#include <dbus/message.h>

#include "constants.h"
#include "uploader/upload_service.h"

using base::FilePath;
using base::StringPrintf;
using base::Time;
using base::TimeDelta;
using base::TimeTicks;
using chromeos_metrics::PersistentInteger;
using com::android::Weave::CommandProxy;
using com::android::Weave::ManagerProxy;
using std::map;
using std::string;
using std::vector;

namespace {

const char kCrashReporterInterface[] = "org.chromium.CrashReporter";
const char kCrashReporterUserCrashSignal[] = "UserCrash";
const char kCrashReporterMatchRule[] =
    "type='signal',interface='%s',path='/',member='%s'";

const int kSecondsPerMinute = 60;
const int kMinutesPerHour = 60;
const int kHoursPerDay = 24;
const int kMinutesPerDay = kHoursPerDay * kMinutesPerHour;
const int kSecondsPerDay = kSecondsPerMinute * kMinutesPerDay;
const int kDaysPerWeek = 7;
const int kSecondsPerWeek = kSecondsPerDay * kDaysPerWeek;

// Interval between calls to UpdateStats().
const uint32_t kUpdateStatsIntervalMs = 300000;

const char kKernelCrashDetectedFile[] = "/var/run/kernel-crash-detected";
const char kUncleanShutdownDetectedFile[] =
    "/var/run/unclean-shutdown-detected";

const int kMetricMeminfoInterval = 30;    // seconds

const char kMeminfoFileName[] = "/proc/meminfo";
const char kVmStatFileName[] = "/proc/vmstat";

// Thermal CPU throttling.

const char kMetricScaledCpuFrequencyName[] =
    "Platform.CpuFrequencyThermalScaling";

}  // namespace

// Zram sysfs entries.

const char MetricsDaemon::kComprDataSizeName[] = "compr_data_size";
const char MetricsDaemon::kOrigDataSizeName[] = "orig_data_size";
const char MetricsDaemon::kZeroPagesName[] = "zero_pages";

// Memory use stats collection intervals.  We collect some memory use interval
// at these intervals after boot, and we stop collecting after the last one,
// with the assumption that in most cases the memory use won't change much
// after that.
static const int kMemuseIntervals[] = {
  1 * kSecondsPerMinute,    // 1 minute mark
  4 * kSecondsPerMinute,    // 5 minute mark
  25 * kSecondsPerMinute,   // 0.5 hour mark
  120 * kSecondsPerMinute,  // 2.5 hour mark
  600 * kSecondsPerMinute,  // 12.5 hour mark
};

MetricsDaemon::MetricsDaemon()
    : memuse_final_time_(0),
      memuse_interval_index_(0) {}

MetricsDaemon::~MetricsDaemon() {
}

// static
double MetricsDaemon::GetActiveTime() {
  struct timespec ts;
  int r = clock_gettime(CLOCK_MONOTONIC, &ts);
  if (r < 0) {
    PLOG(WARNING) << "clock_gettime(CLOCK_MONOTONIC) failed";
    return 0;
  } else {
    return ts.tv_sec + static_cast<double>(ts.tv_nsec) / (1000 * 1000 * 1000);
  }
}

int MetricsDaemon::Run() {
  if (CheckSystemCrash(kKernelCrashDetectedFile)) {
    ProcessKernelCrash();
  }

  if (CheckSystemCrash(kUncleanShutdownDetectedFile)) {
    ProcessUncleanShutdown();
  }

  // On OS version change, clear version stats (which are reported daily).
  int32_t version = GetOsVersionHash();
  if (version_cycle_->Get() != version) {
    version_cycle_->Set(version);
    kernel_crashes_version_count_->Set(0);
    version_cumulative_active_use_->Set(0);
    version_cumulative_cpu_use_->Set(0);
  }

  return brillo::DBusDaemon::Run();
}

void MetricsDaemon::RunUploaderTest() {
  upload_service_.reset(new UploadService(
      new SystemProfileCache(true, metrics_directory_),
      metrics_lib_,
      server_));
  upload_service_->Init(upload_interval_, metrics_directory_);
  upload_service_->UploadEvent();
}

uint32_t MetricsDaemon::GetOsVersionHash() {
  brillo::OsReleaseReader reader;
  reader.Load();
  string version;
  if (!reader.GetString(metrics::kProductVersion, &version)) {
    LOG(ERROR) << "failed to read the product version.";
    version = metrics::kDefaultVersion;
  }

  uint32_t version_hash = base::Hash(version);
  if (testing_) {
    version_hash = 42;  // return any plausible value for the hash
  }
  return version_hash;
}

void MetricsDaemon::Init(bool testing,
                         bool uploader_active,
                         bool dbus_enabled,
                         MetricsLibraryInterface* metrics_lib,
                         const string& diskstats_path,
                         const string& scaling_max_freq_path,
                         const string& cpuinfo_max_freq_path,
                         const base::TimeDelta& upload_interval,
                         const string& server,
                         const base::FilePath& metrics_directory) {
  CHECK(metrics_lib);
  testing_ = testing;
  uploader_active_ = uploader_active;
  dbus_enabled_ = dbus_enabled;
  metrics_directory_ = metrics_directory;
  metrics_lib_ = metrics_lib;

  upload_interval_ = upload_interval;
  server_ = server;

  daily_active_use_.reset(
      new PersistentInteger("Platform.UseTime.PerDay"));
  version_cumulative_active_use_.reset(
      new PersistentInteger("Platform.CumulativeUseTime"));
  version_cumulative_cpu_use_.reset(
      new PersistentInteger("Platform.CumulativeCpuTime"));

  kernel_crash_interval_.reset(
      new PersistentInteger("Platform.KernelCrashInterval"));
  unclean_shutdown_interval_.reset(
      new PersistentInteger("Platform.UncleanShutdownInterval"));
  user_crash_interval_.reset(
      new PersistentInteger("Platform.UserCrashInterval"));

  any_crashes_daily_count_.reset(
      new PersistentInteger("Platform.AnyCrashes.PerDay"));
  any_crashes_weekly_count_.reset(
      new PersistentInteger("Platform.AnyCrashes.PerWeek"));
  user_crashes_daily_count_.reset(
      new PersistentInteger("Platform.UserCrashes.PerDay"));
  user_crashes_weekly_count_.reset(
      new PersistentInteger("Platform.UserCrashes.PerWeek"));
  kernel_crashes_daily_count_.reset(
      new PersistentInteger("Platform.KernelCrashes.PerDay"));
  kernel_crashes_weekly_count_.reset(
      new PersistentInteger("Platform.KernelCrashes.PerWeek"));
  kernel_crashes_version_count_.reset(
      new PersistentInteger("Platform.KernelCrashesSinceUpdate"));
  unclean_shutdowns_daily_count_.reset(
      new PersistentInteger("Platform.UncleanShutdown.PerDay"));
  unclean_shutdowns_weekly_count_.reset(
      new PersistentInteger("Platform.UncleanShutdowns.PerWeek"));

  daily_cycle_.reset(new PersistentInteger("daily.cycle"));
  weekly_cycle_.reset(new PersistentInteger("weekly.cycle"));
  version_cycle_.reset(new PersistentInteger("version.cycle"));

  scaling_max_freq_path_ = scaling_max_freq_path;
  cpuinfo_max_freq_path_ = cpuinfo_max_freq_path;
  disk_usage_collector_.reset(new DiskUsageCollector(metrics_lib_));
  averaged_stats_collector_.reset(
      new AveragedStatisticsCollector(metrics_lib_, diskstats_path,
                                      kVmStatFileName));
  cpu_usage_collector_.reset(new CpuUsageCollector(metrics_lib_));
}

int MetricsDaemon::OnInit() {
  int return_code = dbus_enabled_ ? brillo::DBusDaemon::OnInit() :
      brillo::Daemon::OnInit();
  if (return_code != EX_OK)
    return return_code;

  StatsReporterInit();

  // Start collecting meminfo stats.
  ScheduleMeminfoCallback(kMetricMeminfoInterval);
  memuse_final_time_ = GetActiveTime() + kMemuseIntervals[0];
  ScheduleMemuseCallback(kMemuseIntervals[0]);

  if (testing_)
    return EX_OK;

  if (dbus_enabled_) {
    bus_->AssertOnDBusThread();
    CHECK(bus_->SetUpAsyncOperations());

    if (bus_->is_connected()) {
      const std::string match_rule =
          base::StringPrintf(kCrashReporterMatchRule,
                             kCrashReporterInterface,
                             kCrashReporterUserCrashSignal);

      bus_->AddFilterFunction(&MetricsDaemon::MessageFilter, this);

      DBusError error;
      dbus_error_init(&error);
      bus_->AddMatch(match_rule, &error);

      if (dbus_error_is_set(&error)) {
        LOG(ERROR) << "Failed to add match rule \"" << match_rule << "\". Got "
            << error.name << ": " << error.message;
        return EX_SOFTWARE;
      }
    } else {
      LOG(ERROR) << "DBus isn't connected.";
      return EX_UNAVAILABLE;
    }

    device_ = weaved::Device::CreateInstance(
        bus_,
        base::Bind(&MetricsDaemon::UpdateWeaveState, base::Unretained(this)));
    device_->AddCommandHandler(
        "_metrics._enableAnalyticsReporting",
        base::Bind(&MetricsDaemon::OnEnableMetrics, base::Unretained(this)));
    device_->AddCommandHandler(
        "_metrics._disableAnalyticsReporting",
        base::Bind(&MetricsDaemon::OnDisableMetrics, base::Unretained(this)));
  }

  latest_cpu_use_microseconds_ = cpu_usage_collector_->GetCumulativeCpuUse();
  base::MessageLoop::current()->PostDelayedTask(FROM_HERE,
      base::Bind(&MetricsDaemon::HandleUpdateStatsTimeout,
                 base::Unretained(this)),
      base::TimeDelta::FromMilliseconds(kUpdateStatsIntervalMs));

  if (uploader_active_) {
    upload_service_.reset(
        new UploadService(new SystemProfileCache(), metrics_lib_, server_));
    upload_service_->Init(upload_interval_, metrics_directory_);
  }

  return EX_OK;
}

void MetricsDaemon::OnShutdown(int* return_code) {
  if (!testing_ && dbus_enabled_ && bus_->is_connected()) {
    const std::string match_rule =
        base::StringPrintf(kCrashReporterMatchRule,
                           kCrashReporterInterface,
                           kCrashReporterUserCrashSignal);

    bus_->RemoveFilterFunction(&MetricsDaemon::MessageFilter, this);

    DBusError error;
    dbus_error_init(&error);
    bus_->RemoveMatch(match_rule, &error);

    if (dbus_error_is_set(&error)) {
      LOG(ERROR) << "Failed to remove match rule \"" << match_rule << "\". Got "
          << error.name << ": " << error.message;
    }
  }
  brillo::DBusDaemon::OnShutdown(return_code);
}

void MetricsDaemon::OnEnableMetrics(const std::weak_ptr<weaved::Command>& cmd) {
  auto command = cmd.lock();
  if (!command)
    return;

  if (base::WriteFile(metrics_directory_.Append(metrics::kConsentFileName),
                      "", 0) != 0) {
    PLOG(ERROR) << "Could not create the consent file.";
    command->Abort("metrics_error", "Could not create the consent file",
                   nullptr);
    return;
  }

  UpdateWeaveState();
  command->Complete({}, nullptr);
}

void MetricsDaemon::OnDisableMetrics(
    const std::weak_ptr<weaved::Command>& cmd) {
  auto command = cmd.lock();
  if (!command)
    return;

  if (!base::DeleteFile(metrics_directory_.Append(metrics::kConsentFileName),
                        false)) {
    PLOG(ERROR) << "Could not delete the consent file.";
    command->Abort("metrics_error", "Could not delete the consent file",
                   nullptr);
    return;
  }

  UpdateWeaveState();
  command->Complete({}, nullptr);
}

void MetricsDaemon::UpdateWeaveState() {
  if (!device_)
    return;

  brillo::VariantDictionary state_change{
    { "_metrics._AnalyticsReportingState",
      metrics_lib_->AreMetricsEnabled() ? "enabled" : "disabled" }
  };

  if (!device_->SetStateProperties(state_change, nullptr)) {
    LOG(ERROR) << "failed to update weave's state";
  }
}

// static
DBusHandlerResult MetricsDaemon::MessageFilter(DBusConnection* connection,
                                               DBusMessage* message,
                                               void* user_data) {
  int message_type = dbus_message_get_type(message);
  if (message_type != DBUS_MESSAGE_TYPE_SIGNAL) {
    DLOG(WARNING) << "unexpected message type " << message_type;
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  // Signal messages always have interfaces.
  const std::string interface(dbus_message_get_interface(message));
  const std::string member(dbus_message_get_member(message));
  DLOG(INFO) << "Got " << interface << "." << member << " D-Bus signal";

  MetricsDaemon* daemon = static_cast<MetricsDaemon*>(user_data);

  DBusMessageIter iter;
  dbus_message_iter_init(message, &iter);
  if (interface == kCrashReporterInterface) {
    CHECK_EQ(member, kCrashReporterUserCrashSignal);
    daemon->ProcessUserCrash();
  } else {
    // Ignore messages from the bus itself.
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  return DBUS_HANDLER_RESULT_HANDLED;
}

void MetricsDaemon::ProcessUserCrash() {
  // Counts the active time up to now.
  UpdateStats(TimeTicks::Now(), Time::Now());

  // Reports the active use time since the last crash and resets it.
  SendAndResetCrashIntervalSample(user_crash_interval_);

  any_crashes_daily_count_->Add(1);
  any_crashes_weekly_count_->Add(1);
  user_crashes_daily_count_->Add(1);
  user_crashes_weekly_count_->Add(1);
}

void MetricsDaemon::ProcessKernelCrash() {
  // Counts the active time up to now.
  UpdateStats(TimeTicks::Now(), Time::Now());

  // Reports the active use time since the last crash and resets it.
  SendAndResetCrashIntervalSample(kernel_crash_interval_);

  any_crashes_daily_count_->Add(1);
  any_crashes_weekly_count_->Add(1);
  kernel_crashes_daily_count_->Add(1);
  kernel_crashes_weekly_count_->Add(1);

  kernel_crashes_version_count_->Add(1);
}

void MetricsDaemon::ProcessUncleanShutdown() {
  // Counts the active time up to now.
  UpdateStats(TimeTicks::Now(), Time::Now());

  // Reports the active use time since the last crash and resets it.
  SendAndResetCrashIntervalSample(unclean_shutdown_interval_);

  unclean_shutdowns_daily_count_->Add(1);
  unclean_shutdowns_weekly_count_->Add(1);
  any_crashes_daily_count_->Add(1);
  any_crashes_weekly_count_->Add(1);
}

bool MetricsDaemon::CheckSystemCrash(const string& crash_file) {
  FilePath crash_detected(crash_file);
  if (!base::PathExists(crash_detected))
    return false;

  // Deletes the crash-detected file so that the daemon doesn't report
  // another kernel crash in case it's restarted.
  base::DeleteFile(crash_detected, false);  // not recursive
  return true;
}

void MetricsDaemon::StatsReporterInit() {
  disk_usage_collector_->Schedule();

  cpu_usage_collector_->Init();
  cpu_usage_collector_->Schedule();

  // Don't start a collection cycle during the first run to avoid delaying the
  // boot.
  averaged_stats_collector_->ScheduleWait();
}


bool MetricsDaemon::ReadFreqToInt(const string& sysfs_file_name, int* value) {
  const FilePath sysfs_path(sysfs_file_name);
  string value_string;
  if (!base::ReadFileToString(sysfs_path, &value_string)) {
    LOG(WARNING) << "cannot read " << sysfs_path.value().c_str();
    return false;
  }
  if (!base::RemoveChars(value_string, "\n", &value_string)) {
    LOG(WARNING) << "no newline in " << value_string;
    // Continue even though the lack of newline is suspicious.
  }
  if (!base::StringToInt(value_string, value)) {
    LOG(WARNING) << "cannot convert " << value_string << " to int";
    return false;
  }
  return true;
}

void MetricsDaemon::SendCpuThrottleMetrics() {
  // |max_freq| is 0 only the first time through.
  static int max_freq = 0;
  if (max_freq == -1)
    // Give up, as sysfs did not report max_freq correctly.
    return;
  if (max_freq == 0 || testing_) {
    // One-time initialization of max_freq.  (Every time when testing.)
    if (!ReadFreqToInt(cpuinfo_max_freq_path_, &max_freq)) {
      max_freq = -1;
      return;
    }
    if (max_freq == 0) {
      LOG(WARNING) << "sysfs reports 0 max CPU frequency\n";
      max_freq = -1;
      return;
    }
    if (max_freq % 10000 == 1000) {
      // Special case: system has turbo mode, and max non-turbo frequency is
      // max_freq - 1000.  This relies on "normal" (non-turbo) frequencies
      // being multiples of (at least) 10 MHz.  Although there is no guarantee
      // of this, it seems a fairly reasonable assumption.  Otherwise we should
      // read scaling_available_frequencies, sort the frequencies, compare the
      // two highest ones, and check if they differ by 1000 (kHz) (and that's a
      // hack too, no telling when it will change).
      max_freq -= 1000;
    }
  }
  int scaled_freq = 0;
  if (!ReadFreqToInt(scaling_max_freq_path_, &scaled_freq))
    return;
  // Frequencies are in kHz.  If scaled_freq > max_freq, turbo is on, but
  // scaled_freq is not the actual turbo frequency.  We indicate this situation
  // with a 101% value.
  int percent = scaled_freq > max_freq ? 101 : scaled_freq / (max_freq / 100);
  SendLinearSample(kMetricScaledCpuFrequencyName, percent, 101, 102);
}

void MetricsDaemon::ScheduleMeminfoCallback(int wait) {
  if (testing_) {
    return;
  }
  base::TimeDelta waitDelta = base::TimeDelta::FromSeconds(wait);
  base::MessageLoop::current()->PostDelayedTask(FROM_HERE,
      base::Bind(&MetricsDaemon::MeminfoCallback, base::Unretained(this),
                 waitDelta),
      waitDelta);
}

void MetricsDaemon::MeminfoCallback(base::TimeDelta wait) {
  string meminfo_raw;
  const FilePath meminfo_path(kMeminfoFileName);
  if (!base::ReadFileToString(meminfo_path, &meminfo_raw)) {
    LOG(WARNING) << "cannot read " << meminfo_path.value().c_str();
    return;
  }
  // Make both calls even if the first one fails.
  if (ProcessMeminfo(meminfo_raw)) {
    base::MessageLoop::current()->PostDelayedTask(FROM_HERE,
        base::Bind(&MetricsDaemon::MeminfoCallback, base::Unretained(this),
                   wait),
        wait);
  }
}

// static
bool MetricsDaemon::ReadFileToUint64(const base::FilePath& path,
                                     uint64_t* value) {
  std::string content;
  if (!base::ReadFileToString(path, &content)) {
    PLOG(WARNING) << "cannot read " << path.MaybeAsASCII();
    return false;
  }
  // Remove final newline.
  base::TrimWhitespaceASCII(content, base::TRIM_TRAILING, &content);
  if (!base::StringToUint64(content, value)) {
    LOG(WARNING) << "invalid integer: " << content;
    return false;
  }
  return true;
}

bool MetricsDaemon::ReportZram(const base::FilePath& zram_dir) {
  // Data sizes are in bytes.  |zero_pages| is in number of pages.
  uint64_t compr_data_size, orig_data_size, zero_pages;
  const size_t page_size = 4096;

  if (!ReadFileToUint64(zram_dir.Append(kComprDataSizeName),
                        &compr_data_size) ||
      !ReadFileToUint64(zram_dir.Append(kOrigDataSizeName), &orig_data_size) ||
      !ReadFileToUint64(zram_dir.Append(kZeroPagesName), &zero_pages)) {
    return false;
  }

  // |orig_data_size| does not include zero-filled pages.
  orig_data_size += zero_pages * page_size;

  const int compr_data_size_mb = compr_data_size >> 20;
  const int savings_mb = (orig_data_size - compr_data_size) >> 20;
  const int zero_ratio_percent = zero_pages * page_size * 100 / orig_data_size;

  // Report compressed size in megabytes.  100 MB or less has little impact.
  SendSample("Platform.ZramCompressedSize", compr_data_size_mb, 100, 4000, 50);
  SendSample("Platform.ZramSavings", savings_mb, 100, 4000, 50);
  // The compression ratio is multiplied by 100 for better resolution.  The
  // ratios of interest are between 1 and 6 (100% and 600% as reported).  We
  // don't want samples when very little memory is being compressed.
  if (compr_data_size_mb >= 1) {
    SendSample("Platform.ZramCompressionRatioPercent",
               orig_data_size * 100 / compr_data_size, 100, 600, 50);
  }
  // The values of interest for zero_pages are between 1MB and 1GB.  The units
  // are number of pages.
  SendSample("Platform.ZramZeroPages", zero_pages, 256, 256 * 1024, 50);
  SendSample("Platform.ZramZeroRatioPercent", zero_ratio_percent, 1, 50, 50);

  return true;
}

bool MetricsDaemon::ProcessMeminfo(const string& meminfo_raw) {
  static const MeminfoRecord fields_array[] = {
    { "MemTotal", "MemTotal" },  // SPECIAL CASE: total system memory
    { "MemFree", "MemFree" },
    { "Buffers", "Buffers" },
    { "Cached", "Cached" },
    // { "SwapCached", "SwapCached" },
    { "Active", "Active" },
    { "Inactive", "Inactive" },
    { "ActiveAnon", "Active(anon)" },
    { "InactiveAnon", "Inactive(anon)" },
    { "ActiveFile" , "Active(file)" },
    { "InactiveFile", "Inactive(file)" },
    { "Unevictable", "Unevictable", kMeminfoOp_HistLog },
    // { "Mlocked", "Mlocked" },
    { "SwapTotal", "SwapTotal", kMeminfoOp_SwapTotal },
    { "SwapFree", "SwapFree", kMeminfoOp_SwapFree },
    // { "Dirty", "Dirty" },
    // { "Writeback", "Writeback" },
    { "AnonPages", "AnonPages" },
    { "Mapped", "Mapped" },
    { "Shmem", "Shmem", kMeminfoOp_HistLog },
    { "Slab", "Slab", kMeminfoOp_HistLog },
    // { "SReclaimable", "SReclaimable" },
    // { "SUnreclaim", "SUnreclaim" },
  };
  vector<MeminfoRecord> fields(fields_array,
                               fields_array + arraysize(fields_array));
  if (!FillMeminfo(meminfo_raw, &fields)) {
    return false;
  }
  int total_memory = fields[0].value;
  if (total_memory == 0) {
    // this "cannot happen"
    LOG(WARNING) << "borked meminfo parser";
    return false;
  }
  int swap_total = 0;
  int swap_free = 0;
  // Send all fields retrieved, except total memory.
  for (unsigned int i = 1; i < fields.size(); i++) {
    string metrics_name = base::StringPrintf("Platform.Meminfo%s",
                                             fields[i].name);
    int percent;
    switch (fields[i].op) {
      case kMeminfoOp_HistPercent:
        // report value as percent of total memory
        percent = fields[i].value * 100 / total_memory;
        SendLinearSample(metrics_name, percent, 100, 101);
        break;
      case kMeminfoOp_HistLog:
        // report value in kbytes, log scale, 4Gb max
        SendSample(metrics_name, fields[i].value, 1, 4 * 1000 * 1000, 100);
        break;
      case kMeminfoOp_SwapTotal:
        swap_total = fields[i].value;
      case kMeminfoOp_SwapFree:
        swap_free = fields[i].value;
        break;
    }
  }
  if (swap_total > 0) {
    int swap_used = swap_total - swap_free;
    int swap_used_percent = swap_used * 100 / swap_total;
    SendSample("Platform.MeminfoSwapUsed", swap_used, 1, 8 * 1000 * 1000, 100);
    SendLinearSample("Platform.MeminfoSwapUsed.Percent", swap_used_percent,
                     100, 101);
  }
  return true;
}

bool MetricsDaemon::FillMeminfo(const string& meminfo_raw,
                                vector<MeminfoRecord>* fields) {
  vector<string> lines;
  unsigned int nlines = Tokenize(meminfo_raw, "\n", &lines);

  // Scan meminfo output and collect field values.  Each field name has to
  // match a meminfo entry (case insensitive) after removing non-alpha
  // characters from the entry.
  unsigned int ifield = 0;
  for (unsigned int iline = 0;
       iline < nlines && ifield < fields->size();
       iline++) {
    vector<string> tokens;
    Tokenize(lines[iline], ": ", &tokens);
    if (strcmp((*fields)[ifield].match, tokens[0].c_str()) == 0) {
      // Name matches. Parse value and save.
      if (!base::StringToInt(tokens[1], &(*fields)[ifield].value)) {
        LOG(WARNING) << "Cound not convert " << tokens[1] << " to int";
        return false;
      }
      ifield++;
    }
  }
  if (ifield < fields->size()) {
    // End of input reached while scanning.
    LOG(WARNING) << "cannot find field " << (*fields)[ifield].match
                 << " and following";
    return false;
  }
  return true;
}

void MetricsDaemon::ScheduleMemuseCallback(double interval) {
  if (testing_) {
    return;
  }
  base::MessageLoop::current()->PostDelayedTask(FROM_HERE,
      base::Bind(&MetricsDaemon::MemuseCallback, base::Unretained(this)),
      base::TimeDelta::FromSeconds(interval));
}

void MetricsDaemon::MemuseCallback() {
  // Since we only care about active time (i.e. uptime minus sleep time) but
  // the callbacks are driven by real time (uptime), we check if we should
  // reschedule this callback due to intervening sleep periods.
  double now = GetActiveTime();
  // Avoid intervals of less than one second.
  double remaining_time = ceil(memuse_final_time_ - now);
  if (remaining_time > 0) {
    ScheduleMemuseCallback(remaining_time);
  } else {
    // Report stats and advance the measurement interval unless there are
    // errors or we've completed the last interval.
    if (MemuseCallbackWork() &&
        memuse_interval_index_ < arraysize(kMemuseIntervals)) {
      double interval = kMemuseIntervals[memuse_interval_index_++];
      memuse_final_time_ = now + interval;
      ScheduleMemuseCallback(interval);
    }
  }
}

bool MetricsDaemon::MemuseCallbackWork() {
  string meminfo_raw;
  const FilePath meminfo_path(kMeminfoFileName);
  if (!base::ReadFileToString(meminfo_path, &meminfo_raw)) {
    LOG(WARNING) << "cannot read " << meminfo_path.value().c_str();
    return false;
  }
  return ProcessMemuse(meminfo_raw);
}

bool MetricsDaemon::ProcessMemuse(const string& meminfo_raw) {
  static const MeminfoRecord fields_array[] = {
    { "MemTotal", "MemTotal" },  // SPECIAL CASE: total system memory
    { "ActiveAnon", "Active(anon)" },
    { "InactiveAnon", "Inactive(anon)" },
  };
  vector<MeminfoRecord> fields(fields_array,
                               fields_array + arraysize(fields_array));
  if (!FillMeminfo(meminfo_raw, &fields)) {
    return false;
  }
  int total = fields[0].value;
  int active_anon = fields[1].value;
  int inactive_anon = fields[2].value;
  if (total == 0) {
    // this "cannot happen"
    LOG(WARNING) << "borked meminfo parser";
    return false;
  }
  string metrics_name = base::StringPrintf("Platform.MemuseAnon%d",
                                           memuse_interval_index_);
  SendLinearSample(metrics_name, (active_anon + inactive_anon) * 100 / total,
                   100, 101);
  return true;
}

void MetricsDaemon::SendSample(const string& name, int sample,
                               int min, int max, int nbuckets) {
  metrics_lib_->SendToUMA(name, sample, min, max, nbuckets);
}

void MetricsDaemon::SendKernelCrashesCumulativeCountStats() {
  // Report the number of crashes for this OS version, but don't clear the
  // counter.  It is cleared elsewhere on version change.
  int64_t crashes_count = kernel_crashes_version_count_->Get();
  SendSample(kernel_crashes_version_count_->Name(),
             crashes_count,
             1,                         // value of first bucket
             500,                       // value of last bucket
             100);                      // number of buckets


  int64_t cpu_use_ms = version_cumulative_cpu_use_->Get();
  SendSample(version_cumulative_cpu_use_->Name(),
             cpu_use_ms / 1000,         // stat is in seconds
             1,                         // device may be used very little...
             8 * 1000 * 1000,           // ... or a lot (a little over 90 days)
             100);

  // On the first run after an autoupdate, cpu_use_ms and active_use_seconds
  // can be zero.  Avoid division by zero.
  if (cpu_use_ms > 0) {
    // Send the crash frequency since update in number of crashes per CPU year.
    SendSample("Logging.KernelCrashesPerCpuYear",
               crashes_count * kSecondsPerDay * 365 * 1000 / cpu_use_ms,
               1,
               1000 * 1000,     // about one crash every 30s of CPU time
               100);
  }

  int64_t active_use_seconds = version_cumulative_active_use_->Get();
  if (active_use_seconds > 0) {
    SendSample(version_cumulative_active_use_->Name(),
               active_use_seconds,
               1,                          // device may be used very little...
               8 * 1000 * 1000,            // ... or a lot (about 90 days)
               100);
    // Same as above, but per year of active time.
    SendSample("Logging.KernelCrashesPerActiveYear",
               crashes_count * kSecondsPerDay * 365 / active_use_seconds,
               1,
               1000 * 1000,     // about one crash every 30s of active time
               100);
  }
}

void MetricsDaemon::SendAndResetDailyUseSample(
    const scoped_ptr<PersistentInteger>& use) {
  SendSample(use->Name(),
             use->GetAndClear(),
             1,                        // value of first bucket
             kSecondsPerDay,           // value of last bucket
             50);                      // number of buckets
}

void MetricsDaemon::SendAndResetCrashIntervalSample(
    const scoped_ptr<PersistentInteger>& interval) {
  SendSample(interval->Name(),
             interval->GetAndClear(),
             1,                        // value of first bucket
             4 * kSecondsPerWeek,      // value of last bucket
             50);                      // number of buckets
}

void MetricsDaemon::SendAndResetCrashFrequencySample(
    const scoped_ptr<PersistentInteger>& frequency) {
  SendSample(frequency->Name(),
             frequency->GetAndClear(),
             1,                        // value of first bucket
             100,                      // value of last bucket
             50);                      // number of buckets
}

void MetricsDaemon::SendLinearSample(const string& name, int sample,
                                     int max, int nbuckets) {
  // TODO(semenzato): add a proper linear histogram to the Chrome external
  // metrics API.
  LOG_IF(FATAL, nbuckets != max + 1) << "unsupported histogram scale";
  metrics_lib_->SendEnumToUMA(name, sample, max);
}

void MetricsDaemon::UpdateStats(TimeTicks now_ticks,
                                Time now_wall_time) {
  const int elapsed_seconds = (now_ticks - last_update_stats_time_).InSeconds();
  daily_active_use_->Add(elapsed_seconds);
  version_cumulative_active_use_->Add(elapsed_seconds);
  user_crash_interval_->Add(elapsed_seconds);
  kernel_crash_interval_->Add(elapsed_seconds);
  TimeDelta cpu_use = cpu_usage_collector_->GetCumulativeCpuUse();
  version_cumulative_cpu_use_->Add(
      (cpu_use - latest_cpu_use_microseconds_).InMilliseconds());
  latest_cpu_use_microseconds_ = cpu_use;
  last_update_stats_time_ = now_ticks;

  const TimeDelta since_epoch = now_wall_time - Time::UnixEpoch();
  const int day = since_epoch.InDays();
  const int week = day / 7;

  if (daily_cycle_->Get() != day) {
    daily_cycle_->Set(day);
    SendAndResetDailyUseSample(daily_active_use_);
    SendAndResetCrashFrequencySample(any_crashes_daily_count_);
    SendAndResetCrashFrequencySample(user_crashes_daily_count_);
    SendAndResetCrashFrequencySample(kernel_crashes_daily_count_);
    SendAndResetCrashFrequencySample(unclean_shutdowns_daily_count_);
    SendKernelCrashesCumulativeCountStats();
  }

  if (weekly_cycle_->Get() != week) {
    weekly_cycle_->Set(week);
    SendAndResetCrashFrequencySample(any_crashes_weekly_count_);
    SendAndResetCrashFrequencySample(user_crashes_weekly_count_);
    SendAndResetCrashFrequencySample(kernel_crashes_weekly_count_);
    SendAndResetCrashFrequencySample(unclean_shutdowns_weekly_count_);
  }
}

void MetricsDaemon::HandleUpdateStatsTimeout() {
  UpdateStats(TimeTicks::Now(), Time::Now());
  base::MessageLoop::current()->PostDelayedTask(FROM_HERE,
      base::Bind(&MetricsDaemon::HandleUpdateStatsTimeout,
                 base::Unretained(this)),
      base::TimeDelta::FromMilliseconds(kUpdateStatsIntervalMs));
}
