function RPCController(){var a={};this.registerApp=function(b,c){return a.hasOwnProperty(b)?(console.error("RPC registerApp:  App name RPC target already exists for  "+b),!1):(a[b]=c,!0)},this.execute=function(b,c,d,e,f){if(!a.hasOwnProperty(c))return console.error("RPC execute: App name RPC target doesn't exist for  "+c),!1;var g=a[c].getRPCContext()._execute(b,c,d,e);f!==void 0&&null!==f&&f(g)}}module.exports=RPCController;