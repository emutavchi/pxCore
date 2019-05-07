#ifndef RTSCRIPTJSCPRIVATE_H
#define RTSCRIPTJSCPRIVATE_H

#include <JavaScriptCore/JavaScript.h>

#include <unordered_set>
#include <map>
#include <memory>

#include "rtJSCMisc.h"

namespace RtJSC {

class rtJSCProtected;

class rtJSCContextPrivate
{
  friend class rtJSCProtected;

  int m_refCount { 0 };
  std::unordered_set<rtJSCProtected*> m_protected;
  std::map<rtString, std::unique_ptr<rtJSCProtected>> m_moduleCache;

  void addProtected(rtJSCProtected* o) {
    m_protected.insert(o);
  }

  void removeProtected(rtJSCProtected* o) {
    m_protected.erase(o);
  }
public:
  unsigned long AddRef() {
    return rtAtomicInc(&m_refCount);
  }

  unsigned long Release() {
    long l = rtAtomicDec(&m_refCount);
    if (l == 0) {
      delete this;
    }
    return l;
  }

  void releaseAllProtected();
  JSObjectRef findModule(const rtString &path);
  void addToModuleCache(const rtString &path, JSGlobalContextRef context, JSObjectRef module);

  static rtJSCContextPrivate* create(JSGlobalContextRef contextRef);
  static void setInCtx(JSGlobalContextRef contextRef, rtJSCContextPrivate*);
  static rtJSCContextPrivate* fromCtx(JSGlobalContextRef contextRef);
};

class rtJSCProtected
{
protected:
  friend class rtJSCContextPrivate;

  JSGlobalContextRef m_contextRef { nullptr };
  JSObjectRef m_object { nullptr };
  rtJSCContextPrivate *m_priv { nullptr };

  void releaseProtected();
public:
  rtJSCProtected(JSGlobalContextRef context, JSObjectRef obj, rtJSCContextPrivate *priv);
  rtJSCProtected(JSContextRef context, JSObjectRef obj);
  virtual ~rtJSCProtected();

  JSObjectRef wrapped() const { return m_object; }
  JSGlobalContextRef context() const { return m_contextRef; }
};

}  // RtJSC

#endif /* RTSCRIPTJSCPRIVATE_H */
