#include "rtScriptJSCPrivate.h"

#include <memory>

#ifdef __cplusplus
extern "C" {
#endif

JS_EXPORT JSWeakRef JSWeakCreate(JSContextGroupRef, JSObjectRef);
JS_EXPORT void JSWeakRetain(JSContextGroupRef, JSWeakRef);
JS_EXPORT void JSWeakRelease(JSContextGroupRef, JSWeakRef);
JS_EXPORT JSObjectRef JSWeakGetObject(JSWeakRef);

#ifdef __cplusplus
}
#endif

namespace {

static void rtJSCContextPrivate_finalize(JSObjectRef obj)
{
  RtJSC::rtJSCContextPrivate *priv = (RtJSC::rtJSCContextPrivate *)JSObjectGetPrivate(obj);
  JSObjectSetPrivate(obj, nullptr);
  priv->Release();
}

static const JSClassDefinition rtJSCContextPrivate_class_def =
{
  0,                              // version
  kJSClassAttributeNone,          // attributes
  "__rtJSCContextPrivate__class", // className
  nullptr,                        // parentClass
  nullptr,                        // staticValues
  nullptr,                        // staticFunctions
  nullptr,                        // initialize
  rtJSCContextPrivate_finalize,   // finalize
  nullptr,                        // hasProperty
  nullptr,                        // getProperty
  nullptr,                        // setProperty
  nullptr,                        // deleteProperty
  nullptr,                        // getPropertyNames
  nullptr,                        // callAsFunction
  nullptr,                        // callAsConstructor
  nullptr,                        // hasInstance
  nullptr                         // convertToType
};

static JSStringRef jscContextPrivateName()
{
  static JSStringRef nameStr = JSStringCreateWithUTF8CString("__rt_ctx_priv_obj");
  return nameStr;
}

static JSClassRef jscContextPrivateClass()
{
  static JSClassRef sClassRef = JSClassCreate(&rtJSCContextPrivate_class_def);
  return sClassRef;
}

}  // namespace

namespace RtJSC {

rtJSCContextPrivate* rtJSCContextPrivate::create(JSGlobalContextRef contextRef)
{
  auto priv = std::make_unique<rtJSCContextPrivate>();

  JSValueRef exception = nullptr;
  JSObjectRef globalObj = JSContextGetGlobalObject(contextRef);
  JSStringRef privName = jscContextPrivateName();
  JSObjectRef privObj = JSObjectMake(contextRef, jscContextPrivateClass(), priv.get());

  JSObjectSetProperty(
    contextRef, globalObj, privName, privObj,
    kJSPropertyAttributeDontEnum | kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, &exception);

  if (exception) {
    printException(contextRef, exception);
    return nullptr;
  }

  // released in rtJSCContextPrivate_finalize
  priv->AddRef();
  return priv.release();
}

void rtJSCContextPrivate::setInCtx(JSGlobalContextRef contextRef, rtJSCContextPrivate* priv)
{
  // released in rtJSCContextPrivate_finalize
  priv->AddRef();

  JSValueRef exception = nullptr;
  JSObjectRef globalObj = JSContextGetGlobalObject(contextRef);
  JSStringRef privName = jscContextPrivateName();
  JSObjectRef privObj = JSObjectMake(contextRef, jscContextPrivateClass(), priv);

  JSObjectSetProperty(
    contextRef, globalObj, privName, privObj,
    kJSPropertyAttributeDontEnum | kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, &exception);

  if (exception)
    printException(contextRef, exception);
}

rtJSCContextPrivate* rtJSCContextPrivate::fromCtx(JSGlobalContextRef contextRef)
{
  JSValueRef exception = nullptr;
  JSObjectRef globalObj = JSContextGetGlobalObject(contextRef);
  JSStringRef privName = jscContextPrivateName();
  JSValueRef valueRef = JSObjectGetProperty(contextRef, globalObj, privName, &exception);
  if (exception) {
    printException(contextRef, exception);
    return nullptr;
  }

  JSObjectRef objectRef = JSValueToObject(contextRef, valueRef, &exception);
  if (exception) {
    printException(contextRef, exception);
    return nullptr;
  }
  return (rtJSCContextPrivate*)JSObjectGetPrivate(objectRef);
}

JSObjectRef rtJSCContextPrivate::findModule(const rtString &path)
{
  auto it = m_moduleCache.find(path);
  if (it != m_moduleCache.end())
    return it->second->wrapped();
  return nullptr;
}

void rtJSCContextPrivate::addToModuleCache(const rtString &path, JSGlobalContextRef context, JSObjectRef module)
{
  m_moduleCache[path] = std::make_unique<rtJSCProtected>(context, module, this);
}

void rtJSCContextPrivate::releaseAllProtected()
{
  auto protectedSet = std::move(m_protected);
  for(auto &p : protectedSet)
    p->releaseProtected();
  m_moduleCache.clear();
}

rtJSCProtected::rtJSCProtected(JSGlobalContextRef context, JSObjectRef object, rtJSCContextPrivate *priv)
  : m_contextRef(JSGlobalContextRetain(context))
  , m_object(object)
  , m_priv(priv)
{
  // TODO: consider using JSWeakRef and JSContextGroupAddMarkingConstraint
  JSValueProtect(m_contextRef, m_object);
  m_priv->addProtected(this);
}

rtJSCProtected::rtJSCProtected(JSContextRef context, JSObjectRef object)
  : m_contextRef(JSGlobalContextRetain(JSContextGetGlobalContext(context)))
  , m_object(object)
{
  JSValueProtect(m_contextRef, m_object);
  m_priv = rtJSCContextPrivate::fromCtx(m_contextRef);
  m_priv->addProtected(this);
}

rtJSCProtected::~rtJSCProtected()
{
  releaseProtected();
}

void rtJSCProtected::releaseProtected()
{
  if (m_contextRef && m_object) {
    m_priv->removeProtected(this);
    JSValueUnprotect(m_contextRef, m_object);
    JSGlobalContextRelease(m_contextRef);
    // releaseGlobalContexLater(m_contextRef);
    m_object = nullptr;
    m_contextRef = nullptr;
    m_priv = nullptr;
  }
}

rtJSCWeak::rtJSCWeak()
{
}

rtJSCWeak::rtJSCWeak(JSContextRef context, JSObjectRef obj)
{
  m_groupRef = JSContextGetGroup(JSContextGetGlobalContext(context));
  m_weakRef = JSWeakCreate(m_groupRef, obj);
}

rtJSCWeak::rtJSCWeak(const rtJSCWeak& other)
  : m_groupRef(other.m_groupRef)
  , m_weakRef(other.m_weakRef)
{
  if (m_groupRef && m_weakRef)
    JSWeakRetain(m_groupRef, m_weakRef);
}

rtJSCWeak::rtJSCWeak(rtJSCWeak&& other) noexcept
  : m_groupRef(std::exchange(other.m_groupRef, nullptr))
  , m_weakRef(std::exchange(other.m_weakRef, nullptr))
{
}

rtJSCWeak::~rtJSCWeak()
{
  releaseWeakRef();
}

void rtJSCWeak::releaseWeakRef()
{
  if (m_groupRef && m_weakRef) {
    JSWeakRelease(m_groupRef, m_weakRef);
    m_groupRef = nullptr;
    m_weakRef = nullptr;
  }
}

rtJSCWeak& rtJSCWeak::operator=(const rtJSCWeak &other)
{
  if (this != &other) {
    releaseWeakRef();
    m_groupRef = other.m_groupRef;
    m_weakRef = other.m_weakRef;
    if (m_groupRef && m_weakRef)
      JSWeakRetain(m_groupRef, m_weakRef);
  }
  return *this;
}

rtJSCWeak& rtJSCWeak::operator=(rtJSCWeak &&other) noexcept
{
  if (this != &other) {
    releaseWeakRef();
    m_groupRef = std::exchange(other.m_groupRef, nullptr);
    m_weakRef = std::exchange(other.m_weakRef, nullptr);
  }
  return *this;
}

JSObjectRef rtJSCWeak::wrapped() const
{
  if (m_groupRef)
    return JSWeakGetObject(m_weakRef);
  return nullptr;
}

}  // RtJSC
