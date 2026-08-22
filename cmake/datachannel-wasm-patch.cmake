# Run by cmake/LibDataChannel.cmake as datachannel-wasm's patch step, in its
# source directory. Two changes to its JavaScript, both idempotent: a second
# run finds nothing left to do.
#
# 1. datachannel-wasm 0.4.0's JavaScript reaches the heap through
#    Module['HEAPU8'] and Module['HEAPU32']. Emscripten stopped exporting those
#    on Module by default years ago, so under a current one they are
#    undefined, and every data channel send, every binary message received,
#    and every peer connection made with ICE servers throws "Cannot read
#    properties of undefined". Seen 2026-09-27 under Emscripten 6.0.9. The
#    views themselves, HEAPU8 and HEAPU32, are in scope for library code and
#    are what Emscripten's own libraries use.
#
# 2. Deleting a WebSocket closes it, and closing one that is still connecting
#    makes the browser log "WebSocket connection to ... failed: WebSocket is
#    closed before the connection is established". A host that destroys a
#    transport soon after starting it gets that line. DevilutionX does it on
#    every create and join, when it discards its lobby's transport (seen
#    2026-10-01 in Chromium). A connecting socket is now left to finish
#    opening and closed then. Its callbacks are already disowned, so nothing
#    else hears of it; the server sees a connection that says nothing and
#    closes.

foreach(file wasm/js/webrtc.js wasm/js/websocket.js)
  file(READ "${file}" text)
  string(REPLACE "Module['HEAPU8']" "HEAPU8" patched "${text}")
  string(REPLACE "Module['HEAPU32']" "HEAPU32" patched "${patched}")
  if(NOT patched STREQUAL text)
    file(WRITE "${file}" "${patched}")
    message(STATUS "datachannel-wasm: ${file} now uses the heap views directly")
  endif()
endforeach()

# A release that reaches the heap some other new way would slip past the
# replacements above; say so rather than fail in a browser.
foreach(file wasm/js/webrtc.js wasm/js/websocket.js)
  file(READ "${file}" text)
  string(FIND "${text}" "Module['HEAP" found)
  if(NOT found EQUAL -1)
    message(FATAL_ERROR "datachannel-wasm: ${file} still reads a heap view from Module")
  endif()
endforeach()

set(file wasm/js/websocket.js)
file(READ "${file}" text)
set(deferred "if(webSocket.readyState == 0) webSocket.onopen = function() { webSocket.close(); }; else webSocket.close();")
string(FIND "${text}" "${deferred}" done)
if(done EQUAL -1)
  # wsDeleteWebSocket's is the file's only close. (Matched without its ';',
  # which would make each match a list of two.)
  string(REGEX MATCHALL "webSocket\\.close\\(\\)" closes "${text}")
  list(LENGTH closes count)
  if(count EQUAL 1)
    string(REPLACE "webSocket.close();" "${deferred}" patched "${text}")
    file(WRITE "${file}" "${patched}")
    message(STATUS "datachannel-wasm: ${file} lets a connecting socket open before closing it")
  else()
    # Not worth failing a build over: what it costs is a line in the console.
    message(WARNING "datachannel-wasm: ${file} has changed, so a socket closed while connecting "
                    "makes the browser log an error again")
  endif()
endif()
