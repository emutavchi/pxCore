#include "rtJSCMisc.h"

#include "rtLog.h"
#include "pxTimer.h"

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <list>
#include <memory>
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <mutex>
#include <map>

#if defined(USE_GLIB)
#include <glib.h>
#endif

#if defined(USE_UV)
#include <uv.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
  JS_EXPORT void JSRemoteInspectorStart(void);
  JS_EXPORT JSStringRef JSContextCreateBacktrace(JSContextRef ctx, unsigned maxStackSize);
  JS_EXPORT void JSSynchronousGarbageCollectForDebugging(JSContextRef);
#ifdef __cplusplus
}
#endif

namespace WTF {
void initializeMainThread();
};

namespace JSC {
void initializeThreading();
};

namespace RtJSC {

static void releaseGlobalContexNow();
static void dispatchPending();

#if defined(USE_GLIB)
static GMainLoop *gMainLoop = nullptr;

void initMainLoop()
{
  WTF::initializeMainThread();
  JSC::initializeThreading();
  if (!gMainLoop && g_main_depth() == 0) {
    gMainLoop = g_main_loop_new (nullptr, false);
  }
  JSRemoteInspectorStart();
}

void pumpMainLoop()
{
  dispatchPending();
#if defined(USE_UV)
  {
    auto uvloop = uv_default_loop();
    uv_run(uvloop, UV_RUN_NOWAIT);
  }
#endif
  if (gMainLoop && g_main_depth() == 0) {
    gboolean ret;
    do {
      ret = g_main_context_iteration(nullptr, false);
    } while(ret);
  }
  releaseGlobalContexNow();
}

#else

void initMainLoop()
{
  WTF::initializeMainThread();
  JSC::initializeThreading();
}

void pumpMainLoop()
{
  dispatchPending();
  releaseGlobalContexNow();
}
#endif  // defined(USE_GLIB)

static std::list<JSGlobalContextRef> gCtxToRelease;
static std::list<std::function<void ()>> gPendingFun;
static std::mutex gDispatchMutex;

static void releaseGlobalContexNow() {
  std::list<JSGlobalContextRef> ctxVec;
  ctxVec.swap(gCtxToRelease);
  for(auto& ctx : ctxVec)
    JSGlobalContextRelease(ctx);
}

void releaseGlobalContexLater(JSGlobalContextRef ctx) {
  gCtxToRelease.push_back(ctx);
}

void printException(JSContextRef ctx, JSValueRef exception) {
  JSStringRef exceptStr = JSValueToStringCopy(ctx, exception, nullptr);
  rtString errorStr = jsToRtString(exceptStr);
  JSStringRelease(exceptStr);
  rtLogError("Got Exception: %s", errorStr.cString());
  // JSStringRef stackTraceStr = JSContextCreateBacktrace(ctx, 100);
  // rtString stackStr = jsToRtString(stackTraceStr);
  // JSStringRelease(stackTraceStr);
  // rtLogError("Stack:\n%s", stackStr.cString());
}

rtString jsToRtString(JSStringRef str)
{
  if (!str)
    return rtString();
  size_t len = JSStringGetMaximumUTF8CStringSize(str);
  std::unique_ptr<char[]> buffer(new char[len]);
  len = JSStringGetUTF8CString(str, buffer.get(), len);
  return rtString(buffer.get(), len); // does a copy
}

std::string readFile(const char *file)
{
  std::ifstream       src_file(file);
  std::stringstream   src_script;
  src_script << src_file.rdbuf();
  return src_script.str();
}

bool fileExists(const char* name)
{
  struct stat buffer;
  bool ret = (stat (name, &buffer) == 0);
  return ret;
}

static void dispatchPending()
{
  std::unique_lock<std::mutex> lock(gDispatchMutex);
  std::list<std::function<void ()>> pending = std::move(gPendingFun);
  gDispatchMutex.unlock();
  for(auto& fun : pending)
    fun();
}

void dispatchOnMainLoop(std::function<void ()>&& fun)
{
  std::unique_lock<std::mutex> lock(gDispatchMutex);
  gPendingFun.push_back(std::move(fun));
}

struct TimerInfo
{
  std::function<void ()> callback;
  double fireTime;
  bool repeat;
  uint32_t tag;
  ~TimerInfo();
};

// TODO: remove glib timout dependency and implement timer heap
static std::map<uint32_t, TimerInfo*> gTimerMap;

TimerInfo::~TimerInfo()
{
  gTimerMap.erase(tag);
}

#if defined(USE_GLIB)
static gboolean timerCallback(gpointer user_data)
{
  TimerInfo* info = reinterpret_cast<TimerInfo*>(user_data);
  bool repeat = info->repeat;
  info->callback();
  return repeat ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static void timerDestroy(gpointer user_data)
{
  TimerInfo* info = reinterpret_cast<TimerInfo*>(user_data);
  delete info;
}
#endif

uint32_t installTimeout(double intervalMs, bool repeat, std::function<void ()>&& fun)
{
  if (intervalMs < 0)
    intervalMs = 0;

  if (intervalMs == 0 && repeat)
    intervalMs = 10;

  double currentTime = pxMilliseconds();
  TimerInfo *info = new TimerInfo;
  info->fireTime = currentTime + intervalMs;
  info->repeat = repeat;
  info->callback = std::move(fun);
#if defined(USE_GLIB)
  info->tag = g_timeout_add_full(
    G_PRIORITY_DEFAULT, static_cast<guint>(intervalMs), timerCallback, info, timerDestroy);
#else
  static uint32_t timerIdx = 0;
  info->tag = ++timerIdx;
#endif
  gTimerMap[info->tag] = info;
  return info->tag;
}

void clearTimeout(uint32_t tag)
{
#if defined(USE_GLIB)
  if (gTimerMap.find(tag) != gTimerMap.end()) {
    g_source_remove(tag);
  }
#else
  auto it = gTimerMap.find(tag);
  if (it != gTimerMap.end()) {
    TimerInfo* info = it->second;
    delete info;
  }
#endif
}

}  // RtJSC
