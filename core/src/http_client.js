// The browser's fetch(), for http_client_wasm.cpp: one request, its body
// handed to C++ as it arrives. Linked with --js-library (core/CMakeLists.txt).
// Calls back through function pointers, the way datachannel-wasm's glue does.
// A request started with none -- a goodbye nobody waits for -- never calls
// into the program at all, so it may outlive it.

addToLibrary({
  $SNPS_HTTP: {
    requests: {},
    next: 1,
  },

  snps_http_start__deps: ['$SNPS_HTTP', '$UTF8ToString', '$stringToNewUTF8', 'malloc', 'free'],
  snps_http_start: function(pMethod, pUrl, pBody, bodyLength, onResponse, onBody, onDone, user) {
    const id = SNPS_HTTP.next++;
    const controller = new AbortController();
    SNPS_HTTP.requests[id] = controller;
    const method = UTF8ToString(pMethod);
    const url = UTF8ToString(pUrl);
    // Copied now: the C++ side is free to let go of it when this returns.
    const body = method === 'GET' ? undefined : HEAPU8.slice(pBody, pBody + bodyLength);
    const live = () => SNPS_HTTP.requests[id] === controller;

    (async () => {
      let error = '';
      try {
        const init = { method, signal: controller.signal, cache: 'no-store' };
        if (body !== undefined) {
          init.body = body;
          // text/plain keeps a page on another origin from needing a preflight.
          init.headers = { 'Content-Type': 'text/plain;charset=utf-8' };
        }
        const response = await fetch(url, init);
        if (!live())
          return;
        if (!onDone) {
          delete SNPS_HTTP.requests[id];
          return; // nobody to tell; the request has been made
        }
        {{{ makeDynCall('vii', 'onResponse') }}}(response.status, user);
        if (response.body) {
          const reader = response.body.getReader();
          for (;;) {
            const { value, done } = await reader.read();
            if (done || !live())
              break;
            const pChunk = _malloc(value.length);
            HEAPU8.set(value, pChunk);
            {{{ makeDynCall('viii', 'onBody') }}}(pChunk, value.length, user);
            _free(pChunk);
            if (!live())
              break;
          }
        }
      } catch (e) {
        error = String((e && e.message) || e || 'failed');
      }
      if (!live())
        return;
      delete SNPS_HTTP.requests[id];
      if (!onDone)
        return;
      const pError = stringToNewUTF8(error);
      {{{ makeDynCall('vii', 'onDone') }}}(pError, user);
      _free(pError);
    })();
    return id;
  },

  snps_http_cancel__deps: ['$SNPS_HTTP'],
  snps_http_cancel: function(id) {
    const controller = SNPS_HTTP.requests[id];
    if (controller) {
      delete SNPS_HTTP.requests[id];
      controller.abort();
    }
  },
});
