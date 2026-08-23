// Linked in with --pre-js. datachannel-wasm's glue reaches for window.WebSocket
// and window.RTCPeerConnection. A page has a window; Node has only globals.
if (typeof window == 'undefined')
  globalThis.window = globalThis;
