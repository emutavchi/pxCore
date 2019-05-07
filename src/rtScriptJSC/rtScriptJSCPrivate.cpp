#include "rtScriptJSCPrivate.h"

#include <memory>

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
    releaseGlobalContexLater(m_contextRef);
    m_object = nullptr;
    m_contextRef = nullptr;
    m_priv = nullptr;
  }
}


}  // RtJSC
