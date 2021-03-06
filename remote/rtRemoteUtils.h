#ifndef __RT_REMOTE_UTILS_H__
#define __RT_REMOTE_UTILS_H__

#include "rtRemoteMessage.h"
#include "rtRemoteTypes.h"
#include "rtRemoteEndpoint.h"

#include <rtError.h>
#include <string>
#include <sys/socket.h>
#include <stdint.h>

rtError rtRemoteEndpointAddressToSocket(rtRemoteEndPointPtr addr, sockaddr_storage& ss);
rtError rtRemoteSocketToEndpointAddress(sockaddr_storage const& ss, rtConnType const& connType, rtRemoteEndPointPtr& endpoint);
rtError rtRemoteParseUri(std::string const& uri, std::string& scheme, std::string& path, std::string& host, uint16_t* port);

bool    rtRemoteSameEndpoint(sockaddr_storage const& first, sockaddr_storage const& second);
bool    rtRemoteSameEndpoint(rtRemoteEndPointPtr const& first, rtRemoteEndPointPtr const& second);

rtNetType  rtRemoteParseNetType(std::string const& host);
rtCastType rtRemoteParseCastType(std::string const& host);

rtError rtRemoteDocumentToEndpoint(rtRemoteMessagePtr const& doc, rtRemoteEndPointPtr& endpoint);
rtError rtRemoteEndpointToDocument(rtRemoteEndPointPtr& endpoint, rtRemoteMessagePtr& doc);
rtError rtRemoteCombineDocuments(rtRemoteMessagePtr& target, rtRemoteMessagePtr& source);

#endif
