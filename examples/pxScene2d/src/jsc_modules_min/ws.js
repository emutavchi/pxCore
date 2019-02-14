"use strict";const CONNECTING=0,OPEN=1,CLOSING=2,CLOSED=3;function WBEmit(){}WBEmit.prototype.$emit=function(a){var b=Array.prototype.slice.call(arguments,1);return this._events&&this._events[a]&&this._events[a].forEach(function(a){a.apply(this,b)}.bind(this)),this},WBEmit.prototype.$off=function(a,b){return this._events?(b||delete this._events[a],this._events[a]&&(this._events[a]=this._events[a].filter(function(a){return a!=b})),this):this},WBEmit.prototype.$on=function(a,b){return this._events||(this._events={}),this._events[a]||(this._events[a]=[]),this._events[a].push(b),b};function WebSocket(a,b,c){var d=a;this.readyState=CONNECTING,this.emit=new WBEmit,b&&(d=b+"://"+d),c=c||{},c.timeout=c.timeout||1e3*60,c.headers=c.headers||{};var e={uri:d,headers:c.headers,timeoutMs:c.timeout};this._instance=webscoketGet(e),setTimeout(()=>{this._instance.connect()}),this._instance.on("open",(...a)=>{this.readyState=OPEN,this.emit.$emit("open",...a)}),this._instance.on("close",(...a)=>{this.readyState=CLOSED,this.emit.$emit("close",...a)}),this._instance.on("message",(...a)=>this.emit.$emit("message",...a)),this._instance.on("error",(...a)=>this.emit.$emit("error",...a))}WebSocket.prototype.on=function(a,b){this.emit.$on(a,b)},WebSocket.prototype.close=function(){this.readyState=CLOSING,this._instance.close()},WebSocket.prototype.send=function(a){this._instance.send(a)},WebSocket.prototype.removeListener=function(a,b){this.emit.$off(a,b)},WebSocket.prototype.removeAllListeners=function(a){this.emit.$off(a)},WebSocket.prototype.closeimmediate=function(){this.close()},module.exports=WebSocket;