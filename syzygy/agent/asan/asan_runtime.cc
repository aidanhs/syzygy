// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "syzygy/agent/asan/asan_runtime.h"

#include <algorithm>
#include <vector>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/environment.h"
#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/pe_image.h"
#include "base/win/wrapped_window_proc.h"
#include "syzygy/agent/asan/asan_logger.h"
#include "syzygy/agent/asan/block.h"
#include "syzygy/agent/asan/heap_checker.h"
#include "syzygy/agent/asan/page_protection_helpers.h"
#include "syzygy/agent/asan/shadow.h"
#include "syzygy/agent/asan/stack_capture_cache.h"
#include "syzygy/agent/asan/windows_heap_adapter.h"
#include "syzygy/trace/client/client_utils.h"
#include "syzygy/trace/protocol/call_trace_defs.h"

namespace agent {
namespace asan {

namespace {

using agent::asan::AsanLogger;
using agent::asan::StackCaptureCache;
using agent::asan::WindowsHeapAdapter;
using base::win::WinProcExceptionFilter;

// Signatures of the various Breakpad functions for setting custom crash
// key-value pairs.
// Post r194002.
typedef void (__cdecl * SetCrashKeyValuePairPtr)(const char*, const char*);
// Post r217590.
typedef void (__cdecl * SetCrashKeyValueImplPtr)(const wchar_t*,
                                                 const wchar_t*);

// Collects the various Breakpad-related exported functions.
struct BreakpadFunctions {
  // The Breakpad crash reporting entry point.
  WinProcExceptionFilter crash_for_exception_ptr;

  // Various flavours of the custom key-value setting function. The version
  // exported depends on the version of Chrome. It is possible for both of these
  // to be NULL even if crash_for_exception_ptr is not NULL.
  SetCrashKeyValuePairPtr set_crash_key_value_pair_ptr;
  SetCrashKeyValueImplPtr set_crash_key_value_impl_ptr;
};

// The static breakpad functions. All runtimes share these. This is under
// AsanRuntime::lock_.
BreakpadFunctions breakpad_functions = {};

// A custom exception code we use to indicate that the exception originated
// from ASan, and shouldn't be processed again by our unhandled exception
// handler. This value has been created according to the rules here:
// http://msdn.microsoft.com/en-us/library/windows/hardware/ff543026(v=vs.85).aspx
// See winerror.h for more details.
static const DWORD kAsanFacility = 0x68B;  // No more than 11 bits.
static const DWORD kAsanStatus = 0x5AD0;  // No more than 16 bits.
static const DWORD kAsanException =
    (3 << 30) |  // Severity = error.
    (1 << 29) |  // Customer defined code (not defined by MS).
    (kAsanFacility << 16) |  // Facility code.
    kAsanStatus;  // Status code.
COMPILE_ASSERT((kAsanFacility >> 11) == 0, too_many_facility_bits);
COMPILE_ASSERT((kAsanStatus >> 16) == 0, too_many_status_bits);
COMPILE_ASSERT((kAsanException & (3 << 27)) == 0,
               bits_27_and_28_must_be_clear);

// Raises an exception, first wrapping it an ASan specific exception. This
// indicates to our unhandled exception handler that it doesn't need to
// process the exception.
void RaiseFilteredException(
    DWORD code, DWORD flags, DWORD num_args, const ULONG_PTR* args) {
  // Retain the original arguments and craft a new exception.
  const ULONG_PTR arguments[4] = {
      code, flags, num_args, reinterpret_cast<const ULONG_PTR>(args) };
  ::RaiseException(kAsanException, 0, ARRAYSIZE(arguments), arguments);
}

// The default error handler. It is expected that this will be bound in a
// callback in the ASan runtime.
// @param context The context when the error has been reported.
// @param error_info The information about this error.
void DefaultErrorHandler(AsanErrorInfo* error_info) {
  DCHECK_NE(reinterpret_cast<AsanErrorInfo*>(NULL), error_info);

  ULONG_PTR arguments[] = {
    reinterpret_cast<ULONG_PTR>(&error_info->context),
    reinterpret_cast<ULONG_PTR>(error_info)
  };

  ::DebugBreak();

  // This raises an error in such a way that the ASan unhandled exception
  // handler will not process it.
  RaiseFilteredException(EXCEPTION_ARRAY_BOUNDS_EXCEEDED,
                         0,
                         ARRAYSIZE(arguments),
                         arguments);
}

// Returns the breakpad crash reporting functions if breakpad is enabled for
// the current executable.
//
// If we're running in the context of a breakpad enabled binary we can
// report errors directly via that breakpad entry-point. This allows us
// to report the exact context of the error without including the ASan RTL
// in crash context, depending on where and when we capture the context.
//
// @param breakpad_functions The Breakpad functions structure to be populated.
// @returns true if we found breakpad functions, false otherwise.
bool GetBreakpadFunctions(BreakpadFunctions* breakpad_functions) {
  DCHECK_NE(reinterpret_cast<BreakpadFunctions*>(NULL), breakpad_functions);

  // Clear the structure.
  ::memset(breakpad_functions, 0, sizeof(*breakpad_functions));

  // The named entry-point exposed to report a crash.
  static const char kCrashHandlerSymbol[] = "CrashForException";

  // The named entry-point exposed to annotate a crash with a key/value pair.
  static const char kSetCrashKeyValuePairSymbol[] = "SetCrashKeyValuePair";
  static const char kSetCrashKeyValueImplSymbol[] = "SetCrashKeyValueImpl";

  // Get a handle to the current executable image.
  HMODULE exe_hmodule = ::GetModuleHandle(NULL);

  // Lookup the crash handler symbol.
  breakpad_functions->crash_for_exception_ptr =
      reinterpret_cast<WinProcExceptionFilter>(
          ::GetProcAddress(exe_hmodule, kCrashHandlerSymbol));
  if (breakpad_functions->crash_for_exception_ptr == NULL)
    return false;

  // Lookup the crash annotation symbol.
  breakpad_functions->set_crash_key_value_pair_ptr =
      reinterpret_cast<SetCrashKeyValuePairPtr>(
          ::GetProcAddress(exe_hmodule, kSetCrashKeyValuePairSymbol));
  breakpad_functions->set_crash_key_value_impl_ptr =
      reinterpret_cast<SetCrashKeyValueImplPtr>(
          ::GetProcAddress(exe_hmodule, kSetCrashKeyValueImplSymbol));

  return true;
}

// Sets a crash key using the given breakpad function.
void SetCrashKeyValuePair(const BreakpadFunctions& breakpad_functions,
                          const char* key,
                          const char* value) {
  if (breakpad_functions.set_crash_key_value_pair_ptr != NULL) {
    breakpad_functions.set_crash_key_value_pair_ptr(key, value);
    return;
  }

  if (breakpad_functions.set_crash_key_value_impl_ptr != NULL) {
    std::wstring wkey = base::UTF8ToWide(key);
    std::wstring wvalue = base::UTF8ToWide(value);
    breakpad_functions.set_crash_key_value_impl_ptr(wkey.c_str(),
                                                    wvalue.c_str());
    return;
  }

  return;
}

// Writes the appropriate crash keys for the given error.
void SetCrashKeys(const BreakpadFunctions& breakpad_functions,
                  AsanErrorInfo* error_info) {
  DCHECK(breakpad_functions.crash_for_exception_ptr != NULL);
  DCHECK(error_info != NULL);

  SetCrashKeyValuePair(breakpad_functions,
                       "asan-error-type",
                       ErrorInfoAccessTypeToStr(error_info->error_type));

  if (error_info->shadow_info[0] != '\0') {
    SetCrashKeyValuePair(breakpad_functions,
                         "asan-error-message",
                         error_info->shadow_info);
  }
}

// Initializes an exception record for an ASAN crash.
void InitializeExceptionRecord(const AsanErrorInfo* error_info,
                               EXCEPTION_RECORD* record,
                               EXCEPTION_POINTERS* pointers) {
  DCHECK_NE(static_cast<AsanErrorInfo*>(nullptr), error_info);
  DCHECK_NE(static_cast<EXCEPTION_RECORD*>(nullptr), record);
  DCHECK_NE(static_cast<EXCEPTION_POINTERS*>(nullptr), pointers);

  ::memset(record, 0, sizeof(EXCEPTION_RECORD));
  record->ExceptionCode = EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
  record->ExceptionAddress = reinterpret_cast<PVOID>(
      error_info->context.Eip);
  record->NumberParameters = 2;
  record->ExceptionInformation[0] = reinterpret_cast<ULONG_PTR>(
      &error_info->context);
  record->ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(error_info);

  pointers->ExceptionRecord = record;
  pointers->ContextRecord = const_cast<CONTEXT*>(&error_info->context);
}

// The breakpad error handler. It is expected that this will be bound in a
// callback in the ASan runtime.
// @param breakpad_functions A struct containing pointers to the various
//     Breakpad reporting functions.
// @param error_info The information about this error.
void BreakpadErrorHandler(const BreakpadFunctions& breakpad_functions,
                          AsanErrorInfo* error_info) {
  DCHECK(breakpad_functions.crash_for_exception_ptr != NULL);
  DCHECK(error_info != NULL);

  SetCrashKeys(breakpad_functions, error_info);

  EXCEPTION_RECORD exception = {};
  EXCEPTION_POINTERS pointers = {};
  InitializeExceptionRecord(error_info, &exception, &pointers);

  breakpad_functions.crash_for_exception_ptr(&pointers);
  NOTREACHED();
}

// A helper function to find if an intrusive list contains a given entry.
// @param list The list in which we want to look for the entry.
// @param item The entry we want to look for.
// @returns true if the list contains this entry, false otherwise.
bool HeapListContainsEntry(const LIST_ENTRY* list, const LIST_ENTRY* item) {
  LIST_ENTRY* current = list->Flink;
  while (current != NULL) {
    LIST_ENTRY* next_item = NULL;
    if (current->Flink != list) {
      next_item = current->Flink;
    }

    if (current == item) {
      return true;
    }

    current = next_item;
  }
  return false;
}

// Check if the current process is large address aware.
// @returns true if it is, false otherwise.
bool CurrentProcessIsLargeAddressAware() {
  const base::win::PEImage image(::GetModuleHandle(NULL));

  bool process_is_large_address_aware =
    (image.GetNTHeaders()->FileHeader.Characteristics &
        IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;

  return process_is_large_address_aware;
}

// A helper function to send a command to Windbg. Windbg should first receive
// the ".ocommand ASAN" command to treat those messages as commands.
void ASANDbgCmd(const wchar_t* fmt, ...) {
  if (!base::debug::BeingDebugged())
    return;
  // The string should start with "ASAN" to be interpreted by the debugger as a
  // command.
  std::wstring command_wstring = L"ASAN ";
  va_list args;
  va_start(args, fmt);

  // Append the actual command to the wstring.
  base::StringAppendV(&command_wstring, fmt, args);

  // Append "; g" to make sure that the debugger continues its execution after
  // executing this command. This is needed because when the .ocommand function
  // is used under Windbg the debugger will break on OutputDebugString.
  command_wstring.append(L"; g");

  OutputDebugString(command_wstring.c_str());
}

// A helper function to print a message to Windbg's console.
void ASANDbgMessage(const wchar_t* fmt, ...) {
  if (!base::debug::BeingDebugged())
    return;
  // Prepend the message with the .echo command so it'll be printed into the
  // debugger's console.
  std::wstring message_wstring = L".echo ";
  va_list args;
  va_start(args, fmt);

  // Append the actual message to the wstring.
  base::StringAppendV(&message_wstring, fmt, args);

  // Treat the message as a command to print it.
  ASANDbgCmd(message_wstring.c_str());
}

// Switch to the caller's context and print its stack trace in Windbg.
void ASANDbgPrintContext(const CONTEXT& context) {
  if (!base::debug::BeingDebugged())
    return;
  ASANDbgMessage(L"Caller's context (%p) and stack trace:", &context);
  ASANDbgCmd(L".cxr %p; kv", reinterpret_cast<uint32>(&context));
}

// Returns the maximum allocation size that can be made safely. This leaves
// space for child function frames, ideally enough for Breakpad to do its
// work.
size_t MaxSafeAllocaSize() {
  // We leave 5KB of stack space for Breakpad and other crash reporting
  // machinery.
  const size_t kReservedStack = 5 * 1024;

  // Find the base of the stack.
  MEMORY_BASIC_INFORMATION mbi = {};
  void* stack = &mbi;
  if (VirtualQuery(stack, &mbi, sizeof(mbi)) == 0)
    return 0;
  size_t max_size = reinterpret_cast<uint8*>(stack) -
      reinterpret_cast<uint8*>(mbi.AllocationBase);
  max_size -= std::min(max_size, kReservedStack);
  return max_size;
}

// Performs a dynamic stack allocation of at most |size| bytes. Sets the actual
// size of the allocation and the pointer to it by modifying |size| and |result|
// directly.
#define SAFE_ALLOCA(size, result) {  \
    size_t max_size = MaxSafeAllocaSize();  \
    size = std::min(size, max_size);  \
    result = _alloca(size);  \
    if (result == NULL)  \
      size = 0;  \
  }

// Runs the heap checker if enabled. If heap corruption is found serializes
// the results to the stack and modifies the |error_info| structure.
#define CHECK_HEAP_CORRUPTION(runtime, error_info)  \
  (error_info)->heap_is_corrupt = false;  \
  if (!((runtime)->params_.check_heap_on_failure)) {  \
    runtime_->logger_->Write(  \
          "SyzyASAN: Heap checker disabled, ignoring exception.");  \
  } else {  \
    runtime_->logger_->Write(  \
          "SyzyASAN: Heap checker enabled, processing exception.");  \
    AutoHeapManagerLock lock(runtime_->heap_manager_.get());  \
    HeapChecker heap_checker;  \
    HeapChecker::CorruptRangesVector corrupt_ranges;  \
    heap_checker.IsHeapCorrupt(&corrupt_ranges);  \
    size_t size = (runtime)->CalculateCorruptHeapInfoSize(corrupt_ranges);  \
    void* buffer = NULL;  \
    if (size > 0) {  \
      SAFE_ALLOCA(size, buffer);  \
      (runtime)->WriteCorruptHeapInfo(  \
          corrupt_ranges, size, buffer, error_info);  \
    }  \
  }

}  // namespace

const char AsanRuntime::kSyzygyAsanOptionsEnvVar[] = "SYZYGY_ASAN_OPTIONS";

base::Lock AsanRuntime::lock_;
AsanRuntime* AsanRuntime::runtime_ = NULL;
LPTOP_LEVEL_EXCEPTION_FILTER AsanRuntime::previous_uef_ = NULL;
bool AsanRuntime::uef_installed_ = false;

AsanRuntime::AsanRuntime()
    : logger_(), stack_cache_(), asan_error_callback_(), heap_manager_() {
  ::common::SetDefaultAsanParameters(&params_);
  starting_ticks_ = ::GetTickCount();
}

AsanRuntime::~AsanRuntime() {
}

void AsanRuntime::SetUp(const std::wstring& flags_command_line) {
  base::AutoLock auto_lock(lock_);
  DCHECK(!runtime_);
  runtime_ = this;

  // Ensure that the current process is not large address aware. It shouldn't be
  // because the shadow memory assume that the process will only be able to use
  // 2GB of address space.
  CHECK(!CurrentProcessIsLargeAddressAware());

  // Initialize the command-line structures. This is needed so that
  // SetUpLogger() can include the command-line in the message announcing
  // this process. Note: this is mostly for debugging purposes.
  CommandLine::Init(0, NULL);

  Shadow::SetUp();

  // Setup the "global" state.
  common::StackCapture::Init();
  StackCaptureCache::Init();
  SetUpLogger();
  SetUpStackCache();
  SetUpHeapManager();
  WindowsHeapAdapter::SetUp(heap_manager_.get());

  // Parse and propagate any flags set via the environment variable. This logs
  // failure for us.
  if (!::common::ParseAsanParameters(flags_command_line, &params_))
    return;

  // Propagates the flags values to the different modules.
  PropagateParams();

  // Register the error reporting callback to use if/when an ASan error is
  // detected. If we're able to resolve a breakpad error reporting function
  // then use that; otherwise, fall back to the default error handler.
  if (!params_.disable_breakpad_reporting &&
      GetBreakpadFunctions(&breakpad_functions)) {
    logger_->Write("SyzyASAN: Using Breakpad for error reporting.");
    SetErrorCallBack(base::Bind(&BreakpadErrorHandler, breakpad_functions));
  } else {
    logger_->Write("SyzyASAN: Using default error reporting handler.");
    SetErrorCallBack(base::Bind(&DefaultErrorHandler));
  }

  // Install the unhandled exception handler. This is only installed once
  // across all runtime instances in a process so we check that it hasn't
  // already been installed.
  if (!uef_installed_) {
    uef_installed_ = true;
    previous_uef_ = ::SetUnhandledExceptionFilter(&UnhandledExceptionFilter);
  }

  // Finally, initialize the heap manager. This comes after parsing all
  // parameters as some decisions can only be made once.
  heap_manager_->Init();
}

void AsanRuntime::TearDown() {
  base::AutoLock auto_lock(lock_);

  WindowsHeapAdapter::TearDown();
  TearDownHeapManager();
  TearDownStackCache();
  TearDownLogger();
  DCHECK(asan_error_callback_.is_null() == FALSE);
  asan_error_callback_.Reset();
  Shadow::TearDown();

  // Unregister ourselves as the singleton runtime for UEF.
  runtime_ = NULL;

  // In principle, we should also check that all the heaps have been destroyed
  // but this is not guaranteed to be the case in Chrome, so the heap list may
  // not be empty here.
}

void AsanRuntime::OnErrorImpl(AsanErrorInfo* error_info) {
  DCHECK_NE(reinterpret_cast<AsanErrorInfo*>(NULL), error_info);

  LogAsanErrorInfo(error_info);

  if (params_.minidump_on_failure) {
    DCHECK(logger_.get() != NULL);
    logger_->SaveMiniDump(&error_info->context, error_info);
  }

  if (params_.exit_on_failure) {
    DCHECK(logger_.get() != NULL);
    logger_->Stop();
    exit(EXIT_FAILURE);
  }
}

void AsanRuntime::OnError(AsanErrorInfo* error_info) {
  DCHECK_NE(reinterpret_cast<AsanErrorInfo*>(NULL), error_info);

  // Grab the global page protection lock to prevent page protection settings
  // from being modified while processing the error.
  ::common::AutoRecursiveLock lock(block_protect_lock);

  // Unfortunately this is a giant macro, but it needs to be as it performs
  // stack allocations.
  CHECK_HEAP_CORRUPTION(this, error_info);

  OnErrorImpl(error_info);

  // Call the callback to handle this error.
  DCHECK(!asan_error_callback_.is_null());
  asan_error_callback_.Run(error_info);
}

void AsanRuntime::SetErrorCallBack(const AsanOnErrorCallBack& callback) {
  asan_error_callback_ = callback;
}

void AsanRuntime::SetUpLogger() {
  // Setup variables we're going to use.
  scoped_ptr<base::Environment> env(base::Environment::Create());
  scoped_ptr<AsanLogger> client(new AsanLogger);
  CHECK(env.get() != NULL);
  CHECK(client.get() != NULL);

  // Initialize the client.
  client->set_instance_id(
      base::UTF8ToWide(trace::client::GetInstanceIdForThisModule()));
  client->Init();

  // Register the client singleton instance.
  logger_.reset(client.release());
}

void AsanRuntime::TearDownLogger() {
  logger_.reset();
}

void AsanRuntime::SetUpStackCache() {
  DCHECK(stack_cache_.get() == NULL);
  DCHECK(logger_.get() != NULL);
  stack_cache_.reset(new StackCaptureCache(logger_.get()));
}

void AsanRuntime::TearDownStackCache() {
  DCHECK(stack_cache_.get() != NULL);
  stack_cache_->LogStatistics();
  stack_cache_.reset();
}

void AsanRuntime::SetUpHeapManager() {
  DCHECK_EQ(static_cast<heap_managers::BlockHeapManager*>(NULL),
            heap_manager_.get());
  DCHECK_NE(static_cast<StackCaptureCache*>(NULL), stack_cache_.get());
  heap_manager_.reset(new heap_managers::BlockHeapManager(stack_cache_.get()));

  // Configure the heap manager to notify us on heap corruption.
  heap_manager_->SetHeapErrorCallback(base::Bind(&AsanRuntime::OnError,
                                                 base::Unretained(this)));
}

void AsanRuntime::TearDownHeapManager() {
  DCHECK_NE(static_cast<heap_managers::BlockHeapManager*>(NULL),
            heap_manager_.get());
  heap_manager_.reset();
}

bool AsanRuntime::GetAsanFlagsEnvVar(std::wstring* env_var_wstr) {
  scoped_ptr<base::Environment> env(base::Environment::Create());
  if (env.get() == NULL) {
    LOG(ERROR) << "base::Environment::Create returned NULL.";
    return false;
  }

  // If this fails, the environment variable simply does not exist.
  std::string env_var_str;
  if (!env->GetVar(kSyzygyAsanOptionsEnvVar, &env_var_str)) {
    return true;
  }

  *env_var_wstr = base::SysUTF8ToWide(env_var_str);

  return true;
}

void AsanRuntime::PropagateParams() {
  // This function has to be kept in sync with the AsanParameters struct. These
  // checks will ensure that this is the case.
  COMPILE_ASSERT(sizeof(::common::AsanParameters) == 56,
                 must_update_propagate_params);
  COMPILE_ASSERT(::common::kAsanParametersVersion == 7,
                 must_update_parameters_version);

  // Push the configured parameter values to the appropriate endpoints.
  heap_manager_->set_parameters(params_);
  StackCaptureCache::set_compression_reporting_period(
      params_.reporting_period);
  common::StackCapture::set_bottom_frames_to_skip(
      params_.bottom_frames_to_skip);
  stack_cache_->set_max_num_frames(params_.max_num_frames);
  // ignored_stack_ids is used locally by AsanRuntime.
  logger_->set_log_as_text(params_.log_as_text);
  // exit_on_failure is used locally by AsanRuntime.
  logger_->set_minidump_on_failure(params_.minidump_on_failure);
}

size_t AsanRuntime::CalculateCorruptHeapInfoSize(
    const HeapChecker::CorruptRangesVector& corrupt_ranges) {
  size_t n = corrupt_ranges.size() *
      (sizeof(AsanCorruptBlockRange) + sizeof(AsanBlockInfo));
  return n;
}

void AsanRuntime::WriteCorruptHeapInfo(
    const HeapChecker::CorruptRangesVector& corrupt_ranges,
    size_t buffer_size,
    void* buffer,
    AsanErrorInfo* error_info) {
  DCHECK((buffer_size == 0 && buffer == NULL) ||
         (buffer_size != 0 && buffer != NULL));
  DCHECK_NE(reinterpret_cast<AsanErrorInfo*>(NULL), error_info);

  ::memset(buffer, 0, buffer_size);

  error_info->heap_is_corrupt = false;
  error_info->corrupt_range_count = 0;
  error_info->corrupt_block_count = 0;
  error_info->corrupt_ranges_reported = 0;
  error_info->corrupt_ranges = NULL;

  if (corrupt_ranges.empty())
    return;

  // If we have corrupt ranges then set the aggregate fields.
  error_info->heap_is_corrupt = true;
  error_info->corrupt_range_count = corrupt_ranges.size();
  for (size_t i = 0; i < corrupt_ranges.size(); ++i)
    error_info->corrupt_block_count += corrupt_ranges[i].block_count;

  // We report a AsanCorruptBlockRange and at least one AsanBlockInfo per
  // corrupt range. Determine how many ranges we can report on.
  size_t range_count = buffer_size /
      (sizeof(AsanCorruptBlockRange) + sizeof(AsanBlockInfo));
  range_count = std::min(range_count, corrupt_ranges.size());

  // Allocate space for the corrupt range metadata.
  uint8* cursor = reinterpret_cast<uint8*>(buffer);
  uint8* buffer_end = cursor + buffer_size;
  error_info->corrupt_ranges = reinterpret_cast<AsanCorruptBlockRange*>(
      cursor);
  cursor += range_count * sizeof(AsanCorruptBlockRange);
  error_info->corrupt_range_count = corrupt_ranges.size();
  error_info->corrupt_ranges_reported = range_count;

  // Allocate space for the corrupt block metadata.
  size_t block_count = (buffer_end - cursor) / sizeof(AsanBlockInfo);
  AsanBlockInfo* block_infos = reinterpret_cast<AsanBlockInfo*>(cursor);
  cursor += block_count * sizeof(AsanBlockInfo);

  // Write as many corrupt block ranges as we have room for. This is
  // effectively random as it is by order of address.
  for (size_t i = 0; i < range_count; ++i) {
    // Copy the information about the corrupt range.
    error_info->corrupt_ranges[i] = corrupt_ranges[i];

    // Allocate space for the first block of this range on the stack.
    // TODO(sebmarchand): Report more blocks if necessary.
    AsanBlockInfo* asan_block_info = block_infos;
    error_info->corrupt_ranges[i].block_info = block_infos;
    error_info->corrupt_ranges[i].block_info_count = 1;
    ++block_infos;

    // Use a shadow walker to find the first corrupt block in this range and
    // copy its metadata.
    ShadowWalker shadow_walker(
        false,
        reinterpret_cast<const uint8*>(corrupt_ranges[i].address),
        reinterpret_cast<const uint8*>(corrupt_ranges[i].address) +
            corrupt_ranges[i].length);
    BlockInfo block_info = {};
    CHECK(shadow_walker.Next(&block_info));
    // The heap checker removes block protections as it goes, so this block
    // should be readable. However, remove page protections just to be sure.
    // They are left turned off so that the minidump generation can introspect
    // the block.
    BlockProtectNone(block_info);
    asan_block_info->header = block_info.header;
    ErrorInfoGetAsanBlockInfo(stack_cache_.get(), asan_block_info);
    DCHECK(asan_block_info->corrupt);
  }

  return;
}

void AsanRuntime::LogAsanErrorInfo(AsanErrorInfo* error_info) {
  DCHECK_NE(reinterpret_cast<AsanErrorInfo*>(NULL), error_info);

  const char* bug_descr = ErrorInfoAccessTypeToStr(error_info->error_type);
  if (logger_->log_as_text()) {
    std::string output(base::StringPrintf(
        "SyzyASAN error: %s on address 0x%08X (stack_id=0x%08X)\n",
        bug_descr, error_info->location, error_info->crash_stack_id));
    if (error_info->access_mode != agent::asan::ASAN_UNKNOWN_ACCESS) {
      const char* access_mode_str = NULL;
      if (error_info->access_mode == agent::asan::ASAN_READ_ACCESS)
        access_mode_str = "READ";
      else
        access_mode_str = "WRITE";
      base::StringAppendF(&output,
                          "%s of size %d at 0x%08X\n",
                          access_mode_str,
                          error_info->access_size,
                          error_info->location);
    }

    // Log the failure and stack.
    logger_->WriteWithContext(output, error_info->context);

    logger_->Write(error_info->shadow_info);
    if (error_info->free_stack_size != 0U) {
      logger_->WriteWithStackTrace("freed here:\n",
                                   error_info->free_stack,
                                   error_info->free_stack_size);
    }
    if (error_info->alloc_stack_size != NULL) {
      logger_->WriteWithStackTrace("previously allocated here:\n",
                                   error_info->alloc_stack,
                                   error_info->alloc_stack_size);
    }
    if (error_info->error_type >= USE_AFTER_FREE) {
      std::string shadow_text;
      Shadow::AppendShadowMemoryText(error_info->location, &shadow_text);
      logger_->Write(shadow_text);
    }
  }

  // Print the base of the Windbg help message.
  ASANDbgMessage(L"An Asan error has been found (%ls), here are the details:",
                 base::SysUTF8ToWide(bug_descr).c_str());

  // Print the Windbg information to display the allocation stack if present.
  if (error_info->alloc_stack_size != NULL) {
    ASANDbgMessage(L"Allocation stack trace:");
    ASANDbgCmd(L"dps %p l%d",
               error_info->alloc_stack,
               error_info->alloc_stack_size);
  }

  // Print the Windbg information to display the free stack if present.
  if (error_info->free_stack_size != NULL) {
    ASANDbgMessage(L"Free stack trace:");
    ASANDbgCmd(L"dps %p l%d",
               error_info->free_stack,
               error_info->free_stack_size);
  }
}

void AsanRuntime::GetBadAccessInformation(AsanErrorInfo* error_info) {
  base::AutoLock lock(lock_);

  // Checks if this is an access to an internal structure or if it's an access
  // in the upper region of the memory (over the 2 GB limit).
  if ((reinterpret_cast<size_t>(error_info->location) & (1 << 31)) != 0 ||
      Shadow::GetShadowMarkerForAddress(error_info->location)
          == kAsanMemoryMarker) {
      error_info->error_type = WILD_ACCESS;
  } else if (Shadow::GetShadowMarkerForAddress(error_info->location) ==
      kInvalidAddressMarker) {
    error_info->error_type = INVALID_ADDRESS;
  } else {
    ErrorInfoGetBadAccessInformation(stack_cache_.get(), error_info);
  }
}

bool AsanRuntime::allocation_filter_flag() {
  return heap_manager_->allocation_filter_flag();
}

void AsanRuntime::set_allocation_filter_flag(bool value) {
  heap_manager_->set_allocation_filter_flag(value);
}

void AsanRuntime::AddThreadId(uint32 thread_id) {
  DCHECK_NE(0u, thread_id);
  base::AutoLock lock(thread_ids_lock_);
  thread_ids_.insert(thread_id);
}

bool AsanRuntime::ThreadIdIsValid(uint32 thread_id) {
  base::AutoLock lock(thread_ids_lock_);
  return thread_ids_.count(thread_id) > 0;
}

bool AsanRuntime::HeapIdIsValid(HeapManagerInterface::HeapId heap_id) {
  return heap_manager_->IsValidHeap(heap_id);
}

int AsanRuntime::CrashForException(EXCEPTION_POINTERS* exception) {
  return ExceptionFilterImpl(false, exception);
}

LONG WINAPI AsanRuntime::UnhandledExceptionFilter(
    struct _EXCEPTION_POINTERS* exception) {
  return ExceptionFilterImpl(true, exception);
}

LONG AsanRuntime::ExceptionFilterImpl(bool is_unhandled,
                                      EXCEPTION_POINTERS* exception) {
  // This ensures that we don't have multiple colliding crashes being processed
  // simultaneously.
  base::AutoLock auto_lock(lock_);

  // Grab the global page protection lock to prevent page protection settings
  // from being modified while processing the error.
  ::common::AutoRecursiveLock lock(block_protect_lock);

  // This is needed for unittesting.
  runtime_->logger_->Write("SyzyASAN: Handling an exception.");

  // If we're bound to a runtime then look for heap corruption and
  // potentially augment the exception record. This needs to exist in the
  // outermost scope of this function as pointers to it may be passed to
  // other exception handlers.
  AsanErrorInfo error_info = {};

  // If this is set to true then an ASAN error will be emitted.
  bool emit_asan_error = false;

  // If this is an exception that we launched then extract the original
  // exception data and continue processing it.
  if (exception->ExceptionRecord->ExceptionCode == kAsanException) {
    ULONG_PTR* args = exception->ExceptionRecord->ExceptionInformation;
    DWORD code = args[0];
    DWORD flags = args[1];
    DWORD nargs = args[2];
    const ULONG_PTR* orig_args = reinterpret_cast<const ULONG_PTR*>(args[3]);

    // Rebuild the exception with the original exception data.
    exception->ExceptionRecord->ExceptionCode = code;
    exception->ExceptionRecord->ExceptionFlags = flags;
    exception->ExceptionRecord->NumberParameters = nargs;
    for (DWORD i = 0; i < nargs; ++i)
      args[i] = orig_args[i];
  } else if (runtime_) {
    // Initialize this as if heap corruption is the primary error being
    // reported. This will be overridden by the access violation handling
    // code below, if necessary.
    error_info.location = exception->ExceptionRecord->ExceptionAddress;
    error_info.context = *exception->ContextRecord;
    error_info.error_type = CORRUPT_HEAP;
    error_info.access_mode = ASAN_UNKNOWN_ACCESS;

    // It is possible that access violations are due to page protections of a
    // sufficiently large allocation. In this case the shadow will contain
    // block redzone markers at the given address. We take over the exception
    // if that is the case.
    if (exception->ExceptionRecord->ExceptionCode ==
            EXCEPTION_ACCESS_VIOLATION &&
        exception->ExceptionRecord->NumberParameters >= 2 &&
        exception->ExceptionRecord->ExceptionInformation[0] <= 1) {
      void* address = reinterpret_cast<void*>(
          exception->ExceptionRecord->ExceptionInformation[1]);
      ShadowMarker marker = Shadow::GetShadowMarkerForAddress(address);
      if (ShadowMarkerHelper::IsRedzone(marker) &&
          ShadowMarkerHelper::IsActiveBlock(marker)) {
        BlockInfo block_info = {};
        if (Shadow::BlockInfoFromShadow(address, &block_info)) {
          // Page protections have to be removed from this block otherwise our
          // own inspection will cause further errors.
          ScopedBlockAccess block_access(block_info);

          // Useful for unittesting.
          runtime_->logger_->Write("SyzyASAN: Caught an invalid access via "
              "an access violation exception.");

          // Override the invalid access location with the faulting address,
          // not the code address.
          error_info.location = address;
          // The exact access size isn't reported so simply set it to 1 (an
          // obvious lower bound).
          error_info.access_size = 1;
          // Determine if this is a read or a write using information in the
          // exception record.
          error_info.access_mode =
              exception->ExceptionRecord->ExceptionInformation[0] == 0 ?
              ASAN_READ_ACCESS : ASAN_WRITE_ACCESS;

          // Fill out the rest of the bad access information.
          ErrorInfoGetBadAccessInformation(runtime_->stack_cache(),
                                           &error_info);
          emit_asan_error = true;
        }
      }
    }

    CHECK_HEAP_CORRUPTION(runtime_, &error_info);
    if (error_info.heap_is_corrupt)
      emit_asan_error = true;
  }

  // If an ASAN error was detected then report it via the logger and take over
  // the exception record.
  EXCEPTION_RECORD record = {};
  if (emit_asan_error) {
    // Log the error via the usual means.
    runtime_->OnErrorImpl(&error_info);

    // If we have Breakpad integration then set our crash keys.
    if (breakpad_functions.crash_for_exception_ptr != NULL)
      SetCrashKeys(breakpad_functions, &error_info);

    // Remember the old exception record.
    EXCEPTION_RECORD* old_record = exception->ExceptionRecord;

    // Initialize the exception record and chain the original exception to it.
    InitializeExceptionRecord(&error_info, &record, exception);
    record.ExceptionRecord = old_record;
  }

  if (is_unhandled) {
    // Pass the buck to the next exception handler. If the process is Breakpad
    // enabled this will eventually make its way there.
    if (previous_uef_ != NULL)
      return (*previous_uef_)(exception);
  }

  // If we've found an ASAN error then pass the buck to Breakpad directly,
  // if possible. Otherwise, simply let things take their natural course.
  if (emit_asan_error && breakpad_functions.crash_for_exception_ptr)
    return (*breakpad_functions.crash_for_exception_ptr)(exception);

  // We can't do anything with this, so let the system deal with it.
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace asan
}  // namespace agent
