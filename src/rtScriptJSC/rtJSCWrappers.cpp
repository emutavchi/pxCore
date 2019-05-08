#include "rtJSCWrappers.h"

#include "rtAtomic.h"

#include <cassert>
#include <memory>
#include <map>
#include <unordered_set>
#include <string>

using namespace RtJSC;

namespace {

static const char* kIsJSObjectWrapper = "833fba0e-31fd-11e9-b210-d663bd873d93";

static JSValueRef rtObjectWrapper_wrapPromise(JSContextRef context, rtObjectRef obj);
static JSValueRef rtObjectWrapper_wrapObject(JSContextRef context, rtObjectRef obj);
static JSValueRef rtFunctionWrapper_wrapFunction(JSContextRef context, rtFunctionRef func);

struct rtObjectWrapperPrivate
{
  rtValue v;
  std::map<std::string, std::pair<rtIObject*, rtJSCWeak>> wrapperCache;
};

static std::map<rtIObject*, rtJSCWeak> gWrapperCache;
static bool gEnableWrapperCache = true;

static bool isJSObjectWrapper(const rtObjectRef& obj)
{
  rtValue value;
  return obj && obj->Get(kIsJSObjectWrapper, &value) == RT_OK;
}

static bool rtIsPromise(const rtObjectRef& objRef)
{
  if (objRef)
  {
    rtMethodMap* methodMap = objRef.getPtr()->getMap();
    return methodMap && methodMap->className && strcmp(methodMap->className, "rtPromise") == 0;
  }
  return false;
}

class rtPromiseCallbackWrapper : public RefCounted<rtIFunction>
{
private:
  rtFunctionRef m_callback;
  size_t hash() override { return -1;  }
  void setHash(size_t hash) override {  UNUSED_PARAM(hash);  }
  rtError Send(int numArgs, const rtValue* args, rtValue* result) override
  {
    std::vector<rtValue> newArgs;
    if (numArgs > 0) {
      newArgs.reserve(numArgs);
      for (size_t i = 0; i < numArgs; ++i)
        newArgs.push_back(args[i]);
    }
    RtJSC::dispatchOnMainLoop(
      [callback = std::move(m_callback), newArgs = std::move(newArgs)] () mutable {
        rtError rc = callback->Send(newArgs.size(), newArgs.data(), nullptr);
        if (rc != RT_OK)
        {
          rtLogWarn("rtPromiseCallbackWrapper dispatch failed rc=%d", rc);
        }
      });
    return RT_OK;
  }
public:
  rtPromiseCallbackWrapper(rtFunctionRef ref)
    : m_callback(ref)
  {
  }
};

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
      if (jsToRt(context, arguments[i], val, exception) == RT_OK) {
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
  return rtToJs(context, result);
}

static void rtFunctionWrapper_finalize(JSObjectRef thisObject)
{
  rtObjectWrapperPrivate *p = (rtObjectWrapperPrivate *)JSObjectGetPrivate(thisObject);
  JSObjectSetPrivate(thisObject, nullptr);
  RtJSC::dispatchOnMainLoop( [p=p] {
      delete p;
    });
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
  rtObjectWrapperPrivate *p = new rtObjectWrapperPrivate;
  p->v.setFunction(func);
  return JSObjectMake(context, classRef, p);
}

static bool rtObjectWrapper_setProperty(JSContextRef context, JSObjectRef thisObject, JSStringRef propertyName, JSValueRef value, JSValueRef *exception)
{
  rtObjectWrapperPrivate *p = (rtObjectWrapperPrivate *)JSObjectGetPrivate(thisObject);
  rtObjectRef objectRef = p->v.toObject();
  if (!objectRef) {
    JSStringRef errStr = JSStringCreateWithUTF8CString("Not an rt object");
    *exception = JSValueMakeString(context, errStr);
    JSStringRelease(errStr);
    return false;
  }

  rtValue val;
  if (jsToRt(context, value, val, exception) != RT_OK) {
    printException(context, *exception);
    return false;
  }

  rtString name = jsToRtString(propertyName);
  rtError e = objectRef.set(name, val);
  if (e != RT_OK) {
    rtString errMessage = rtString("Failed to set property: ") + name;
#if 1
    rtLogWarn("%s", errMessage.cString());
#else
    JSStringRef jsStr = JSStringCreateWithUTF8CString(errMessage.cString());
    *exception = JSValueMakeString(context, jsStr);
    JSStringRelease(jsStr);
    printException(context, *exception);
    return false;
#endif
  }
  return true;
}

static JSValueRef rtObjectWrapper_convertToType(JSContextRef context, JSObjectRef object, JSType type, JSValueRef* exception)
{
  rtObjectWrapperPrivate *p = (rtObjectWrapperPrivate *)JSObjectGetPrivate(object);
  rtObjectRef objectRef = p->v.toObject();
  if (!objectRef)
    return JSValueMakeUndefined(context);

  if (type != kJSTypeString)
    return JSValueMakeNumber(context, 0);

  rtString desc;
  rtError e = objectRef.sendReturns<rtString>("description", desc);
  if (e != RT_OK)
    return JSValueMakeUndefined(context);

  JSStringRef jsStr = JSStringCreateWithUTF8CString(desc.cString());
  JSValueRef jsVal = JSValueMakeString(context, jsStr);
  JSStringRelease(jsStr);
  return jsVal;
}

static JSValueRef rtObjectWrapper_toStringCallback(JSContextRef context, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
  return rtObjectWrapper_convertToType(context, thisObject, kJSTypeString, exception);
}

static JSValueRef rtObjectWrapper_getProperty(JSContextRef context, JSObjectRef thisObject, JSStringRef propertyName, JSValueRef *exception)
{
  rtObjectWrapperPrivate *p = (rtObjectWrapperPrivate *)JSObjectGetPrivate(thisObject);
  rtObjectRef objectRef = p->v.toObject();
  if (!objectRef)
    return JSValueMakeUndefined(context);

  rtString propName = jsToRtString(propertyName);
  if (propName.isEmpty())
    return JSValueMakeUndefined(context);

  if (!strcmp(propName.cString(), "Symbol.toPrimitive") ||
      !strcmp(propName.cString(), "valueOf"))
    return JSValueMakeUndefined(context);

  if (!strcmp(propName.cString(), "toString"))
    return JSObjectMakeFunctionWithCallback(context, nullptr, rtObjectWrapper_toStringCallback);

  if (!strcmp(propName.cString(), "toJSON")) {
    return JSValueMakeUndefined(context);
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

  if (e != RT_OK) {
    rtLogDebug("rtObjectWrapper_getProperty failed name=%s, err=%d", propName.cString(), e);
    return JSValueMakeUndefined(context);
  }

  if (RT_objectType == v.getType())
  {
    rtObjectRef o = v.toObject();
    if (rtIsPromise(o))
    {
      JSValueRef res = nullptr;
      auto &cache = p->wrapperCache[propName.cString()];
      if (cache.first == o.getPtr() && cache.second.wrapped())
      {
        res = cache.second.wrapped();
      }
      else
      {
        res = rtObjectWrapper_wrapPromise(JSContextGetGlobalContext(context), o);
        if (JSValueIsObject(context, res)) {
          cache.first = o.getPtr();
          cache.second = rtJSCWeak(context, JSValueToObject(context, res, exception));
        } else {
          p->wrapperCache.erase(propName.cString());
        }
      }
      return res;
    }
  }

  return rtToJs(context, v);
}

static void rtObjectWrapper_getPropertyNames(JSContextRef ctx, JSObjectRef object, JSPropertyNameAccumulatorRef propertyNames)
{
  rtObjectWrapperPrivate *p = (rtObjectWrapperPrivate *)JSObjectGetPrivate(object);
  rtObjectRef objectRef = p->v.toObject();
  if (!objectRef)
    return;

  rtValue allKeys;
  rtError rc = objectRef->Get("allKeys", &allKeys);
  if (rc != RT_OK) {
    rtValue length;
    rc = objectRef->Get("length", &length);
    if (rc != RT_OK)
      return;
    uint32_t l = length.toUInt32();
    for (uint32_t i = 0; i < l; ++i) {
      JSStringRef jsStr = JSStringCreateWithUTF8CString(std::to_string(i).c_str());
      JSPropertyNameAccumulatorAddName(propertyNames, jsStr);
      JSStringRelease(jsStr);
    }
    return;
  }
  rtArrayObject* arr = (rtArrayObject*) allKeys.toObject().getPtr();
  if (!arr)
    return;

  for (uint32_t i = 0, l = arr->length(); i < l; ++i) {
    rtValue key;
    if (arr->Get(i, &key) == RT_OK && !key.isEmpty()) {
      JSStringRef jsStr = JSStringCreateWithUTF8CString(key.toString().cString());
      JSPropertyNameAccumulatorAddName(propertyNames, jsStr);
      JSStringRelease(jsStr);
    }
  }
}

static void rtObjectWrapper_finalize(JSObjectRef thisObject)
{
  rtObjectWrapperPrivate *p = (rtObjectWrapperPrivate *)JSObjectGetPrivate(thisObject);
  JSObjectSetPrivate(thisObject, nullptr);
  RtJSC::dispatchOnMainLoop([p=p] {
      if (gEnableWrapperCache) {
        rtIObject* ptr = p->v.toObject().getPtr();
        auto it = gWrapperCache.find(ptr);
        if (it != gWrapperCache.end() && it->second.wrapped() == nullptr) {
          gWrapperCache.erase(it);
        }
      }
      delete p;
    });
}

static const JSClassDefinition rtObjectWrapper_class_def =
{
  0,                                 // version
  kJSClassAttributeNone,             // attributes
  "__rtObject__class",               // className
  nullptr,                           // parentClass
  nullptr,                           // staticValues
  nullptr,                           // staticFunctions
  nullptr,                           // initialize
  rtObjectWrapper_finalize,          // finalize
  nullptr,                           // hasProperty
  rtObjectWrapper_getProperty,       // getProperty
  rtObjectWrapper_setProperty,       // setProperty
  nullptr,                           // deleteProperty
  rtObjectWrapper_getPropertyNames,  // getPropertyNames
  nullptr,                           // callAsFunction
  nullptr,                           // callAsConstructor
  nullptr,                           // hasInstance
  rtObjectWrapper_convertToType      // convertToType
};

static JSValueRef rtObjectWrapper_wrapPromise(JSContextRef context, rtObjectRef obj)
{
  if (!obj)
    return JSValueMakeNull(context);

  assert(rtIsPromise(obj));

  static const char* createPromise = "(function(){\n"
    "  let promiseCap = {};\n"
    "  promiseCap.promise = new Promise(function(resolve, reject){\n"
    "    promiseCap.resolve = resolve;\n"
    "    promiseCap.reject = reject;\n"
    "  });\n"
    "  return promiseCap;\n"
    "})()";

  static JSStringRef jsCreatePromiseStr = JSStringCreateWithUTF8CString(createPromise);
  static JSStringRef jsResolveStr = JSStringCreateWithUTF8CString("resolve");
  static JSStringRef jsRejectStr = JSStringCreateWithUTF8CString("reject");
  static JSStringRef jsPromiseStr = JSStringCreateWithUTF8CString("promise");

  JSValueRef exception = nullptr;
  JSValueRef evalResult = JSEvaluateScript(context, jsCreatePromiseStr, nullptr, nullptr, 0, &exception);
  if (exception) {
    printException(context, exception);
    return JSValueMakeUndefined(context);
  }
  JSObjectRef promiseCapObj = JSValueToObject(context, evalResult, &exception);
  if (exception) {
    printException(context, exception);
    return JSValueMakeUndefined(context);
  }
  JSValueRef promiseVal = JSObjectGetProperty(context, promiseCapObj, jsPromiseStr, &exception);
  if (exception) {
    printException(context, exception);
    return JSValueMakeUndefined(context);
  }
  JSValueRef resolveVal = JSObjectGetProperty(context, promiseCapObj, jsResolveStr, &exception);
  if (exception) {
    printException(context, exception);
    return JSValueMakeUndefined(context);
  }
  JSValueRef rejectVal = JSObjectGetProperty(context, promiseCapObj, jsRejectStr, &exception);
  if (exception) {
    printException(context, exception);
    return JSValueMakeUndefined(context);
  }
  rtValue resolveCallback;
  rtError rc = jsToRt(context, resolveVal, resolveCallback, &exception);
  if (rc != RT_OK) {
    rtLogError("Failed to convert resove callback. rc = %d", rc);
    return JSValueMakeUndefined(context);
  }
  if (resolveCallback.getType() != RT_functionType) {
    rtLogError("Resove callback has wrong type");
    return JSValueMakeUndefined(context);
  }
  if (exception) {
    printException(context, exception);
    return JSValueMakeUndefined(context);
  }
  rtValue rejectCallback;
  rc = jsToRt(context, rejectVal, rejectCallback, &exception);
  if (rc != RT_OK) {
    rtLogError("Failed to convert reject callback. rc = %d", rc);
    return JSValueMakeUndefined(context);
  }
  if (rejectCallback.getType() != RT_functionType) {
    rtLogError("Resove callback has wrong type");
    return JSValueMakeUndefined(context);
  }
  if (exception) {
    printException(context, exception);
    return JSValueMakeUndefined(context);
  }

  rtFunctionRef resolve(new rtPromiseCallbackWrapper(resolveCallback.toFunction()));
  rtFunctionRef reject(new rtPromiseCallbackWrapper(rejectCallback.toFunction()));

  rtObjectRef ignore;
  rc = obj.send("then", resolve, reject, ignore);
  if (rc != RT_OK) {
    rtLogError("rtPromise.then failed. rc = %d", rc);
    return JSValueMakeNull(context);
  }

  return promiseVal;
}

static JSValueRef rtObjectWrapper_wrapObject(JSContextRef context, rtObjectRef obj)
{
  if (!obj)
    return JSValueMakeNull(context);

  if (rtIsPromise(obj))
    return rtObjectWrapper_wrapPromise(JSContextGetGlobalContext(context), obj);

  if (gEnableWrapperCache)
  {
    rtIObject* ptr = obj.getPtr();
    auto it = gWrapperCache.find(ptr);
    if (it != gWrapperCache.end()) {
      JSObjectRef res = it->second.wrapped();
      if (!res) {
        gWrapperCache.erase(it);
      } else {
        return res;
      }
    }
  }

  JSClassRef classRef = nullptr;
  rtMethodMap* objMap = obj->getMap();
  if (objMap && objMap->className)
  {
    // hack to make destructuring assignment work
    if (!strcmp(objMap->className, "pxObjectChildren"))
    {
      rtValue length;
      rtError rc = obj->Get("length", &length);
      if (rc == RT_OK)
      {
        uint32_t l = length.toUInt32();
        std::vector<JSValueRef> arrArgs;
        arrArgs.reserve(l);
        for (uint32_t i = 0; i < l; ++i)
        {
          rtValue v;
          obj->Get(i, &v);
          arrArgs.push_back(rtToJs(context, v));
        }
        return JSObjectMakeArray(context, arrArgs.size(), arrArgs.data(), nullptr);
      }
      return JSObjectMakeArray(context, 0, nullptr, nullptr);
    }

    static std::map <std::string, JSClassRef> classCache;
    auto it = classCache.find(objMap->className);
    if (it == classCache.end())
    {
      JSClassDefinition classDef = { 0 };
      std::vector<JSStaticValue> staticValues;
      classDef.attributes = kJSClassAttributeNone;
      classDef.className = objMap->className;
      classDef.finalize = rtObjectWrapper_finalize;
      classDef.convertToType = rtObjectWrapper_convertToType;
      if (!strcmp(objMap->className, "rtMapObject") ||
          !strcmp(objMap->className, "rtArrayObject"))
      {
        classDef.getProperty = rtObjectWrapper_getProperty;
        classDef.setProperty = rtObjectWrapper_setProperty;
        classDef.getPropertyNames = rtObjectWrapper_getPropertyNames;
      }
      else
      {
        for (rtMethodMap* m = obj->getMap(); m; m = m->parentsMap)
        {
          rtPropertyEntry* e = m->getFirstProperty();
          while(e)
          {
            if (e->mPropertyName && strcmp(e->mPropertyName,"allKeys")) {
              JSStaticValue val;
              val.name = e->mPropertyName;
              val.getProperty = rtObjectWrapper_getProperty;
              val.setProperty = rtObjectWrapper_setProperty;
              val.attributes = kJSClassAttributeNone;
              staticValues.push_back(val);
            }
            e = e->mNext;
          }
        }

        for (rtMethodMap* m = obj->getMap(); m; m = m->parentsMap)
        {
          rtMethodEntry* e = m->getFirstMethod();
          while(e)
          {
            if (e->mMethodName) {
              JSStaticValue val;
              val.name = e->mMethodName;
              val.getProperty = rtObjectWrapper_getProperty;
              val.setProperty = nullptr;
              val.attributes = kJSClassAttributeNone;
              staticValues.push_back(val);
            }
            e = e->mNext;
          }
        }
        staticValues.push_back(JSStaticValue { nullptr, nullptr, nullptr, kJSClassAttributeNone });
        classDef.staticValues = staticValues.data();
      }
      classRef = JSClassCreate(&classDef);
      classCache[objMap->className] = classRef;
    }
    else
    {
      classRef = it->second;
    }
  }
  else
  {
    static JSClassRef sClassRef = JSClassCreate(&rtObjectWrapper_class_def);
    classRef = sClassRef;
  }

  rtObjectWrapperPrivate *p = new rtObjectWrapperPrivate;
  p->v.setObject(obj);
  JSObjectRef res = JSObjectMake(context, classRef, p);
  if (gEnableWrapperCache)
  {
    gWrapperCache[obj.getPtr()] = rtJSCWeak(context, res);
  }
  return res;
}

}  // namespace

namespace RtJSC {

rtError jsToRt(JSContextRef context, JSValueRef valueRef, rtValue &result, JSValueRef *exception)
{
  static const auto convertObject =
    [](JSContextRef ctx, JSValueRef valueRef, rtValue &result, JSValueRef &exc) {
      if (JSValueIsDate(ctx, valueRef)) {
        JSStringRef str = JSValueToStringCopy(ctx, valueRef, &exc);
        result.setString(jsToRtString(str));
        JSStringRelease(str);
        return;
      }

      JSObjectRef objectRef = JSValueToObject(ctx, valueRef, &exc);
      if (exc)
        return;

      rtObjectWrapperPrivate *p = (rtObjectWrapperPrivate *)JSObjectGetPrivate(objectRef);
      if (p) {
        result = p->v;
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

JSValueRef rtToJs(JSContextRef context, const rtValue &v)
{
  if (!context) {
    rtLogWarn("Lost JS context!");
    return nullptr;
  }

  if (v.isEmpty())
    return JSValueMakeNull(context);

  JSValueRef jsVal = nullptr;

  switch(v.getType())
  {
    case RT_objectType:
    {
      rtObjectRef o = v.toObject();
      if (o && !o->getMap() && isJSObjectWrapper(o)) {
        JSObjectWrapper* wrapper = static_cast<JSObjectWrapper*>(o.getPtr());
        if (JSContextGetGroup(wrapper->context()) == JSContextGetGroup(JSContextGetGlobalContext(context))) {
          jsVal = wrapper->wrapped();
        }
      }
      if (!jsVal) {
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

JSObjectWrapper::JSObjectWrapper(JSContextRef context, JSObjectRef object, bool isArray)
  : rtJSCProtected(context, object)
  , m_isArray(isArray)
{

}

JSObjectWrapper::~JSObjectWrapper()
{
}

rtError JSObjectWrapper::Get(const char* name, rtValue* value) const
{
  if (!m_contextRef || !m_object) {
    rtLogWarn("Lost JS context!");
    return RT_FAIL;
  }

  if (!name || !value)
    return RT_ERROR_INVALID_ARG;

  if (strcmp(name, kIsJSObjectWrapper) == 0)
    return RT_OK;

  if (strcmp(name, "description") == 0) {
    return RT_PROP_NOT_FOUND;
    // TODO: this should return a function
    JSValueRef exc = nullptr;
    JSStringRef descJsStr = JSValueToStringCopy(m_contextRef, m_object, &exc);
    if (exc) {
      printException(m_contextRef, exc);
      return RT_FAIL;
    }
    value->setString(jsToRtString(descJsStr));
    JSStringRelease(descJsStr);
    return RT_OK;
  }

  if (m_isArray && !!strcmp(name, "length"))
    return RT_PROP_NOT_FOUND;

  JSValueRef exc = nullptr;
  if (!strcmp(name, "allKeys")) {
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

  if (!m_isArray && JSValueGetType(m_contextRef, valueRef) == kJSTypeObject) {
    JSObjectRef objectRef = JSValueToObject(m_contextRef, valueRef, &exc);
    if (exc) {
      printException(m_contextRef, exc);
      return RT_FAIL;
    }
    if (JSObjectIsFunction(m_contextRef, objectRef)) {
      value->setFunction(new JSFunctionWrapper(m_contextRef, m_object, objectRef));
      return RT_OK;
    }
  }

  rtError ret = jsToRt(m_contextRef, valueRef, *value, &exc);
  if (exc) {
    printException(m_contextRef, exc);
    return RT_FAIL;
  }
  return ret;
}

rtError JSObjectWrapper::Get(uint32_t i, rtValue* value) const
{
  if (!value)
    return RT_ERROR_INVALID_ARG;
  JSValueRef exc = nullptr;
  JSValueRef valueRef = JSObjectGetPropertyAtIndex(m_contextRef, m_object, i, &exc);
  if (exc) {
    printException(m_contextRef, exc);
    return RT_FAIL;
  }
  rtError ret = jsToRt(m_contextRef, valueRef, *value, &exc);
  if (exc) {
    printException(m_contextRef, exc);
    return RT_FAIL;
  }
  return ret;
}

rtError JSObjectWrapper::Set(const char* name, const rtValue* value)
{
  if (!m_contextRef) {
    rtLogWarn("Lost JS context!");
    return RT_FAIL;
  }
  if (!name || !value)
    return RT_FAIL;
  if (m_isArray)
    return RT_PROP_NOT_FOUND;
  JSValueRef valueRef = rtToJs(m_contextRef, *value);
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

rtError JSObjectWrapper::Set(uint32_t i, const rtValue* value)
{
  if (!value)
    return RT_FAIL;
  JSValueRef valueRef = rtToJs(m_contextRef, *value);
  JSValueRef exc = nullptr;
  JSObjectSetPropertyAtIndex(m_contextRef, m_object, i, valueRef, &exc);
  if (exc) {
    printException(m_contextRef, exc);
    return RT_FAIL;
  }
  return RT_OK;
}

JSFunctionWrapper::JSFunctionWrapper(JSContextRef context, JSObjectRef thisObj, JSObjectRef funcObj)
  : rtJSCProtected(context, funcObj)
  , m_thisObj(thisObj)
{
}

JSFunctionWrapper::JSFunctionWrapper(JSContextRef context, JSObjectRef funcObj)
  : rtJSCProtected(context, funcObj)
{
}

JSFunctionWrapper::~JSFunctionWrapper()
{
}

rtError JSFunctionWrapper::Send(int numArgs, const rtValue* args, rtValue* result)
{
  if (!m_contextRef || !m_object) {
    rtLogWarn("Lost JS context!");
    return RT_FAIL;
  }
  std::vector<JSValueRef> jsArgs;
  if (numArgs) {
    jsArgs.reserve(numArgs);
    for (int i = 0; i < numArgs; ++i) {
      const rtValue &rtVal = args[i];
      jsArgs.push_back(rtToJs(m_contextRef, rtVal));
    }
  }
  JSValueRef exception = nullptr;
  JSValueRef jsResult = JSObjectCallAsFunction(m_contextRef, m_object, m_thisObj, numArgs, jsArgs.data(), &exception);
  if (exception) {
    printException(m_contextRef, exception);
    JSGlobalContextRelease(m_contextRef);
    return RT_FAIL;
  }
  rtError ret = RT_OK;
  if (result)
    ret = jsToRt(m_contextRef, jsResult, *result, &exception);
  return ret;
}

}  // RtJSC
