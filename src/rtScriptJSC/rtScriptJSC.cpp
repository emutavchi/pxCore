#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <chrono>

#include <JavaScriptCore/JavaScript.h>

#include "pxTimer.h"
#include "rtScript.h"
#include "rtCore.h"
#include "rtObject.h"
#include "rtValue.h"
#include "rtAtomic.h"
#include "rtPathUtils.h"
#include "rtScriptJSC.h"

#ifdef __cplusplus
extern "C" {
#endif

JS_EXPORT void JSGlobalContextSetRemoteInspectionEnabled(JSGlobalContextRef ctx, bool enabled);
JS_EXPORT void JSRemoteInspectorStart(void);
JS_EXPORT void JSSynchronousGarbageCollectForDebugging(JSContextRef);

#ifdef __cplusplus
}
#endif

std::list<JSGlobalContextRef> gCtxToRelease;

void releaseGlobalContexLater(JSGlobalContextRef ctx) {
  gCtxToRelease.push_back(ctx);
}

void releaseGlobalContexNow() {
  std::list<JSGlobalContextRef> ctxVec;
  ctxVec.swap(gCtxToRelease);
  for(auto& ctx : ctxVec)
    JSGlobalContextRelease(ctx);
}

// #define USE_GLIB

#if defined(USE_GLIB)
#include <glib.h>

static GMainLoop *gMainLoop = nullptr;

void initMainLoop()
{
  if (!gMainLoop && g_main_depth() == 0) {
    gMainLoop = g_main_loop_new (g_main_context_default(), true);
  }
}

void pumpMainLoop()
{
  releaseGlobalContexNow();
  if (!gMainLoop || g_main_depth() != 0)
    return;
  GMainContext *ctx = g_main_loop_get_context(gMainLoop);
  gboolean ret;
  do {
    ret = g_main_context_iteration(ctx, false);
  } while(ret);
}

#else

void initMainLoop() {}

void pumpMainLoop() {
  releaseGlobalContexNow();
}

#endif  // defined(USE_GLIB)

namespace {

static std::string readFile(const char *file)
{
  std::ifstream       src_file(file);
  std::stringstream   src_script;
  src_script << src_file.rdbuf();
  return src_script.str();
}

static bool fileExists(const std::string& name)
{
  struct stat buffer;
  bool ret = (stat (name.c_str(), &buffer) == 0);
  return ret;
}

static rtString jsToRtString(JSStringRef str)
{
    if (!str)
        return rtString();
    size_t len = JSStringGetMaximumUTF8CStringSize(str);
    std::unique_ptr<char[]> buffer(new char[len]);
    len = JSStringGetUTF8CString(str, buffer.get(), len);
    return rtString(buffer.get(), len); // does a copy
}

static void printException(JSContextRef ctx, JSValueRef exception)
{
  JSStringRef exceptStr = JSValueToStringCopy(ctx, exception, nullptr);
  rtString errorStr = jsToRtString(exceptStr);
  JSStringRelease(exceptStr);
  rtLogError("Got Exception: %s", errorStr.cString());
}

static rtError jsToRtValue(JSContextRef context, JSValueRef value, rtValue &result, JSValueRef *exception);
static JSValueRef rtToJSCValue(JSContextRef context, const rtValue &rtval);
static JSValueRef rtObjectWrapper_wrapObject(JSContextRef context, rtObjectRef obj);
static JSValueRef rtFunctionWrapper_wrapFunction(JSContextRef context, rtFunctionRef func);

static const char* kIsJSObjectWrapper = "833fba0e-31fd-11e9-b210-d663bd873d93";

bool isJSObjectWrapper(const rtObjectRef& obj)
{
  rtValue value;
  return obj && obj->Get(kIsJSObjectWrapper, &value) == RT_OK;
}

class JSObjectWrapper: public rtIObject
{
  int m_refCount { 0 };
  JSGlobalContextRef m_contextRef;
  JSObjectRef m_object;
  bool m_isArray;
public:
  JSObjectRef getObject() const { return m_object; }

  JSObjectWrapper(JSContextRef context, JSObjectRef object, bool isArray)
    : m_contextRef(JSGlobalContextRetain(JSContextGetGlobalContext(context)))
    , m_object(object)
    , m_isArray(isArray) {
    JSValueProtect(m_contextRef, m_object);
  }

  ~JSObjectWrapper() {
    JSValueUnprotect(m_contextRef, m_object);
    releaseGlobalContexLater(m_contextRef);
  }

  unsigned long AddRef() override {
    return rtAtomicInc(&m_refCount);
  }

  unsigned long Release() override {
    long l = rtAtomicDec(&m_refCount);
    if (l == 0) {
      delete this;
    }
    return l;
  }

  rtMethodMap* getMap() const override  {
    return nullptr;
  }

  rtError Get(const char* name, rtValue* value) const override {
    if (!name || !value)
      return RT_ERROR_INVALID_ARG;

    if (strcmp(name, kIsJSObjectWrapper) == 0)
      return RT_OK;

    if (m_isArray && !!strcmp(name, "length"))
      return RT_PROP_NOT_FOUND;

    JSValueRef exc = nullptr;
    if (0 == strcmp(name, "allKeys")) {
      rtArrayObject* array = new rtArrayObject;
      JSPropertyNameArrayRef namesRef = JSObjectCopyPropertyNames(m_contextRef, m_object);
      size_t size = JSPropertyNameArrayGetCount(namesRef);
      for (size_t i = 0; i < size; ++i) {
        JSStringRef namePtr = JSPropertyNameArrayGetNameAtIndex(namesRef, i);
        array->pushBack(jsToRtString(namePtr));
      }
      JSPropertyNameArrayRelease(namesRef);
      value->setObject(array);
      return RT_OK;
    }

    JSStringRef namePtr = JSStringCreateWithUTF8CString(name);
    JSValueRef valueRef = JSObjectGetProperty(m_contextRef, m_object, namePtr, &exc);
    JSStringRelease(namePtr);
    if (exc) {
      printException(m_contextRef, exc);
      return RT_FAIL;
    }

    rtError ret = jsToRtValue(m_contextRef, valueRef, *value, &exc);
    if (exc) {
      printException(m_contextRef, exc);
      return RT_FAIL;
    }
    return ret;
  }

  rtError Get(uint32_t i, rtValue* value) const override {
    if (!value)
      return RT_FAIL;
    JSValueRef exc = nullptr;
    JSValueRef valueRef = JSObjectGetPropertyAtIndex(m_contextRef, m_object, i, &exc);
    if (exc) {
      printException(m_contextRef, exc);
      return RT_FAIL;
    }
    rtError ret = jsToRtValue(m_contextRef, valueRef, *value, &exc);
    if (exc) {
      printException(m_contextRef, exc);
      return RT_FAIL;
    }
    return ret;
  }

  rtError Set(const char* name, const rtValue* value) override {
    if (!name || !value)
      return RT_FAIL;
    if (m_isArray)
      return RT_PROP_NOT_FOUND;
    JSValueRef valueRef = rtToJSCValue(m_contextRef, *value);
    JSValueRef exc = nullptr;
    JSStringRef namePtr = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(m_contextRef, m_object, namePtr, valueRef, kJSPropertyAttributeNone, &exc);
    JSStringRelease(namePtr);
    if (exc) {
      printException(m_contextRef, exc);
      return RT_FAIL;
    }
    return RT_OK;
  }

  rtError Set(uint32_t i, const rtValue* value) override {
    if (!value)
      return RT_FAIL;
    JSValueRef valueRef = rtToJSCValue(m_contextRef, *value);
    JSValueRef exc = nullptr;
    JSObjectSetPropertyAtIndex(m_contextRef, m_object, i, valueRef, &exc);
    if (exc) {
      printException(m_contextRef, exc);
      return RT_FAIL;
    }
    return RT_OK;
  }
};

class JSFunctionWrapper : public rtIFunction
{
  rtError Send(int numArgs, const rtValue* args, rtValue* result) override {
    std::vector<JSValueRef> jsArgs;
    if (numArgs) {
      jsArgs.reserve(numArgs);
      for (int i = 0; i < numArgs; ++i) {
        const rtValue &rtVal = args[i];
        jsArgs.push_back(rtToJSCValue(m_contextRef, rtVal));
      }
    }
    JSValueRef exception = nullptr;
    JSValueRef jsResult = JSObjectCallAsFunction(m_contextRef, m_funcObj, nullptr, numArgs, jsArgs.data(), &exception);
    if (exception) {
      rtLogError("RT to JS callback failed");
      printException(m_contextRef, exception);
      return RT_FAIL;
    }
    if (result) {
      return jsToRtValue(m_contextRef, jsResult, *result, &exception);
    }
    return RT_OK;
  }

  unsigned long AddRef() override {
    return rtAtomicInc(&m_refCount);
  }

  unsigned long Release() override {
    long l = rtAtomicDec(&m_refCount);
    if (l == 0) {
      delete this;
    }
    return l;
  }

  size_t hash() override {
    return -1;
  }

  void setHash(size_t hash) override {
    UNUSED_PARAM(hash);
  }

  int m_refCount { 0 };
  JSObjectRef m_funcObj;
  JSGlobalContextRef m_contextRef;

public:
  JSFunctionWrapper(JSContextRef context, JSObjectRef funcObj)
    : m_contextRef(JSGlobalContextRetain(JSContextGetGlobalContext(context)))
    , m_funcObj(funcObj) {
    JSValueProtect(m_contextRef, m_funcObj);
  }

  ~JSFunctionWrapper() {
    JSValueUnprotect(m_contextRef, m_funcObj);
    releaseGlobalContexLater(m_contextRef);
  }
};

static rtError jsToRtValue(JSContextRef context, JSValueRef valueRef, rtValue &result, JSValueRef *exception)
{
  static auto convertArray = [](JSContextRef ctx, JSValueRef valueRef, rtValue &result, JSValueRef &exc) {
    JSObjectRef objectRef = JSValueToObject(ctx, valueRef, &exc);
    if (exc)
      return;

    static JSStringRef namePtr = JSStringCreateWithUTF8CString("length");
    JSValueRef lenRef = JSObjectGetProperty(ctx, objectRef, namePtr, &exc);
    if (exc)
      return;

    size_t size = static_cast<size_t>(JSValueToNumber(ctx, lenRef, &exc));
    if (exc)
      return;

    std::unique_ptr<rtArrayObject> array(new rtArrayObject);
    for (size_t i = 0; i < size; ++i) {
      JSValueRef valueRef = JSObjectGetPropertyAtIndex(ctx, objectRef, i, &exc);
      if (exc)
        break;

      rtValue converted;
      jsToRtValue(ctx, valueRef, converted, &exc);
      if (exc)
        break;

      array->pushBack(converted);
    }

    if (!exc)
      result.setObject(array.release());
  };

  static auto convertObject = [](JSContextRef ctx, JSValueRef valueRef, rtValue &result, JSValueRef &exc) {
    if (JSValueIsDate(ctx, valueRef)) {
      JSStringRef str = JSValueToStringCopy(ctx, valueRef, &exc);
      result.setString(jsToRtString(str));
      JSStringRelease(str);
      return;
    }

    JSObjectRef objectRef = JSValueToObject(ctx, valueRef, &exc);
    if (exc)
      return;

    rtValue *v = (rtValue *)JSObjectGetPrivate(objectRef);
    if (v) {
      result = *v;
      return;
    }

    if (JSObjectIsFunction(ctx, objectRef)) {
      rtFunctionRef callback = new JSFunctionWrapper(ctx, objectRef);
      result = rtValue(callback);
      return;
    }

    rtObjectRef obj = new JSObjectWrapper(ctx, objectRef, JSValueIsArray(ctx, valueRef));
    result.setObject(obj);
  };

  JSValueRef exc = nullptr;
  JSType type = JSValueGetType(context, valueRef);
  switch (type)
  {
    case kJSTypeUndefined:
    case kJSTypeNull:
    {
      result.setEmpty();
      break;
    }
    case kJSTypeBoolean:
    {
      result.setBool(JSValueToBoolean(context, valueRef));
      break;
    }
    case kJSTypeNumber:
    {
      result.setDouble(JSValueToNumber(context, valueRef, &exc));
      break;
    }
    case kJSTypeString:
    {
      JSStringRef str = JSValueToStringCopy(context, valueRef, &exc);
      result.setString(jsToRtString(str));
      JSStringRelease(str);
      break;
    }
    case kJSTypeObject:
    {
      if (JSValueIsArray(context, valueRef))
        convertArray(context, valueRef, result, exc);
      else
        convertObject(context, valueRef, result, exc);
      break;
    }
    default:
    {
      JSStringRef str = JSStringCreateWithUTF8CString("Unknown value type!");
      exc = JSValueMakeString(context, str);
      JSStringRelease(str);
      break;
    }
  }
  if (exception)
    *exception = exc;
  return exc ? RT_FAIL : RT_OK;
}

static JSValueRef rtToJSCValue(JSContextRef context, const rtValue &v)
{
  if (v.isEmpty())
    return JSValueMakeNull(context);

  JSValueRef jsVal = nullptr;

  switch(v.getType())
  {
    case RT_objectType:
    {
      rtObjectRef o = v.toObject();
      if (o && !o->getMap() && isJSObjectWrapper(o)) {
        jsVal = static_cast<JSObjectWrapper*>(o.getPtr())->getObject();
      } else {
        jsVal = rtObjectWrapper_wrapObject(context, o);
      }
      break;
    }
    case RT_functionType:
    {
      jsVal = rtFunctionWrapper_wrapFunction(context, v.toFunction());
      break;
    }
    case RT_voidType:
    {
      jsVal = JSValueMakeUndefined(context);
      break;
    }
    case RT_int32_tType:
    case RT_uint32_tType:
    case RT_int64_tType:
    case RT_floatType:
    case RT_doubleType:
    case RT_uint64_tType:
    {
      jsVal = JSValueMakeNumber(context, v.toDouble());
      break;
    }
    case RT_boolType:
    {
      jsVal = JSValueMakeBoolean(context, v.toBool());
      break;
    }
    case RT_rtStringType:
    default:
    {
      JSStringRef jsStr = JSStringCreateWithUTF8CString(v.toString().cString());
      jsVal = JSValueMakeString(context, jsStr);
      JSStringRelease(jsStr);
      break;
    }
  }
  return jsVal;
}

static JSValueRef rtFunctionWrapper_callAsFunction(JSContextRef context, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
  rtValue *v = (rtValue *)JSObjectGetPrivate(function);
  rtFunctionRef funcRef = v->toFunction();
  if (!funcRef) {
    rtLogError("No rt func object");
    return JSValueMakeUndefined(context);
  }

  std::vector<rtValue> args;
  if (argumentCount > 0) {
    args.reserve(argumentCount);
    for (size_t i = 0; i < argumentCount; ++i) {
      rtValue val;
      if (jsToRtValue(context, arguments[i], val, exception) == RT_OK) {
        args.push_back(val);
      } else {
        rtLogError("Cannot convert js to rt value");
        printException(context, *exception);
        return JSValueMakeUndefined(context);
      }
    }
  }

  rtValue result;
  rtError rc = funcRef.SendReturns(argumentCount, args.data(), result);
  if (rc != RT_OK) {
    rtLogError("SendReturns failed, rc = %d", rc);
    JSStringRef errStr = JSStringCreateWithUTF8CString("rt SendReturns failed");
    *exception = JSValueMakeString(context, errStr);
    JSStringRelease(errStr);
    return JSValueMakeUndefined(context);
  }
  return rtToJSCValue(context, result);
}

static void rtFunctionWrapper_finalize(JSObjectRef thisObject)
{
  rtValue *v = (rtValue *)JSObjectGetPrivate(thisObject);
  JSObjectSetPrivate(thisObject, nullptr);
  delete v;
}

static const JSClassDefinition rtFunctionWrapper_class_def =
{
  0,                                // version
  kJSClassAttributeNone,            // attributes
  "__rtFunction__class",            // className
  nullptr,                          // parentClass
  nullptr,                          // staticValues
  nullptr,                          // staticFunctions
  nullptr,                          // initialize
  rtFunctionWrapper_finalize,       // finalize
  nullptr,                          // hasProperty
  nullptr,                          // getProperty
  nullptr,                          // setProperty
  nullptr,                          // deleteProperty
  nullptr,                          // getPropertyNames
  rtFunctionWrapper_callAsFunction, // callAsFunction
  nullptr,                          // callAsConstructor
  nullptr,                          // hasInstance
  nullptr                           // convertToType
};

static JSValueRef rtFunctionWrapper_wrapFunction(JSContextRef context, rtFunctionRef func)
{
  if (!func)
    return JSValueMakeNull(context);
  static JSClassRef classRef = JSClassCreate(&rtFunctionWrapper_class_def);
  rtValue *v = new rtValue(func);
  return JSObjectMake(context, classRef, v);
}

static bool rtObjectWrapper_setProperty(JSContextRef context, JSObjectRef thisObject, JSStringRef propertyName, JSValueRef value, JSValueRef *exception)
{
  rtValue *p = (rtValue *)JSObjectGetPrivate(thisObject);
  rtObjectRef objectRef = p->toObject();
  if (!objectRef) {
    JSStringRef errStr = JSStringCreateWithUTF8CString("No rt object");
    *exception = JSValueMakeString(context, errStr);
    JSStringRelease(errStr);
    return false;
  }

  rtValue val;
  if (jsToRtValue(context, value, val, exception) != RT_OK) {
    printException(context, *exception);
    return false;
  }

  rtString name = jsToRtString(propertyName);
  rtError e = objectRef.set(name, val);
  if (e != RT_OK) {
    JSStringRef errStr = JSStringCreateWithUTF8CString("Failed to set property");
    *exception = JSValueMakeString(context, errStr);
    JSStringRelease(errStr);
    printException(context, *exception);
    return false;
  }
  return true;
}

static JSValueRef rtObjectWrapper_getProperty(JSContextRef context, JSObjectRef thisObject, JSStringRef propertyName, JSValueRef *exception)
{
  rtValue *p = (rtValue *)JSObjectGetPrivate(thisObject);
  rtObjectRef objectRef = p->toObject();
  if (!objectRef)
    return JSValueMakeUndefined(context);

  rtString propName = jsToRtString(propertyName);
  if (propName.isEmpty())
    return JSValueMakeUndefined(context);

  if (!strcmp(propName.cString(), "Symbol.toPrimitive") ||
      !strcmp(propName.cString(), "toString") ||
      !strcmp(propName.cString(), "valueOf")) {
    static JSStringRef script =
      JSStringCreateWithUTF8CString("return '[object __rtObject_class]'");
    return JSObjectMakeFunction(context, nullptr, 0, nullptr, script, nullptr, 1, exception);
  }

  if (!strcmp(propName.cString(), "toJSON")) {
    static JSStringRef script = JSStringCreateWithUTF8CString("return {}");
    return JSObjectMakeFunction(context, nullptr, 0, nullptr, script, nullptr, 1, exception);
  }

  rtValue v;
  rtError e = RT_OK;
  if (std::isdigit(*propName.cString())) {
    uint32_t idx = std::stoul(propName.cString());
    e = objectRef.get(idx, v);
  } else {
    e = objectRef.get(propName.cString(), v);
  }

  if (e != RT_OK)
    return JSValueMakeUndefined(context);

  return rtToJSCValue(context, v);
}

static void rtObjectWrapper_finalize(JSObjectRef thisObject)
{
  rtValue *v = (rtValue *)JSObjectGetPrivate(thisObject);
  JSObjectSetPrivate(thisObject, nullptr);
  delete v;
}

static const JSClassDefinition rtObjectWrapper_class_def =
{
  0,                              // version
  kJSClassAttributeNone,          // attributes
  "__rtObject__class",            // className
  nullptr,                        // parentClass
  nullptr,                        // staticValues
  nullptr,                        // staticFunctions
  nullptr,                        // initialize
  rtObjectWrapper_finalize,       // finalize
  nullptr,                        // hasProperty
  rtObjectWrapper_getProperty,    // getProperty
  rtObjectWrapper_setProperty,    // setProperty
  nullptr,                        // deleteProperty
  nullptr,                        // getPropertyNames
  nullptr,                        // callAsFunction
  nullptr,                        // callAsConstructor
  nullptr,                        // hasInstance
  nullptr                         // convertToType
};

static JSValueRef rtObjectWrapper_wrapObject(JSContextRef context, rtObjectRef obj)
{
  if (!obj)
    return JSValueMakeNull(context);
  static JSClassRef classRef = JSClassCreate(&rtObjectWrapper_class_def);
  rtValue *v = new rtValue(obj);
  return JSObjectMake(context, classRef, v);
}

static JSValueRef noopCallback(JSContextRef ctx, JSObjectRef fun, JSObjectRef, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
  static JSStringRef namePtr = JSStringCreateWithUTF8CString("name");
  JSValueRef exc = nullptr;
  JSValueRef valueRef = JSObjectGetProperty(ctx, fun, namePtr, &exc);
  if (!exc) {
    rtValue v;
    jsToRtValue(ctx, valueRef, v, nullptr);
    rtString name = v.toString();
    rtLogError("%s no-op", name.cString());
  } else {
    rtLogError("no-op");
  }
  return JSValueMakeUndefined(ctx);
}

static JSValueRef uvGetHrTime(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
  auto dur = std::chrono::high_resolution_clock::now().time_since_epoch();
  uint64_t hrtime = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
  int nanoseconds = hrtime % (int)1e9;
  int seconds = hrtime / 1e9;
  JSValueRef args[] = { JSValueMakeNumber(ctx, seconds), JSValueMakeNumber(ctx, nanoseconds) };
  return JSObjectMakeArray(ctx, 2, args, exception);
}

static bool resolveModulePath(const rtString &name, rtString &data)
{
  std::list<rtString> dirs;
  std::list<rtString> endings;
  bool found = false;
  rtString path;

  dirs.push_back(""); // this dir
  dirs.push_back("jsc_modules/");

  endings.push_back(".js");

  std::list<rtString>::const_iterator it, jt;
  for (it = dirs.begin(); !found && it != dirs.end(); ++it) {
    rtString s = *it;
    if (!s.isEmpty() && !s.endsWith("/")) {
      s.append("/");
    }
    s.append(name.beginsWith("./") ? name.substring(2) : name);
    for (jt = endings.begin(); !found && jt != endings.end(); ++jt) {
      path = s;
      if (!path.endsWith((*jt).cString())) {
        path.append(*jt);
      }
      found = fileExists(path.cString());
    }
  }

  if (found)
    data = path;
  return found;
}


static std::map<rtString, JSValueRef> gModuleCache;

static JSValueRef requireCallback(JSContextRef ctx, JSObjectRef, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
  if (argumentCount != 1)
      return JSValueMakeNull(ctx);

  do {
    JSStringRef reqArgStr = JSValueToStringCopy(ctx, arguments[0], exception);
    if (exception && *exception)
      break;
    rtString moduleName = jsToRtString(reqArgStr);

    rtString path;
    if (!resolveModulePath(moduleName, path)) {
      JSStringRelease(reqArgStr);
      rtLogError("Module %s not found", moduleName.cString());
      break;
    }

    JSGlobalContextRef globalCtx = JSContextGetGlobalContext(ctx);
    auto cachedModule= gModuleCache.find(path);
    if (cachedModule != gModuleCache.end()) {
      JSStringRelease(reqArgStr);
      return cachedModule->second;
    }

    std::string codeStr = readFile(path.cString());
    if(codeStr.empty()) {
      JSStringRelease(reqArgStr);
      rtLogError(" %s  ... load error / not found.",__PRETTY_FUNCTION__);
      break;
    }

    codeStr = "(function(){var module = this; var exports = {}; module.exports = exports;\n"
      + codeStr +
      ";\n return this;}).call(new Object);";

    JSStringRef jsstr = JSStringCreateWithUTF8CString(codeStr.c_str());
    JSValueRef module = JSEvaluateScript(globalCtx, jsstr, nullptr, reqArgStr, 0, exception);
    JSStringRelease(jsstr);
    JSStringRelease(reqArgStr);

    if (exception && *exception) {
      JSStringRef exceptStr = JSValueToStringCopy(globalCtx, *exception, nullptr);
      rtString errorStr = jsToRtString(exceptStr);
      JSStringRelease(exceptStr);
      rtLogError("Failed to eval, \n\terror='%s'\n\tmodule=%s\n\tscript='...'", errorStr.cString(), path.cString());
      break;
    }

    JSObjectRef moduleObj = JSValueToObject(globalCtx, module, exception);
    if (exception && *exception) {
      JSStringRef exceptStr = JSValueToStringCopy(globalCtx, *exception, nullptr);
      rtString errorStr = jsToRtString(exceptStr);
      JSStringRelease(exceptStr);
      rtLogError("Failed to convert module to object, \n\terror='%s'\n\tmodule=%s", errorStr.cString(), path.cString());
      break;
    }

    static JSStringRef exportsStr = JSStringCreateWithUTF8CString("exports");
    JSValueRef exportsVal = JSObjectGetProperty(globalCtx, moduleObj, exportsStr, exception);
    if (exception && *exception) {
      JSStringRef exceptStr = JSValueToStringCopy(globalCtx, *exception, nullptr);
      rtString errorStr = jsToRtString(exceptStr);
      JSStringRelease(exceptStr);
      rtLogError("Failed to get exports module to object, \n\terror='%s'\n\tmodule=%s", errorStr.cString(), path.cString());
      break;
    }

    JSValueProtect(globalCtx, module);
    gModuleCache[path] = exportsVal;

    return exportsVal;
  } while(0);

  return JSValueMakeNull(ctx);
}

static void markIsJSC(JSContextRef ctx, JSObjectRef globalObj, JSValueRef *exception)
{
  static JSStringRef jsName = JSStringCreateWithUTF8CString("_isJSC");
  JSContextRef globalCtx = JSContextGetGlobalContext(ctx);
  if (!globalObj)
    globalObj = JSContextGetGlobalObject(globalCtx);
  JSObjectSetProperty(globalCtx, globalObj, jsName, JSValueMakeBoolean(globalCtx, true), kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, exception);
}

static JSValueRef runInNewContext(JSContextRef ctx, JSObjectRef, JSObjectRef thisobj, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
  if (argumentCount < 7)
    return JSValueMakeUndefined(ctx);

  JSValueRef result = nullptr;

  JSContextGroupRef groupRef = JSContextGetGroup(JSContextGetGlobalContext(ctx));
  JSGlobalContextRef newCtx = JSGlobalContextCreateInGroup(groupRef, nullptr);

  do {
    JSObjectRef newGlobalObj = JSContextGetGlobalObject(newCtx);
    markIsJSC(newCtx, newGlobalObj, exception);
    if (exception && *exception)
      break;

    // sandbox
    JSObjectRef sandboxRef = JSValueToObject(ctx, arguments[1],  exception);
    if (exception && *exception)
      break;

    // setup global
    JSPropertyNameArrayRef namesRef = JSObjectCopyPropertyNames(ctx, sandboxRef);
    size_t size = JSPropertyNameArrayGetCount(namesRef);
    for (size_t i = 0; i < size; ++i) {
      JSStringRef namePtr = JSPropertyNameArrayGetNameAtIndex(namesRef, i);
      JSValueRef valueRef = JSObjectGetProperty(ctx, sandboxRef, namePtr, exception);
      if (exception && *exception)
        break;
      JSObjectSetProperty(newCtx, newGlobalObj, namePtr, valueRef, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, exception);
    }
    JSPropertyNameArrayRelease(namesRef);
    if (exception && *exception)
      break;

    // code
    JSStringRef codeStr = JSValueToStringCopy(ctx, arguments[0], exception);
    if (exception && *exception)
      break;

    JSStringRef fileNameStr = JSValueToStringCopy(ctx, arguments[5], exception);
    if (exception && *exception) {
      JSStringRelease(codeStr);
      break;
    }
    JSGlobalContextSetName(newCtx, fileNameStr);

    JSValueRef evalResult = JSEvaluateScript(newCtx, codeStr, newGlobalObj, fileNameStr, 0, exception);

    JSStringRelease(codeStr);
    JSStringRelease(fileNameStr);
    if (exception && *exception)
      break;

    JSObjectRef funcObj = JSValueToObject(ctx, evalResult, exception);
    if (exception && *exception)
      break;

    JSValueRef args[] = { arguments[3], arguments[4] };
    result = JSObjectCallAsFunction(newCtx, funcObj, newGlobalObj, 2, args, exception);
  } while (0);

  JSGlobalContextRelease(newCtx);

  if (exception && *exception) {
    printException(ctx, *exception);
    return JSValueMakeUndefined(ctx);
  }

  return result;
}

void injectBindings(JSContextRef jsContext)
{
  auto injectFun = [](JSContextRef jsContext, const char* name, JSObjectCallAsFunctionCallback callback) {
    JSContextRef globalCtx = JSContextGetGlobalContext(jsContext);
    JSObjectRef globalObj = JSContextGetGlobalObject(globalCtx);
    JSStringRef funcName = JSStringCreateWithUTF8CString(name);
    JSObjectRef funcObj = JSObjectMakeFunctionWithCallback(jsContext, funcName, callback);
    JSObjectSetProperty(jsContext, globalObj, funcName, funcObj, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, nullptr);
    JSStringRelease(funcName);
  };

  injectFun(jsContext, "uv_platform", noopCallback);
  injectFun(jsContext, "setTimeout", noopCallback);
  injectFun(jsContext, "clearTimeout", noopCallback);
  injectFun(jsContext, "setInterval", noopCallback);
  injectFun(jsContext, "clearInterval", noopCallback);
  injectFun(jsContext, "httpGet", noopCallback);
  injectFun(jsContext, "webscoketGet", noopCallback);

  injectFun(jsContext, "uv_hrtime", uvGetHrTime);
  injectFun(jsContext, "require", requireCallback);
  injectFun(jsContext, "_runInNewContext", runInNewContext);
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class rtJSCContext: public rtIScriptContext
{
public:
  rtJSCContext();
  virtual ~rtJSCContext();

  rtError add(const char *name, rtValue  const& val);
  rtValue get(const char *name);
  bool    has(const char *name);

  rtError runScript(const char *script, rtValue* retVal = nullptr, const char *args = nullptr);
  rtError runFile  (const char *file,   rtValue* retVal = nullptr, const char *args = nullptr);

  unsigned long AddRef()  { return rtAtomicInc(&m_refCount);  }
  unsigned long Release();

private:
  rtError evaluateScript(const char *script, const char *name, rtValue* retVal = nullptr, const char *args = nullptr);
  int m_refCount { 0 };
  JSContextGroupRef m_contextGroup { nullptr };
  JSGlobalContextRef m_context { nullptr };
};

rtJSCContext::rtJSCContext()
{
  rtLogInfo(__FUNCTION__);
  // TODO: probably this should be per script ...
  static auto group = JSContextGroupCreate();
  m_contextGroup = JSContextGroupRetain(group);
  m_context = JSGlobalContextCreateInGroup(m_contextGroup, nullptr);

  markIsJSC(m_context, nullptr, nullptr);
  injectBindings(m_context);

  // JSGlobalContextSetRemoteInspectionEnabled(m_context, false);
}

rtJSCContext::~rtJSCContext()
{
  rtLogInfo(__FUNCTION__);
  // JSGarbageCollect(m_context);
  JSSynchronousGarbageCollectForDebugging(m_context);
  JSGlobalContextRelease(m_context);
  JSContextGroupRelease(m_contextGroup);
  rtLogInfo(__FUNCTION__);
}

rtError rtJSCContext::add(const char *name, rtValue const& val)
{
  JSStringRef jsName = JSStringCreateWithUTF8CString(name);
  JSValueRef jsVal = rtToJSCValue(m_context, val);
  JSObjectRef globalObj = JSContextGetGlobalObject(m_context);
  JSValueRef exception = nullptr;
  JSObjectSetProperty(m_context, globalObj, jsName, jsVal, kJSPropertyAttributeDontEnum, &exception);
  JSStringRelease(jsName);

  if (exception) {
    JSStringRef exceptStr = JSValueToStringCopy(m_context, exception, nullptr);
    rtString errorStr = jsToRtString(exceptStr);
    JSStringRelease(exceptStr);
    rtLogError("Failed to add to context, error='%s'\n", errorStr.cString());
    return RT_FAIL;
  }
  return RT_OK;
}

rtValue rtJSCContext::get(const char *name)
{
  rtLogError("%s not implemented",__FUNCTION__);
  return rtValue();
}

bool rtJSCContext::has(const char *name)
{
  rtLogError("%s not implemented",__FUNCTION__);
  return false;
}

rtError rtJSCContext::evaluateScript(const char* script, const char* name, rtValue* retVal, const char *args)
{
  rtLogInfo(__FUNCTION__);

  JSValueRef exception = nullptr;
  JSStringRef codeStr = JSStringCreateWithUTF8CString(script);
  JSObjectRef globalObj = JSContextGetGlobalObject(m_context);
  JSStringRef fileStr = nullptr;

  if (name) {
    static int evalCount = 0;
    ++evalCount;
    std::string ctxNameStr = std::to_string(evalCount) + ": " + name;
    fileStr = JSStringCreateWithUTF8CString(ctxNameStr.c_str());
    JSGlobalContextSetName(m_context, fileStr);
  } else {
    fileStr = JSStringCreateWithUTF8CString("rtJSCContext::evaluateScript");
  }

  JSValueRef result = JSEvaluateScript(m_context, codeStr, globalObj, fileStr, 0, &exception);
  JSStringRelease(codeStr);
  JSStringRelease(fileStr);

  if (exception) {
    JSStringRef exceptStr = JSValueToStringCopy(m_context, exception, nullptr);
    rtString errorStr = jsToRtString(exceptStr);
    JSStringRelease(exceptStr);
    rtLogError("Failed to eval, error='%s'", errorStr.cString());
    return RT_FAIL;
  }

  rtError ret = RT_OK;
  if (retVal) {
    if(result)
      ret = jsToRtValue(m_context, result, *retVal, nullptr);
    else
      *retVal = rtValue();
  }
  return ret;
}

rtError rtJSCContext::runScript(const char* script, rtValue* retVal, const char *args)
{
  return evaluateScript(script, nullptr, retVal, args);
}

rtError rtJSCContext::runFile(const char *file, rtValue* retVal, const char* args)
{
  if (!file) {
    rtLogError(" %s  ... no script given.",__PRETTY_FUNCTION__);
    return RT_FAIL;
  }

  std::string codeStr = readFile(file);
  if(codeStr.empty()) {
    rtLogError(" %s  ... load error / not found.",__PRETTY_FUNCTION__);
    return RT_FAIL;
  }

  return evaluateScript(codeStr.c_str(), file, retVal, args);
}

unsigned long rtJSCContext::Release()
{
  long l = rtAtomicDec(&m_refCount);
  if (l == 0) {
    delete this;
  }
  return l;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class rtScriptJSC: public rtIScript
{
public:
  rtScriptJSC();
  virtual ~rtScriptJSC();

  unsigned long AddRef() override { return rtAtomicInc(&m_refCount); }
  unsigned long Release() override;

  rtError init() override;
  rtError term() override;
  rtString engine() override { return "JavaScriptCore"; }
  rtError pump() override;
  rtError createContext(const char *lang, rtScriptContextRef& ctx) override;
  rtError collectGarbage() override;
  void* getParameter(rtString param) override;

private:
  int m_refCount { 0 };
};

rtScriptJSC::rtScriptJSC()
{
  initMainLoop();
  JSRemoteInspectorStart();
}

rtError rtScriptJSC::init()
{
  return RT_OK;
}

rtScriptJSC::~rtScriptJSC()
{
  rtLogInfo(__FUNCTION__);
}

unsigned long rtScriptJSC::Release()
{
  long l = rtAtomicDec(&m_refCount);
  if (l == 0)
  {
    delete this;
  }
  return l;
}

rtError rtScriptJSC::pump()
{
  pumpMainLoop();
  return RT_OK;
}

rtError rtScriptJSC::collectGarbage()
{
  return RT_OK;
}

void* rtScriptJSC::getParameter(rtString param)
{
  return nullptr;
}

rtError rtScriptJSC::term()
{
  return RT_OK;
}

rtError rtScriptJSC::createContext(const char *lang, rtScriptContextRef& ctx)
{
  ctx = static_cast<rtIScriptContext*>(new rtJSCContext());
  return RT_OK;
}

rtError createScriptJSC(rtScriptRef& script)
{
  script = new rtScriptJSC();
  return RT_OK;
}
