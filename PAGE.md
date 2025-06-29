# WiFiManager Chunking with class Page
## Contents
 - [Summary](#summary)
   - Chunking Mode
   - Non-Chunking Mode
 - [Detail](#detail)
   - Chunking Mode
   - Non-Chunking Mode
 - [Implementation Notes](#implementation-notes)
 - [Contributors](#contributors)
## Summary
The primary purpose of *class Page* is to manage HTTP chunking of web pages as they are built and sent by *WiFiManager* in order to avoid building large
strings and running out of heap memory.

Alternatively, it can be used without chunking to determine whether and where large strings are being built and/or memory allocations are failing. This is a secondary
purpose since the use of chunking should generally make this unnecessary. Unless you have a particular reason to want this, it is recommended that you just use
chunking mode and don't bother reading the sub-sections below headed *Non-Chunking Mode*.
### Chunking Mode
1. This mode avoids heap memory exhaustion by supporting chunk-by-chunk building and sending of web pages. This does not mean just sending
   a full page using HTTP chunking, it means building a chunk, sending that chunk, building the next chunk, sending that chunk, etc.
   As a result, the size of a `Page page` string never exceeds the specified chunk size (by default 500 bytes). Without chunking,
   the size of a `String page` could be several kilobytes, and with many custom parameters, it could be much larger than that.

2. There are several places in *WiFiManager* where a large string, typically stored in flash `(PROGMEM)`, is subject to several find&replace operations before
   it is sent. To avoid storing a copy of the flash string in heap memory plus a second copy as the replace operation is done, *class Page* has support for doing
   multiple find&replace operations simultaneously whilst also sending the result as chunks.  This constitutes a large part of the code for *class Page*.

3. To minimise the number of code changes required to the pre-existing *WiFiManager* library to implement this chunking, *class Page* imitates the String
   library by supporting a `+=` operator. In many cases it was sufficient to replace `String page;` with `Page page(this);` because the existing `page += ...;`
   and `HTTPSend(page);` statements did the rest.

4. If allocation of the single chunk of String space required to buffer the current chunk fails, then it is reported by debugging messages, if enabled,
   sending a HTTP response status code of 503 (*Service Unavailable*).
   This is unlikely but possible e.g. if the application has allocated most of the heap memory before
   starting *WiFiManager*, or if the chunk size has been increased to a large number. The default chunk size is 500 bytes so if it fails to allocate even that much,
   the app likely has bigger problems.
### Non-Chunking Mode
5. The *class Page* has an alternative mode in which it does not chunk but it does detect and report failures to allocate memory by the String library
   when performing operations that extend the length of a string.
   This mode is an artefact of the analysis done during development of chunking mode to determine where the "big ticket" items were.
   It has been preserved for developers who, for some reason, want detail of how and where string memory is used.
   Unless this is you, just use chunking mode!

6. As well as reporting memory allocation failures via debug messages, this mode sends a HTTP response with status code 503 (*Service Unavailable*) so that the
   client (usually a web browser) reports to the end user that there was a problem.
   Previously, such failures were silent as far as the user was concerned.

7. There is support for reporting the source file line number at which an instance of *class Page* was defined if and when String memory cannot be allocated to it.

8. There is also support for reporting the source file line number from which a failure to allocate memory resulted. This requires some recoding in *WiFiManager* in
   order to capture the line number. A number of macros are provided to help with this (see below).

## Detail
The default mode (chunking, or not chunking) is determined by whether `WM_DEFAULTCHUNKING` is defined, or not, in *WiFiManager.h*. Regardless of this default,
the chunking mode of an instance of `Page` can be specified with the second parameter of the `Page::Page()` constructor.
  ```
  Page page1(this, true);
  ```
will apply chunking to `page1` regardless of `WM_DEFAULTCHUNKING` whereas
  ```
  Page page2(this, false);
  ```
will never apply chunking to `page2`.

In both modes, more informative debug messages can be enabled by using
  ```
  PAGE(page);
  ```
rather than:
  ```
  Page page;
  ```
The `PAGE` macro captures its line number and stores it in `page` for later use with any debug messages related to `page`. It always uses the default chunking mode
(if this is not desired, see macro `PAGE2` in *page.h*).
### Chunking Mode
Given the definition `Page page(this);` chunking mode is enabled and `page += x;` will send `x`, together with any previous string not yet sent, if they are large enough to be a chunk. Otherwise `x` will be stored for later.
When the page is complete, `HTTPSend(page);` will send any remaining content and complete the HTTP chunked transmission protocol.
This works well with *WiFiManager* because it has been mostly coded so that web pages are built and sent using the pattern:
  ```
  String page;
  page += ...;
  page += ...;
  ...;
  HTTPSend(page);
  ```

where simply replacing `String page;` with `Page page(this);` is enough to enable chunking.

There were several cases where building a page used something like:
  `page += getX();`
and *X* could be large so that `getX()` returned a large string and therefore was at risk of failing to allocate memory.
To handle this issue, the problematic cases of `String getX()` have been re-written as `void sendX(Page& page)` such that they build and send *X* as chunks
without ever building a large string.

*WiFiManager* stores some large strings in flash so as to reduce its r/w memory footprint when not instantiated. However when it is instantiated and these
strings are required, *WiFiManager* copies them from flash into heap memory (usually via the `String::String(const __FlashStringHelper *)` constructor). For example,
the HTML header string `HTTP_SCRIPT` is approx 3kB. To avoid this heap memory copy, *class Page* defines the `concat(const __FlashStringHelper *)` method to read
the string from flash memory in chunks and send as those chunks, without ever needing to store the entire string in r/w memory. Because the operator `+=` is
defined in terms of `concat()`, this means that `page += FPSTR(HTTP_SCRIPT);` also does it that way.

Although *class Page* imitates several methods and operators of *class String*, it does not define them all. For example the String methods that manipulate an
existing string do not apply because the "existing string" has, in general, already been sent or partially sent. This was not found to be a problem with *WiFiManager*,
with one significant exception, namely string find&replace operations such as:
  ```
  String page;
  ...
  page += FPSTR(HTTP_HEAD_START);
  page.replace(FPSTR(T_v), title);
  ```
To handle this case, *class Page* defines a method `replace()` which functions as simultaneous `concat()` and `replace()` methods with chunking.
It is used by replacing lines such as the above with:
  ```
  Page page(this);
  ...
  page.replace(FPSTR(HTTP_HEAD_START), FPSTR(T_v), title);
  ```
By using this version of `replace()` the sometimes large flash strings can still be read and sent in chunks even when they are subject to replacement.
For the several cases where a (large) flash string undergoes multiple find&replace operations before it is ready to send, there are extended `replace()` methods
that take a list of *(target, substitute)* pairs and apply them simultaneously while building and sending the result as chunks.

Although *class Page* is designed to minimise changes to the *WiFiManager* source code, there are some caveats.
1. When an instance of *class Page* gets its first content, either from `concat()` (a.k.a. operator `+=`) or from `replace()`, it sends a HTTP response status message (i.e. the first line of HTTP, before the headers) with the specified status code (200 by default). It is not then possible to send a different status instead.
Whilst it was previously theoretically possible to change the status code at the last minute (because the entire content was not sent until it was completely built),
this was not actually used anywhere in *WiFiManager*.

2. Any HTTP headers must be sent before an instance of *class Page* gets its first content and sends its first chunk, otherwise the headers cannot be sent.
   The one (1) case of this in *WiFiManager* has been handled by rearranging the call(s) to `sendHeader()` so as to be in the form of:
     ```
     Page page(this);
     server->sendHeader(...);
     server->sendHeader(...);
     page += ...;
     page += ...;
     HTTPSend(page);
     ```
   As a precaution, *class Page* defines a `sendHeader()` wrapper for the WebServer method that pre-checks whether `Page::begin()` and hence
   `WebServer::send()` have already been called. If they have, it generates a debug message and aborts. This no longer occurs but the wrapper remains
   as a check for future code changes.

A debug message (at level `WM_DEBUG_DEV`) is generated each time a chunk is sent and after the last chunk of the page has been sent.
If the line number at which the instance of `Page` was instantiated was captured (see macro `PAGE` above), this is referenced in those messages.
### Non-Chunking Mode
Each time the page string is extended (through concatentation or replacement), the success or otherwise of the String library operation is checked.
If it fails:
1. An HTTP response with status code 503 and the content *"out of memory"* is returned to the HTTP client.
2. Details are reported in debug messages, the first at level `WM_DEBUG_ERROR` and the others at level `WM_DEBUG_VERBOSE`.
3. If the line number at which the instance of *class Page* was defined has been captured (see macro `PAGE` above), it is reported at level `WM_DEBUG_DEV`.
4. If the line number at which the failure occured has been captured (see macros `APPEND` and `REPLACE` below), it is reported at level `WM_DEBUG_DEV`.

The macros that capture the line number at which a memory allocation failure may occur are as follows.
If, instead of any of:
   ```
   page += string;
   page.concat(cstring, length);
   page.replace(string, replacementList);
   page.replace(string, target, substitute);
   ```
 you use:
   ```
   APPEND(page, string);
   APPEND2(page, cstring, length);
   REPLACE2(page, string, replacementList);
   REPLACE3(page, string, target, substitute);
   ```
 then the line number of the macro will be reported with a debug message after the other out-of-memory debug messages.

## Implementation Notes
1. If an instance of *class Page* is deleted, e.g. by going out of scope, with `begin()` having been called but neither `end()` or `HTTPSend()` called,
   the *class Page* destructor will call `end()` and hence complete the incomplete HTTP response.

2. The actual capacity of the String buffer after `CHUNKSIZE` (or its overridden value) has been reserved is typically a little more than `CHUNKSIZE` because that is
   just what the String library does. We use the actual space that has been allocated and hence may send chunks a little larger than `CHUNKSIZE`
   (e.g. an actual capacity of 511 bytes has been observed after reserving 500 bytes).

3. Testing has been done with:
   1. ESP8266 ([WeMOS LOLIN D1 mini](https://www.wemos.cc/en/latest/d1/d1_mini.html)), Arduino IDE Version: 2.3.3, board *d1_mini* and boards *ESP8266 Boards (3.1.2)*; 
   2. ESP32-C3 ([WeMOS LOLIN C3 pico V1.0.0](https://www.wemos.cc/en/latest/c3/c3_pico.html)), Arduino IDE Version: 2.3.3, board *lolin_c3_mini* and boards *esp32 by Espressif Systems 3.2.0*;
   3. ESP32 ([SparkFun ESP32 Thing](https://www.sparkfun.com/sparkfun-esp32-thing.html)), Arduino IDE Version: 2.3.3, board *esp32thing* and boards *esp32 by Espressif Systems 3.2.0*.

4. There are two conditional-compilations to handle differences between the ESP8266 and ESP32 web server libraries.
   1. The *ESP8266WebServer* library requires an explicit API call to terminate chunking mode. This is done in `Page::endChunking().`
   2. The ESP32 *WebServer* library disables chunking when `send_P()` is called with no content after chunking has been enabled.
   This is probably a bug - it is worked-around in `Page::sendStatusCode().`

5. The constructor for *class Page* takes a pointer to an instance of *class WiFiManager* as its first parameter.
   It uses this to access the web server's methods, via `WiFiManager::server,` and the `DEBUG_WM()` debug message methods of *class WiFiManager*.

6. When debugging is enabled, *class WM_WebServer* is used as a wrapper for the actual web server class (*ESP8266WebServer* or *WebServer*).
   Its purposes are:
   1. when testing chunking, to ensure that no content is being sent using web server API methods that have not been treated for chunking;
   2. whether chunking or not, to generate debug messages (at level `WM_DEBUG_DEV`) when calls are made to the web server API methods with the actual parameters
      of those calls (with long content strings truncated so as not to cause memory issues).
      This is to provide visibility to the developer of what is actually being passed to the web server.

   Note that web server methods not intended to be used are inaccessible via *class WM_WebServer*.

   When debugging is disabled, *class WM_WebServer* is compiled-out and API calls go directly to the web server.

7. The macro `_PAGE_TEST_MEMORY_FAILURE` in *page.h* can be used to force out-of-memory conditions when testing non-chunking mode.

## Contributors
Class Page was written for WiFiManager v2.0.7 by @timr49.
