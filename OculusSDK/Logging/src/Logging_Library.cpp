/************************************************************************************

Filename    :   Logging.cpp
Content     :   Logging system
Created     :   Oct 26, 2015
Authors     :   Chris Taylor

Copyright   :   Copyright (c) Facebook Technologies, LLC and its affiliates. All rights reserved.

Licensed under the Oculus VR Rift SDK License Version 3.3 (the "License");
you may not use the Oculus VR Rift SDK except in compliance with the License,
which is provided at the time of installation or download, or which
otherwise accompanies this software in either electronic or hard copy form.

You may obtain a copy of the License at

http://www.oculusvr.com/licenses/LICENSE-3.3

Unless required by applicable law or agreed to in writing, the Oculus VR SDK
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

************************************************************************************/

#include "Logging/Logging_Library.h"
#include "Logging/Logging_OutputPlugins.h"

#pragma warning(push)
#pragma warning(disable: 4530) // C++ exception handler used, but unwind semantics are not enabled

#include <time.h>
#include <string.h>
#include <assert.h>

#pragma warning(push)

namespace ovrlog {


  //--------------------------------------------------------------------------------------------------
  // LogTime
  //--------------------------------------------------------------------------------------------------

  LogTime GetCurrentLogTime()
  {
#if defined(_WIN32)
    SYSTEMTIME t;
    ::GetLocalTime(&t);
#else
    time_t t = time(NULL);
#endif

    return t;
  }


//--------------------------------------------------------------------------------------------------
// RepeatedMessageManager
//--------------------------------------------------------------------------------------------------

RepeatedMessageManager::RepeatedMessageManager()
  : Mutex(), BusyInWrite(false), RecentMessageMap(), RepeatedMessageMap(), RepeatedMessageExceptionSet()
{}

void RepeatedMessageManager::PrintDeferredAggregateMessage(OutputWorker* outputWorker, RepeatedMessage& repeatedMessage){
  // Don't lock Mutex, as it's expected to already be locked.

  // Add the prefix to repeatedMessage->stream instead of writing it separately because a race 
  // condition could otherwise cause another message to be printed between the two.
  char prefixMessage[64]; // Impossible to overflow below.
  size_t prefixLength = snprintf(prefixMessage, sizeof(prefixMessage), "[Aggregated %d times] ", 
    repeatedMessage.aggregatedCount);
  repeatedMessage.stream.insert(0, prefixMessage, prefixLength);

  // We use WriteOption::DangerouslyIgnoreQueueLimit because it is very unlikely that these 
  // messages could be generated in a runaway fashion. But they are an aggregate and so are more 
  // important that their non-aggregated versions would be.
  BusyInWrite = true;
  outputWorker->Write(repeatedMessage.subsystemName.c_str(), repeatedMessage.messageLogLevel, 
      repeatedMessage.stream.c_str(), false, WriteOption::DangerouslyIgnoreQueueLimit);
  BusyInWrite = false;
}

RepeatedMessageManager::LogTimeMs RepeatedMessageManager::GetCurrentLogMillisecondTime() {
    const LogTime currentLogTime = GetCurrentLogTime();
    const LogTimeMs currentLogTimeMs = LogTimeToMillisecondTime(currentLogTime);
    return currentLogTimeMs;
}

RepeatedMessageManager::LogTimeMs RepeatedMessageManager::LogTimeToMillisecondTime(const LogTime& logTime)
{
#if defined(_WIN32)
    // Time is represented by SYSTEMTIME, which is a calendar time, with 1ms granularity.
    // We need to quickly subtract two SYSTEMTIMEs, which is hard to do because it contains
    // year, month, day, hour, minute, second, millisecond components. We could use the Windows
    // SystemTimeToFileTime function to convert SYSTEMTIME to FILETIME, which is an absolute
    // time with a single value, but that's an expensive conversion. 
    LogTimeMs logTimeMs = (logTime.wHour * 3600000) + (logTime.wMinute * 60000) + 
      (logTime.wSecond * 1000) + logTime.wMilliseconds;
    return logTimeMs;
#else
    // Time is represented by time_t, which is seconds, and thus a granularity of 1000ms.
    return (logTime * 1000);
#endif
}

int64_t RepeatedMessageManager::GetLogMillisecondTimeDifference(LogTimeMs begin, LogTimeMs end)
{
#if defined(_WIN32)
    if(end >= begin) // If the day didn't roll over between begin and end...
      return (end - begin);
    return (86400000 + (end - begin)); // Else assume exactly one day rolled over.
#else
    return (end - begin);
#endif
}

  RepeatedMessageManager::PrefixHashType RepeatedMessageManager::GetHash(const char* p) {
    // Fowler / Noll / Vo (FNV) Hash
    // FNV is a great string hash for reduction of collisions, but the cost benefit is high.
    // We can get away with a fairly poor string hash here which is very fast.
    //
    PrefixHashType hash(2166136261U);
    const char* pEnd = (p + messagePrefixLength);
    while (*p && (p < pEnd)) {
      hash += (hash << 1) + (hash << 4) + (hash << 7) + (hash << 8) + (hash << 24);
      hash ^= (uint8_t)*p++;
    }
    return hash;

    // uint32_t hash(0); // To do: See if this hash is too poor.
    // const char* pEnd = (p + messagePrefixLength);
    // while(*p && (p < pEnd))
    //    hash += (uint8_t)*p++;
    // return hash;
  }

RepeatedMessageManager::HandleResult
RepeatedMessageManager::HandleMessage(const char* subsystemName, Level messageLogLevel, 
  const char* stream)
{
  std::lock_guard<std::recursive_mutex> lock(Mutex);

  if (BusyInWrite) // If we are here due to our own call of OutputWorker::Write from our Poll func..
    return HandleResult::Passed;

  PrefixHashType prefixHash = GetHash(stream);

  // Check to see if we have this particular message in an exception list.
  if (RepeatedMessageExceptionSet.find(prefixHash) != RepeatedMessageExceptionSet.end()) {
    return HandleResult::Passed;
  }

  PrefixHashType subsystemNameHash = GetHash(subsystemName);
  if (RepeatedMessageSubsystemExceptionSet.find(subsystemNameHash) != RepeatedMessageSubsystemExceptionSet.end()) {
    return HandleResult::Passed;
  }

  // We will need the current time below for all pathways.
  const LogTimeMs currentLogTimeMs = GetCurrentLogMillisecondTime();

  // First look at our repeated messages. This is a container of known repeating messages.
  auto itRepeated = RepeatedMessageMap.find(prefixHash);

  if (itRepeated != RepeatedMessageMap.end()) { // If this is a message that's already repeating...
    RepeatedMessage& repeatedMessage = itRepeated->second;

    // Assume subsystemName == repeatedMessage->subsystemName, though theoretically it's possible
    // that two subsystems generate the same prefix string. Let's worry about that if we see it.
    //
    // Assume messageLogLevel == repeatedMessage->messageLogLevel for the purposes of handling
    // repeated messages. It's possible that a subsystem may generate the same message string
    // but with different log levels, but we've never seen that, and it may not be significant
    // with respect to handling repeating anyway. Let's worry about that if we see it.
    const LogTimeMs logTimeDifferenceMs = 
      GetLogMillisecondTimeDifference(repeatedMessage.lastTimeMs, currentLogTimeMs);

    if (logTimeDifferenceMs < maxDeferrableDetectionTimeMs) { // If this message was soon after
      repeatedMessage.lastTimeMs = currentLogTimeMs;          // the last one...

      // We actually print the first few seemingly repeated messages before deferring them.
      if (repeatedMessage.printedCount < printedRepeatCount) {
        repeatedMessage.printedCount++;
        return HandleResult::Passed;
      }

      // Else we aggregate it, and won't print it until later with a summary printing.
      // If repeatedMessage.aggregatedCount >= maxDeferredMessages, then  copy stream to 
      // repeatedMessage.stream, in order to print the most recent variation of this repeat when 
      // the aggregated print is done.
      if (++repeatedMessage.aggregatedCount >= maxDeferredMessages)
        repeatedMessage.stream = stream;

      return HandleResult::Aggregated;
    }
    // Else the repeated message was old and we don't don't consider this a repeat.
    // Don't erase the entry from RepeatedMessageMap here, as we still need to do a final 
    // print of the aggregated message before removing it. We'll handle that in the Poll function.
  }
  else {
    // Else this message wasn't known to be previously repeating, but maybe it's the first repeat
    // we are encountering. Check the RecentMessageMap for this.
    auto itRecent = RecentMessageMap.find(prefixHash);

    if (itRecent != RecentMessageMap.end()) { // If it looks like a repeat of something recent...
      RepeatedMessageMap[prefixHash] = RepeatedMessage(
        subsystemName, messageLogLevel, stream, currentLogTimeMs, currentLogTimeMs, 0);

      // No need to keep it in the RecentMessageMap any more, since it's now classified as repeat.
      RecentMessageMap.erase(itRecent);
    }
    else {
      // Else add it to RecentMessageMap. Old RecentMessageMap entries will be removed by Poll().
      RecentMessageMap[prefixHash] = RecentMessage{ currentLogTimeMs };
    }
  }

  return HandleResult::Passed;
}

void RepeatedMessageManager::Poll(OutputWorker* outputWorker) {
  std::vector<RepeatedMessage> messagesToPrint;

  {
    std::lock_guard<std::recursive_mutex> lock(Mutex);

    if (RecentMessageMap.size() > (recentMessageCount * 2)) {
      // Prune the oldest messages out of RecentMessageMap until 
      // RecentMessageMap.size() == recentMessageCount. Unfortunately, RecentMessageMap is an
      // unsorted hash container, so we can't quickly find the oldest N messages. We can solve 
      // this by doing a copy of iterators to an array, sort, erase first N iterators. A faster 
      // solution would be to keep a std::queue of iterators that were added to the map, though 
      // that results in some complicated code. Maybe we'll do that, but let's do the sort 
      // solution first.
      const size_t arrayCapacity = (recentMessageCount * 3);
      RecentMessageMapType::iterator itArray[arrayCapacity]; // Avoid memory allocation.
      size_t itArraySize = 0;

      for (RecentMessageMapType::iterator it = RecentMessageMap.begin();
        (it != RecentMessageMap.end()) && (itArraySize < arrayCapacity);
        ++it) {
        itArray[itArraySize++] = it;
      }

      std::sort(
        itArray,
        itArray + itArraySize, // Put the oldest at the end of the array.
        [](const RecentMessageMapType::iterator& it1,
          const RecentMessageMapType::iterator& it2) -> bool {
        return (it2->second.timeMs < it1->second.timeMs); // Sort newest to oldest.
      });

      for (size_t i = 0; i < itArraySize; ++i)
        RecentMessageMap.erase(itArray[i]);
    }

    // Currently we go through the entire RepeatedMessageMap every time we are here, though we 
    // have a purgeDeferredMessageTimeMs constant which we have to make this more granular, for 
    // efficiency purposed. To do.
    const LogTimeMs currentLogTimeMs = GetCurrentLogMillisecondTime();

    for (auto it = RepeatedMessageMap.begin(); it != RepeatedMessageMap.end();) {
      RepeatedMessage& repeatedMessage = it->second;
      LogTimeMs logTimeDifferenceMs =
        GetLogMillisecondTimeDifference(repeatedMessage.lastTimeMs, currentLogTimeMs);

      // If this message hasn't repeated in a while...
      if (logTimeDifferenceMs > maxDeferrableDetectionTimeMs) {
        // Print an aggregate result for this entry before removing it.
        // Since we have already printed the first <printedRepeatCount> of a given repeated message,
        // we do an aggregate print only if further printings were called for.
        if (repeatedMessage.aggregatedCount) { // If there are any aggregated messages deferred...
          // This will print the first variation of the string that was encountered.
          // We can move the message instead of copy because we won't need it any more after this.
          messagesToPrint.emplace_back(std::move(repeatedMessage));
        }

        it = RepeatedMessageMap.erase(it);
        continue;
      }
      else if (repeatedMessage.aggregatedCount >= maxDeferredMessages) {
        messagesToPrint.emplace_back(repeatedMessage);
        repeatedMessage.printedCount += repeatedMessage.aggregatedCount;
        repeatedMessage.aggregatedCount = 0; // Reset this for a new round of aggregation.
      }
      ++it;
    }
  } // lock

  // We need to print these messages outside our locked Mutex because printing of these is
  // calling out to external code which itself has mutexes and thus we need to avoid deadlock.
  for (auto& repeatedMessage : messagesToPrint)
    PrintDeferredAggregateMessage(outputWorker, repeatedMessage);
}

void RepeatedMessageManager::AddRepeatedMessageException(const char* messagePrefix) {
  std::lock_guard<std::recursive_mutex> lock(Mutex);

  PrefixHashType prefixHash = GetHash(messagePrefix);
  RepeatedMessageExceptionSet.insert(prefixHash);
}

void RepeatedMessageManager::RemoveRepeatedMessageException(const char* messagePrefix) {
  std::lock_guard<std::recursive_mutex> lock(Mutex);

  PrefixHashType prefixHash = GetHash(messagePrefix);
  auto it = RepeatedMessageExceptionSet.find(prefixHash);
  if (it != RepeatedMessageExceptionSet.end()) {
    RepeatedMessageExceptionSet.erase(it);
  }
}


void RepeatedMessageManager::AddRepeatedMessageSubsystemException(const char* subsystemName) {
  std::lock_guard<std::recursive_mutex> lock(Mutex);

  PrefixHashType subsystemNameHash = GetHash(subsystemName);
  RepeatedMessageSubsystemExceptionSet.insert(subsystemNameHash);
}

void RepeatedMessageManager::RemoveRepeatedMessageSubsytemException(const char* subsystemName) {
  std::lock_guard<std::recursive_mutex> lock(Mutex);

  PrefixHashType subsystemNameHash = GetHash(subsystemName);
  auto it = RepeatedMessageSubsystemExceptionSet.find(subsystemNameHash);
  if (it != RepeatedMessageExceptionSet.end()) {
    RepeatedMessageExceptionSet.erase(it);
  }
}

//-----------------------------------------------------------------------------
// Channel

OutputWorkerOutputFunctionType Channel::OutputWorkerOutputFunction;
ConfiguratorOnChannelLevelChangeType Channel::ConfiguratorOnChannelLevelChange;
ConfiguratorRegisterType Channel::ConfiguratorRegister;
ConfiguratorUnregisterType Channel::ConfiguratorUnregister;

// Don't use locks or register channels until OutputWorker::Start has been called
// Once called the first time, register all known channels and start using locks
static volatile bool OutputWorkerInstValid = false;
static ChannelNode* ChannelNodeHead = nullptr;
void ChannelRegisterNoLock(ChannelNode* channelNode)
{
    if (ChannelNodeHead)
    {
        ChannelNodeHead->Prev = channelNode;
        channelNode->Next = ChannelNodeHead;
        channelNode->Prev = nullptr;
        ChannelNodeHead = channelNode;
    }
    else
    {
        ChannelNodeHead = channelNode;
        channelNode->Next = nullptr;
        channelNode->Prev = nullptr;
    }
}

void ChannelRegister(ChannelNode* channelNode)
{
    Locker locker(OutputWorker::GetInstance()->GetChannelsLock());
    ChannelRegisterNoLock(channelNode);
    Configurator::GetInstance()->RestoreChannelLogLevel(channelNode);
}

void ChannelUnregisterNoLock(ChannelNode* channelNode)
{
    if (channelNode == ChannelNodeHead)
    {
        ChannelNodeHead = channelNode->Next;
        if (ChannelNodeHead)
        {
            ChannelNodeHead->Prev = nullptr;
        }
    }
    else
    {
        channelNode->Prev->Next = channelNode->Next;
        if (channelNode->Next)
        {
            channelNode->Next->Prev = channelNode->Prev;
        }
    }
}

void ChannelUnregister(ChannelNode* channelNode)
{
    Locker locker(OutputWorker::GetInstance()->GetChannelsLock());

    ChannelUnregisterNoLock(channelNode);
}

// Export Write(), and three configurator functions so that Channel has access to it automatically across DLL boundaries
// These functions are looked up using GetProcAddress(GetModuleHandle(NULL), "FunctionName");
extern "C"
{
    void OutputWorkerOutputFunctionC(const char* subsystemName, Log_Level_t messageLogLevel, const char* stream, bool relogged, Write_Option_t option)
    {
        OutputWorker::GetInstance()->Write(subsystemName, (Level)messageLogLevel, stream, relogged, (WriteOption)option);
    }

    void ConfiguratorOnChannelLevelChangeC(const char* channelName, Log_Level_t level)
    {
        Configurator::GetInstance()->OnChannelLevelChange(channelName, level);
    }

    void ConfiguratorRegisterC(ChannelNode* channelNode)
    {
        if (OutputWorkerInstValid == false)
            ChannelRegisterNoLock(channelNode);
        else
            ChannelRegister(channelNode);
    }

    void ConfiguratorUnregisterC(ChannelNode* channelNode)
    {
        if (OutputWorkerInstValid == false)
            ChannelUnregisterNoLock(channelNode);
        else
            ChannelUnregister(channelNode);
    }
}

//-----------------------------------------------------------------------------
// Shutdown the logging system and release memory
void ShutdownLogging()
{
    // This function needs to be robust to multiple calls in a row
    if (OutputWorkerInstValid)
    {
        ovrlog::OutputWorker::GetInstance()->Stop();
    }
}

void RestartLogging()
{
  if (OutputWorkerInstValid)
  {
    ovrlog::OutputWorker::GetInstance()->Start();
  }
}


//-----------------------------------------------------------------------------
// Log Output Worker Thread

OutputWorker* OutputWorker::GetInstance()
{
    static OutputWorker worker;
    return &worker;
}

void OutputWorkerAtExit()
{
    // This function needs to be robust to multiple calls in a row
    ShutdownLogging();
}

OutputWorker::OutputWorker() :
    IsInDebugger(false),
    PluginsLock(),
    Plugins(),
    WorkerWakeEvent(),
    WorkQueueLock(),
    WorkQueueOverrun(0),
    StartStopLock(),
    WorkerTerminator(),
    LoggingThread()
{
    #if defined(_WIN32)
        // Create a worker wake event
        WorkerWakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    #else
        // To do: Implement this.
    #endif
    
    IsInDebugger = IsDebuggerAttached();

    InstallDefaultOutputPlugins();

    OutputWorkerInstValid = true;

    Start();
}

OutputWorker::~OutputWorker()
{
    Stop();
    OutputWorkerInstValid = false;
}

void OutputWorker::InstallDefaultOutputPlugins()
{
    // These are the default outputs for ALL applications:

    // If debugger is *not* attached,
    if (!IsInDebugger)
    {
        // Enable event log output.  This logger is fairly slow, taking about 1 millisecond per log,
        // and this is very expensive to flush after each log message for debugging.  Since we almost
        // never use the Event Log when debugging apps it is better to simply leave it out.
#ifdef OVR_ENABLE_OS_EVENT_LOG
        AddPlugin(std::make_shared<OutputEventLog>());
#endif

        // Do not log to the DbgView output from the worker thread.  When a debugger is attached we
        // instead flush directly to the DbgView log so that the messages are available at breakpoints.
        AddPlugin(std::make_shared<OutputDbgView>());
    }

    #if defined(_WIN32)
        // If there is a console window,
        if (::GetConsoleWindow() != NULL)
        {
            // Enable the console.  This logger takes 3 milliseconds per message, so it is fairly
            // slow and should be avoided if it is not needed (ie. console is not shown).
            AddPlugin(std::make_shared<OutputConsole>());
        }
    #endif
}

void OutputWorker::AddPlugin(std::shared_ptr<OutputPlugin> plugin)
{
    if (!plugin)
    {
        return;
    }

    Locker locker(PluginsLock);

    RemovePlugin(plugin);

    Plugins.insert(plugin);
}

void OutputWorker::RemovePlugin(std::shared_ptr<OutputPlugin> pluginToRemove)
{
    if (!pluginToRemove)
    {
        return;
    }

    const char* nameOfPluginToRemove = pluginToRemove->GetUniquePluginName();

    Locker locker(PluginsLock);

    for (auto& existingPlugin : Plugins)
    {
        const char* existingPluginName = existingPlugin->GetUniquePluginName();

        // If the names match exactly,
        if (0 == strcmp(nameOfPluginToRemove, existingPluginName))
        {
            Plugins.erase(existingPlugin);
            break;
        }
    }
}

std::shared_ptr<OutputPlugin> OutputWorker::GetPlugin(const char* const pluginName)
{
    Locker locker(PluginsLock);

    for (auto& existingPlugin : Plugins)
    {
        const char* const existingPluginName = existingPlugin->GetUniquePluginName();

        // If the names match exactly,
        if (0 == strcmp(pluginName, existingPluginName))
        {
            return existingPlugin;
        }
    }

    return nullptr;
}

void OutputWorker::DisableAllPlugins()
{
    Locker locker(PluginsLock);

    Plugins.clear();
}

Lock* OutputWorker::GetChannelsLock()
{
    return &ChannelsLock;
}

void OutputWorker::AddRepeatedMessageSubsystemException(const char* subsystemName) {
    return RepeatedMessageManagerInstance.AddRepeatedMessageSubsystemException(subsystemName);
}

void OutputWorker::RemoveRepeatedMessageSubsystemException(const char* subsystemName) {
    return RepeatedMessageManagerInstance.RemoveRepeatedMessageSubsytemException(subsystemName);
}

void OutputWorker::Start()
{
    // Hold start-stop lock to prevent Start() and Stop() from being called at the same time.
    Locker startStopLocker(StartStopLock);

    // If already started,
    if (LoggingThread.IsValid())
    {
        return; // Nothing to do!
    }

    // RestoreAllChannelLogLevelsNoLock is used to address http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2008/n2660.htm
    // Section 6.7
    // RestoreAllChannelLogLevelsNoLock otherwise invokes OutputWorker::GetInstance() whose constructor hasn't finished yet
    Configurator::GetInstance()->RestoreAllChannelLogLevelsNoLock();

    if (!WorkerTerminator.Initialize())
    {
        // Unable to create worker terminator event?
        LOGGING_DEBUG_BREAK();
        return;
    }

    #if defined(_WIN32)
        LoggingThread = ::CreateThread(
            nullptr, // No thread security attributes
            0, // Default stack size
            &OutputWorker::WorkerThreadEntrypoint_, // Thread entrypoint
            this, // This parameter
            0, // No creation flags, start immediately
            nullptr); // Do not request thread id
    #else
        // To do: Implement this, and probably convert thread usage here to C++ threads.
    #endif
    
    if (!LoggingThread.IsValid())
    {
        // Unable to create worker thread?
        LOGGING_DEBUG_BREAK();
        return;
    }

    // Note this may queue more than one OutputWorkerAtExit() call.
    // This function needs to be robust to multiple calls in a row
    std::atexit(&OutputWorkerAtExit);
}

void OutputWorker::Stop()
{
    // This function needs to be robust to multiple calls in a row

    // Hold start-stop lock to prevent Start() and Stop() from being called at the same time.
    Locker startStopLocker(StartStopLock);

    if (LoggingThread.IsValid())
    {
        // Flag termination
        WorkerTerminator.Terminate();

        // Wait for thread to end
        #if defined(_WIN32)
            ::WaitForSingleObject(
                LoggingThread.Get(), // Thread handle
                INFINITE); // Wait forever for thread to terminate
        #else
            // To do: Implement this, and probably convert thread usage here to C++ threads.
        #endif

        LoggingThread.Clear();
    }

    // Hold scoped work queue lock:
    {
        // This ensures that logs are not printed out of order on Stop(), and that Flush()
        // can use the flag to check if a flush has already occurred.
        Locker workQueueLock(WorkQueueLock);

        // Finish the last set of queued messages to avoid losing any before Stop() returns.
        ProcessQueuedMessages();
    }
}



static int GetTimestamp(char* buffer, int bufferBytes, const LogTime& logTime)
{
#if defined(_WIN32)
    // GetDateFormat and GetTimeFormat returns the number of characters written to the
    // buffer if successful, including the trailing '\0'; and return 0 on failure.
    char dateBuffer[16];
    int  dateBufferLength;
    int  writtenChars = ::GetDateFormatA(LOCALE_USER_DEFAULT, 0, &logTime, "dd/MM ", dateBuffer, sizeof(dateBuffer));

    if (writtenChars <= 0)
    {
        // Failure
        buffer[0] = '\0';
        return 0;
    }
    dateBufferLength = (writtenChars - 1);

    char timeBuffer[32];
    int  timeBufferLength;
    writtenChars = ::GetTimeFormatA( // Intentionally using 'A' version.
        LOCALE_USER_DEFAULT, // User locale
        0, // Default flags
        &logTime, // Time
        "HH:mm:ss",
        timeBuffer, // Output buffer
        sizeof(timeBuffer)); // Size of buffer in tchars

    if (writtenChars <= 0)
    {
        // Failure
        buffer[0] = '\0';
        return 0;
    }
    timeBufferLength = (writtenChars - 1);

    // Append milliseconds
    const char msBuffer[5] =
    {
        (char)('.'),
        (char)(((logTime.wMilliseconds / 100) % 10) + '0'),
        (char)(((logTime.wMilliseconds / 10) % 10) + '0'),
        (char)((logTime.wMilliseconds % 10) + '0'),
        (char)('\0')
    };

    const int writeSum = (dateBufferLength + timeBufferLength + sizeof(msBuffer));

    if (bufferBytes < writeSum)
    {
        buffer[0] = '\0';
        return 0;
    }

    #pragma warning(push)           // We are guaranteed that strcpy is safe.
    #pragma warning(disable: 4996)  //'strcpy': This function or variable may be unsafe.
    strcpy(buffer, dateBuffer);
    strcpy(buffer + dateBufferLength, timeBuffer);
    strcpy(buffer + dateBufferLength + timeBufferLength, msBuffer);
    #pragma warning(pop)

    return (writeSum - 1); // -1 because we return the strlen of buffer, and don't include the trailing '\0'.
#else
    snprintf(buffer, bufferBytes, "%llu", (uint64_t)logTime);
    return (int)strlen(buffer);
#endif
}



// Returns number of bytes written to buffer
// Precondition: Buffer is large enough to hold everything,
// so don't bother complaining there isn't enough length checking.
static int GetTimestamp(char* buffer, int bufferBytes)
{
    LogTime time = GetCurrentLogTime();
    return GetTimestamp(buffer, bufferBytes, time);
}


void OutputWorker::Flush()
{
    if (!LoggingThread.IsValid())
    {
        LOGGING_DEBUG_BREAK(); // Must be called between Start() and Stop()
        return;
    }

    #if defined(_WIN32)
        AutoHandle flushEvent;

        // Scoped work queue lock:
        {
            Locker workQueueLock(WorkQueueLock);

            // Generate a flush event
            flushEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            LogStringBuffer buffer("Logging", ovrlog::Level::Info);
            LogTime time = GetCurrentLogTime();
            QueuedLogMessage* queuedBuffer = new QueuedLogMessage("Logging", ovrlog::Level::Info, "", time);
            queuedBuffer->FlushEvent = flushEvent.Get();

            // Add queued buffer to the end of the work queue
            WorkQueueAdd(queuedBuffer);

            // Wake the worker thread
            ::SetEvent(WorkerWakeEvent.Get());
        }

        // Wait until the event signals.
        // Since we are guaranteed to never lose log messages, as late as Stop() being called,
        // this cannot cause a hang.
        ::WaitForSingleObject(flushEvent.Get(), INFINITE);
    #else
        // To do: Implement this.
    #endif // _WIN32
}

static void WriteAdvanceStrCpy(char*& buffer, size_t& bufferBytes, const char* str)
{
    // Get length of string to copy into buffer
    size_t slen = strlen(str);

    // If the resulting buffer cannot accommodate the string and a null terminator,
    if (bufferBytes < slen + 1)
    {
        // Do nothing
        return;
    }

    // Copy string to buffer
    memcpy(buffer, str, slen);

    // Advance buffer by number of bytes copied
    buffer += slen;
    bufferBytes -= slen;
}

void OutputWorker::AppendHeader(char* buffer, size_t bufferBytes, Level level, const char* subsystemName)
{
    // Writes <L> [SubSystem] to the provided buffer.

    // Based on message log level,
    const char* initial = "";
    switch (level)
    {
    case Level::Disabled: initial = " {DISABLED}["; break; // This typically should not occur, but we have here for consistency.
    case Level::Trace:    initial = " {TRACE}   ["; break;
    case Level::Debug:    initial = " {DEBUG}   ["; break;
    case Level::Info:     initial = " {INFO}    ["; break;
    case Level::Warning:  initial = " {WARNING} ["; break;
    case Level::Error:    initial = " {!ERROR!} ["; break;
    default:              initial = " {???}     ["; break;
    }
    static_assert(Level::Count == static_cast<Level>(6), "Needs updating");

    WriteAdvanceStrCpy(buffer, bufferBytes, initial);
    WriteAdvanceStrCpy(buffer, bufferBytes, subsystemName);
    WriteAdvanceStrCpy(buffer, bufferBytes, "] ");
    buffer[0] = '\0';
}

OVR_THREAD_FUNCTION_TYPE OutputWorker::WorkerThreadEntrypoint_(void* vworker)
{
    // Invoke thread entry-point
    OutputWorker* worker = reinterpret_cast<OutputWorker*>(vworker);
    if (worker)
    {
        worker->WorkerThreadEntrypoint();
    }
    return 0;
}

void OutputWorker::ProcessQueuedMessages()
{
    // Potentially trigger aggregated repeating messages.
    RepeatedMessageManagerInstance.Poll(this);

    static const int TempBufferBytes = 1024; // 1 KiB
    char HeaderBuffer[TempBufferBytes];

    QueuedLogMessage* message = nullptr;

    // Pull messages off the queue
    int lostCount = 0;
    {
        Locker locker(WorkQueueLock);
        message = WorkQueueHead;
        WorkQueueHead = WorkQueueTail = nullptr;
        lostCount = WorkQueueOverrun;
        WorkQueueOverrun = 0;
        WorkQueueSize = 0;
    }

    if (message == nullptr)
    {
        // No data to process
        return;
    }

    // Log output format:
    // TIMESTAMP <L> [SubSystem] Message

    // If some messages were lost,
    if (lostCount > 0)
    {
        char str[255];
        snprintf(str, sizeof(str), "Lost %i log messages due to queue overrun; try to reduce the amount of logging", lostCount);

        // Insert at the front of the list
        LogTime t = GetCurrentLogTime();
        QueuedLogMessage* queuedMsg = new QueuedLogMessage("Logging", Level::Error, str, t);
        queuedMsg->Next = message;
        message = queuedMsg;
    }

    {
        Locker locker(PluginsLock);

        // For each log message,
        for (QueuedLogMessage* next; message; message = next)
        {
            // If the message is a flush event,
            if (message->FlushEvent != nullptr)
            {
                // Signal it to wake up the waiting Flush() call.
                #if defined(_WIN32)
                    ::SetEvent(message->FlushEvent);
                #else
                    // To do: Implement this. Ideally switch this OutputWorker class to use std::condition_variable
                #endif
            }
            else
            {
                std::size_t timestampLength = GetTimestamp(HeaderBuffer, sizeof(HeaderBuffer), message->Time);

                // Construct header on top of timestamp buffer
                AppendHeader(HeaderBuffer + timestampLength, sizeof(HeaderBuffer) - timestampLength,
                    message->MessageLogLevel, message->SubsystemName.Get());

                // For each plugin,
                for (auto& plugin : Plugins)
                {
                    plugin->Write(
                        message->MessageLogLevel,
                        message->SubsystemName.Get(),
                        HeaderBuffer,
                        message->Buffer.c_str());
                }
            }

            next = message->Next;
            delete message;
        }
    }
}

void OutputWorker::FlushDbgViewLogImmediately(const char* subsystemName, Level messageLogLevel, const char* stream)
{
    static const int TempBufferBytes = 1024; // 1 KiB
    char HeaderBuffer[TempBufferBytes];

    // Get timestamp string
    int timestampLength = GetTimestamp(HeaderBuffer, TempBufferBytes);
    if (timestampLength <= 0)
    {
        LOGGING_DEBUG_BREAK(); return; // Maybe bug in timestamp code?
    }

    // Construct log header on top of timestamp buffer
    AppendHeader(HeaderBuffer + timestampLength, sizeof(HeaderBuffer) - timestampLength,
                 messageLogLevel, subsystemName);

    // Build up a single string to send to OutputDebugStringA so it
    // all appears on the same line in DbgView.
    std::stringstream ss;
    ss << HeaderBuffer << stream << "\n";

    #if defined(_WIN32)
        ::OutputDebugStringA(ss.str().c_str());
    #else
        fputs(ss.str().c_str(), stderr);
    #endif
}

static void SetThreadName(const char* name)
{
    #if defined(_WIN32)
        DWORD threadId = ::GetCurrentThreadId();

        // http://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
        #pragma pack(push,8)
        struct THREADNAME_INFO {
            DWORD  dwType;     // Must be 0x1000
            LPCSTR szName;     // Pointer to name (in user address space)
            DWORD  dwThreadID; // Thread ID (-1 for caller thread)
            DWORD  dwFlags;    // Reserved for future use; must be zero
        };
        union TNIUnion
        {
            THREADNAME_INFO tni;
            ULONG_PTR       upArray[4];
        };
        #pragma pack(pop)

        TNIUnion tniUnion = { { 0x1000, name, threadId, 0 } };

        __try
        {
            RaiseException(0x406D1388, 0, ARRAYSIZE(tniUnion.upArray), tniUnion.upArray);
        }
        __except (GetExceptionCode() == 0x406D1388 ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
    #elif defined (__APPLE__)
        pthread_setname_np(name);
    #else
        pthread_setname_np(pthread_self(), name);
    #endif
}

void OutputWorker::WorkerThreadEntrypoint()
{
    SetThreadName("LoggingOutputWorker");

    #if defined(_WIN32)
        // Lower the priority for logging.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    #else
        // Other desktop platforms (e.g. Linux, OSX) don't let you set thread priorities.
    #endif
    
    while (!WorkerTerminator.IsTerminated())
    {
        if (WorkerTerminator.WaitOn(WorkerWakeEvent.Get()))
        {
            ProcessQueuedMessages();
        }
    }
}

void OutputWorker::Write(const char* subsystemName, Level messageLogLevel, const char* stream, bool relogged, WriteOption option)
{
    bool dropped = false; // Flag indicates if the message was dropped due to queue overrun
    bool needToWakeWorkerThread = false; // Flag indicates if we need to wake the worker thread

    // Add work to queue.
    {
        Locker locker(WorkQueueLock);

        // Check to see if this message looks like it's repeat message which we want to aggregate
        // in order to avoid log spam of the same similar message repeatedly.
        if (RepeatedMessageManagerInstance.HandleMessage(subsystemName, messageLogLevel, stream) == 
            RepeatedMessageManager::HandleResult::Aggregated)
        {
          return;
        }

        if (option != WriteOption::DangerouslyIgnoreQueueLimit &&
            WorkQueueSize >= WorkQueueLimit)
        {
            // Record drop
            WorkQueueOverrun++;
            dropped = true;
        }
        else
        {
            // Add queued buffer to the end of the work queue
            LogTime time = GetCurrentLogTime();
            WorkQueueAdd(new QueuedLogMessage(subsystemName, messageLogLevel, stream, time));

            // Only need to wake the worker thread on the first message
            // The SetEvent() call takes 6 microseconds or so
            if (WorkQueueSize <= 1)
            {
                needToWakeWorkerThread = true;
            }
        }
    }

    if (!dropped && needToWakeWorkerThread)
    {
        // Wake the worker thread
        #if defined(_WIN32)
            ::SetEvent(WorkerWakeEvent.Get());
        #else
            // To do: Implement this. Ideally switch this OutputWorker class to use std::condition_variable
        #endif
    }

    // If this is the first time logging this message,
    if (!relogged)
    {
        // If we are in a debugger,
        if (IsInDebugger)
        {
            FlushDbgViewLogImmediately(subsystemName, messageLogLevel, stream);
        }
    }
}

//-----------------------------------------------------------------------------
// QueuedLogMessage

OutputWorker::QueuedLogMessage::QueuedLogMessage(const char* subsystemName, Level messageLogLevel, const char* stream, const LogTime& time) :
    SubsystemName(subsystemName),
    MessageLogLevel(messageLogLevel),
    Buffer(stream),
    Time(time),
    Next(nullptr),
    FlushEvent(nullptr)
{
}

void Channel::GetFunctionPointers()
{
    static bool gotFunctionPointers = false;
    if (gotFunctionPointers == false)
    {
        #if defined(_WIN32)
            OutputWorkerOutputFunction = (OutputWorkerOutputFunctionType)GetProcAddress(GetModuleHandle(NULL), "OutputWorkerOutputFunctionC");
            ConfiguratorOnChannelLevelChange = (ConfiguratorOnChannelLevelChangeType)GetProcAddress(GetModuleHandle(NULL), "ConfiguratorOnChannelLevelChangeC");
            ConfiguratorRegister = (ConfiguratorRegisterType)GetProcAddress(GetModuleHandle(NULL), "ConfiguratorRegisterC");
            ConfiguratorUnregister = (ConfiguratorUnregisterType)GetProcAddress(GetModuleHandle(NULL), "ConfiguratorUnregisterC");
        #else
            // To do.
        #endif
        
        if (!OutputWorkerOutputFunction)
            OutputWorkerOutputFunction = OutputWorkerOutputFunctionC;

        if (!ConfiguratorOnChannelLevelChange)
            ConfiguratorOnChannelLevelChange = ConfiguratorOnChannelLevelChangeC;

        if (!ConfiguratorRegister)
            ConfiguratorRegister = ConfiguratorRegisterC;

        if (!ConfiguratorUnregister)
            ConfiguratorUnregister = ConfiguratorUnregisterC;

        gotFunctionPointers = true;
    }
}

void Channel::registerNode()
{
    Node.SubsystemName = SubsystemName.Get();
    Node.Level = &MinimumOutputLevel;
    Node.UserOverrodeMinimumOutputLevel = &UserOverrodeMinimumOutputLevel;

    GetFunctionPointers();

    ConfiguratorRegister(&Node);
}

Channel::Channel(const char* nameString) :
    SubsystemName(nameString),
    MinimumOutputLevel((Log_Level_t)DefaultMinimumOutputLevel),
    UserOverrodeMinimumOutputLevel(false)
{
    //OutputDebugStringA((std::string(">>>ctor Channel::Channel ") + SubsystemName.Get() + "\n").c_str());
    registerNode();
}

Channel::Channel(const Channel& other) :
    SubsystemName(other.SubsystemName),
    MinimumOutputLevel(other.MinimumOutputLevel),
    Prefix(other.GetPrefix()),
    UserOverrodeMinimumOutputLevel(other.UserOverrodeMinimumOutputLevel)
{
    //OutputDebugStringA((std::string(">>>copy ctor Channel::Channel ") + SubsystemName.Get() + "\n").c_str());
    registerNode();
}

Channel::~Channel()
{
    // ...We can get modified from other threads here.
    ConfiguratorUnregister(&Node);
}

std::string Channel::GetPrefix() const
{
    Locker locker(PrefixLock);
    return Prefix;
}

void Channel::SetPrefix(const std::string& prefix)
{
    Locker locker(PrefixLock);
    Prefix = prefix;
}

void Channel::SetMinimumOutputLevel(Level newLevel)
{
    SetMinimumOutputLevelNoSave(newLevel);

    ConfiguratorOnChannelLevelChange(SubsystemName.Get(), MinimumOutputLevel);
}

void Channel::SetMinimumOutputLevelNoSave(Level newLevel)
{
    MinimumOutputLevel = (Log_Level_t)newLevel;
    UserOverrodeMinimumOutputLevel = true;
}

Level Channel::GetMinimumOutputLevel() const
{
    return (Level) MinimumOutputLevel;
}

//-----------------------------------------------------------------------------
// Conversion functions

template<>
void LogStringize(LogStringBuffer& buffer, const wchar_t* const & first)
{
#ifdef _WIN32

    // Use Windows' optimized multi-byte UTF8 conversion function for performance.
    // Returns the number of bytes used by the conversion, including the null terminator
    // since -1 is passed in for the input string length.
    // Returns 0 on failure.
    int bytesUsed = ::WideCharToMultiByte(
        CP_ACP, // Default code page
        0, // Default flags
        first, // String to convert
        -1, // Unknown string length
        nullptr, // Null while checking length of buffer
        0, // 0 to request the buffer size required
        nullptr, // Default default character
        nullptr); // Ignore whether or not default character was used
    // Setting the last two arguments to null is documented to execute faster via MSDN.

    // If the function succeeded,
    if (bytesUsed > 0)
    {
        // Avoid allocating memory if the string is fairly small.
        char stackBuffer[128];
        char* dynamicBuffer = nullptr;
        char* convertedString = stackBuffer;
        if (bytesUsed > (int)sizeof(stackBuffer))
        {
            // (defensive coding) Add 8 bytes of slop in case of API bugs.
            dynamicBuffer = new char[bytesUsed + 8];
            convertedString = dynamicBuffer;
        }

        int charsWritten = ::WideCharToMultiByte(
            CP_ACP, // Default code page
            0, // Default flags
            first, // String to convert
            -1, // Unknown string length
            convertedString, // Output buffer
            bytesUsed, // Request the same number of bytes
            nullptr, // Default default character
            nullptr); // Ignore whether or not default character was used
        // Setting the last two arguments to null is documented to execute faster via MSDN.

        if (charsWritten > 0)
        {
            // Append the converted string.
            buffer.Stream << convertedString;
        }

        delete[] dynamicBuffer;
    }

#else

    fprintf(stderr, __FILE__ "::[%s] Not implemented.\n", __func__);
    assert(0); // Unimplemented

#endif // _WIN32
}


//-----------------------------------------------------------------------------
// ConfiguratorPlugin

ConfiguratorPlugin::ConfiguratorPlugin()
{
}

ConfiguratorPlugin::~ConfiguratorPlugin()
{
}


//-----------------------------------------------------------------------------
// Log Configurator

Configurator::Configurator() :
    GlobalMinimumLogLevel((Log_Level_t) Level::Debug),
    Plugin(nullptr)
{
}

Configurator* Configurator::GetInstance()
{
    static Configurator configurator;
    return &configurator;
}

Configurator::~Configurator()
{
}

void Configurator::SetGlobalMinimumLogLevel(Level level)
{
    Locker locker(OutputWorker::GetInstance()->GetChannelsLock());

    GlobalMinimumLogLevel = (Log_Level_t)level;

    for (ChannelNode* channelNode = ChannelNodeHead; channelNode; channelNode = channelNode->Next)
    {
        *(channelNode->Level) = (ovrlog::Log_Level_t) level;
    }
}

// Should be locked already when calling this function!
void Configurator::RestoreChannelLogLevel(const char* channelName)
{
    Level level = (Level) GlobalMinimumLogLevel;

    // Look up the log level for this channel if we can
    if (Plugin)
    {
        Plugin->RestoreChannelLevel(channelName, level);
    }

    const std::string stdChannelName(channelName);

    SetChannelNoLock(stdChannelName, level, false);
}

void Configurator::RestoreChannelLogLevel(ChannelNode* channelNode)
{
    Level level = (Level)GlobalMinimumLogLevel;

    // Look up the log level for this channel if we can
    if (Plugin)
    {
        Plugin->RestoreChannelLevel(channelNode->SubsystemName, level);
    }

    // Don't undo user calls to SetMinimumOutputLevelNoSave()
    if (*(channelNode->UserOverrodeMinimumOutputLevel) == false)
    {
        *(channelNode->Level) = (Log_Level_t)level;
    }
}

void Configurator::RestoreAllChannelLogLevels()
{
    Locker locker(OutputWorker::GetInstance()->GetChannelsLock());
    RestoreAllChannelLogLevelsNoLock();
}

void Configurator::RestoreAllChannelLogLevelsNoLock()
{
    for (ChannelNode* channelNode = ChannelNodeHead; channelNode; channelNode = channelNode->Next)
    {
        RestoreChannelLogLevel(channelNode->SubsystemName);
    }
}

void Configurator::SetPlugin(std::shared_ptr<ConfiguratorPlugin> plugin)
{
    Locker locker(OutputWorker::GetInstance()->GetChannelsLock());

    Plugin = plugin;

    for (ChannelNode* channelNode = ChannelNodeHead; channelNode; channelNode = channelNode->Next)
    {
        RestoreChannelLogLevel(channelNode->SubsystemName);
    }
}
void Configurator::GetChannels(std::vector< std::pair<std::string, Level> > &channels)
{
    channels.clear();

    Locker locker(OutputWorker::GetInstance()->GetChannelsLock());

    for (ChannelNode* channelNode = ChannelNodeHead; channelNode; channelNode = channelNode->Next)
    {
        channels.push_back(std::make_pair(std::string(channelNode->SubsystemName), (Level)*(channelNode->Level)));
    }
}
void Configurator::SetChannel(std::string channelName, Level level)
{
    Locker locker(OutputWorker::GetInstance()->GetChannelsLock());
    SetChannelNoLock(channelName, level, true);
}

// Should be locked already when calling this function!
void Configurator::SetChannelNoLock(std::string channelName, Level level, bool overrideUser)
{
    for (ChannelNode* channelNode = ChannelNodeHead; channelNode; channelNode = channelNode->Next)
    {
        if (std::string(channelNode->SubsystemName) == channelName)
        {
            if (*(channelNode->UserOverrodeMinimumOutputLevel) == false || overrideUser)
            {
                *(channelNode->Level) = (Log_Level_t)level;

                // Purposely no break, channels may have duplicate names
            }
        }
    }
}

void Configurator::OnChannelLevelChange(const char* channelName, Log_Level_t minimumOutputLevel)
{
    Locker locker(OutputWorker::GetInstance()->GetChannelsLock());

    if (Plugin)
    {
        // Save channel level
        Plugin->SaveChannelLevel(channelName, (Level) minimumOutputLevel);
    }
}


//-----------------------------------------------------------------------------
// ErrorSilencer
#if defined(_MSC_VER)
    #if (_MSC_VER < 1300)
        __declspec(thread) int ThreadErrorSilencedOptions = 0;
    #else
        #pragma data_seg(".tls$")
        __declspec(thread) int ThreadErrorSilencedOptions = 0;
        #pragma data_seg(".rwdata")
    #endif
#else
    thread_local int ThreadErrorSilencedOptions = 0;
#endif

int ErrorSilencer::GetSilenceOptions()
{
    return ThreadErrorSilencedOptions;
}

ErrorSilencer::ErrorSilencer(int options) :
    Options(options)
    {
        Silence();
    }

ErrorSilencer::~ErrorSilencer()
{
    Unsilence();
}

void ErrorSilencer::Silence()
{
    // We do not currently support recursive silencers
    assert(!GetSilenceOptions());
    ThreadErrorSilencedOptions = Options;
}

void ErrorSilencer::Unsilence()
{
    // We do not currently support recursive silencers
    assert(GetSilenceOptions());
    ThreadErrorSilencedOptions = 0;
}

} // namespace ovrlog

#ifdef OVR_STRINGIZE
#error "This code must remain independent of LibOVR"
#endif
