#include <Arduino.h>
#include "WiFiManager.h"
#include "page.h"

// Default chunk size.
#ifndef DEFAULT_CHUNKSIZE
#define DEFAULT_CHUNKSIZE 500
#endif

// Whether to optimise the case where all targets are of the form "{...}" - this is the only case in WiFiManager.
#ifndef OPTIMIZE_CURLY_TOKENS
#define OPTIMIZE_CURLY_TOKENS 1
#endif

// Whether to optimise the case where all targets are of the form "{.}" - this is often, but not always, the case in WiFiManager.
#ifndef OPTIMIZE_CURLY_ONECHAR_TOKENS
#define OPTIMIZE_CURLY_ONECHAR_TOKENS 1
#endif

#if defined(WM_NODEBUG) || defined(WM_NOASSERT)
#define ASSERT(x) // empty
#else
#define ASSERT(x) assert(x)
#endif

// class Page methods

Page::Page(WiFiManager *wifiManager, bool chunking, unsigned int chunkSize, LineNumber definedAtLineNumber):
  wifiManager(wifiManager),
  definedAtLineNumber(definedAtLineNumber),
  chunking(chunking),
  chunkSize(chunkSize ? chunkSize : DEFAULT_CHUNKSIZE),
  _string(chunking)
{
  #ifdef WM_DEBUG_LEVEL
  DEBUG_WM(WM_DEBUG_DEV, F("Page::Page() definedAtLineNumber="), (String)definedAtLineNumber);
  DEBUG_WM(WM_DEBUG_DEV, F(" chunking="), (String)chunking);
  DEBUG_WM(WM_DEBUG_DEV, F(" chunkSize="), (String)chunkSize);
  #endif
}

Page::~Page()
{
  end();
}

void Page::begin(uint16_t code) {
  ASSERT(wifiManager);
  ASSERT(wifiManager->server);
  if (begun)
    return;
  begun = true;
  _error = false;
  _sentTotal = 0;
  if (chunking) {
    #ifdef WM_DEBUG_LEVEL
    _sentUnbuffered = 0;
    _string.resetCount();
    #endif
    if (_string.reserve(chunkSize)) {
      #ifdef WM_DEBUG_LEVEL
      _capacity = _string.capacity();
      #endif
      beginChunking();
      sendStatusCode(code);
    }
    else {
      outOfMemory(chunkSize);
      reportLineNumber(__LINE__);
    }
  }
  else {
    _code = code;
  }
}

unsigned int Page::length() const {
  return _sentTotal + _string.length();
}

// Page::sendHeader() is a wrapper for the web server's sendHeader() that checks whether it is being called too late in the process of sending a page.
void Page::sendHeader(const String& name, const String& value) {
  if (begun && chunking) { // Already called begin() which called WebServer::send(), so its too late for more headers.
    #ifdef WM_DEBUG_LEVEL
    DEBUG_WM(WM_DEBUG_ERROR, String(F("Page::sendHeader() called too late")));
    DEBUG_WM(WM_DEBUG_DEV, String(F("Page::sendHeader() name=")), name);
    DEBUG_WM(WM_DEBUG_DEV, String(F("Page::sendHeader() value=")), value);
    #endif
    ASSERT(!(begun&& chunking));
  }
  else
    wifiManager->server->sendHeader(name, value);
}

void Page::sendChunk(const char *cstr, unsigned int len) {
  ASSERT(chunking);
  if (_error)
    return;
  if (!len)
    return;
  #ifdef WM_DEBUG_LEVEL
  DEBUG_WM(WM_DEBUG_DEV, String(F("Page::sendChunk: len=")), String(len, DEC));
  if (definedAtLineNumber)
    DEBUG_WM(WM_DEBUG_DEV, F(" page instantiated at line number="), (String)definedAtLineNumber);
  #endif
  wifiManager->server->sendContent(cstr, len);
  _sentTotal += len;
}

void Page::beginChunking() {
  ASSERT(chunking);
  // This sets chunking mode for both ESP8266 and ESP32.
  wifiManager->server->setContentLength(CONTENT_LENGTH_UNKNOWN);
}

void Page::endChunking() {
  ASSERT(chunking);
  // The ESP32 WebServer handles the final chunk in handleClient() but ESP8266WebServer needs us to do this.
  #if ARDUINO_ARCH_ESP8266
  wifiManager->server->chunkedResponseFinalize();
  #endif
}

bool Page::concat2(const char *cstr, unsigned int len) {
  if (_error)
    return false;
  if (!cstr)
    return false;
  if (len == 0)
    return true;
  begin();
  if (!chunking)
    return _string.concat(cstr, len) || outOfMemory(len);
  while (_string.length() + len >= _string.capacity()) { // There is a String buffer() and there is at least one full chunk to send.
    if (_string.length() == 0) { // There is nothing buffered so just send cstr as a single chunk.
      sendChunk(cstr, len);
      #if WM_DEBUG_LEVEL
      _sentUnbuffered += len;
      #endif
      len = 0;
    }
    else { // Something is buffered so combine it with enough of cstr to fill the buffer, making a chunk to send.
      const size_t n = min(len, _string.capacity() - _string.length()); // available space in String buffer()
      _string.concat(cstr, n);
      cstr += n; len -= n;
      sendChunk(_string.c_str(), _string.length());
      _string.clear();
    }
  }
  if (len) {  // There is a less than full chunk available, so save it in the String buffer.
    ASSERT(_string.length() + len < _string.capacity());
    _string.concat(cstr, len);
  }
  return true;
}

bool Page::concat(const __FlashStringHelper *fstr) {
  if (_error)
    return false;
  if (!fstr)
    return false;
  PGM_P str = (PGM_P)fstr;
  unsigned int len = strlen_P(str);
  if (len == 0)
    return true;
  begin();
  if (!chunking)
    return _string.concat(fstr) || outOfMemory(len);
  while (_string.length() + len >= _string.capacity()) { // There is a String buffer() and there is at least one full chunk to send.
    const size_t n = min(len, _string.capacity() - _string.length()); // available space in String buffer()
    // There is no method String::concat(const __FlashStringHelper *str, unsigned int n) so we do it the hard way.
    for (size_t i = 0; i != n; i++)
      _string.concat((char)pgm_read_byte(str + i));
    str += n; len -= n;
    sendChunk(_string.c_str(), _string.length());
    _string.clear();
  }
  if (len) { // There is less than a full chunk available, so save it in the String buffer.
    ASSERT(_string.length() + len < _string.capacity());
    _string.concat(FPSTR(str));
  }
  return true;  
}

// outOfMemory() may be called whether or not we are chunking.
// If chunking, it will only be called if the initial allocation to _string of one chunk of memory fails.
// If not chunking, it will be called if and when a capacity (re)allocation of _string fails in Page::concat() either called directly,
// called from operator+= or called from concat() or replace().
//
// Notes:
// 1. Since _string is no longer of any use, we free its memory because it may needed to send the debug messages and the error response.
// 2. outOfMemory() always returns false so that it can (optionally) be used in the shorthand manner of "return outOfMemory(additionaLength);"

bool Page::outOfMemory(unsigned int additionaLength) {
  // Setting _error results in a 503 HTTP response being sent, and stops further attempts to allocate memory to _buffer.
  _error = true;
  #ifdef WM_DEBUG_LEVEL
  // Remember some facts that will be destroyed by release().
  const unsigned int length = _string.length();
  const unsigned int capacity = _string.capacity();
  // Free any memory held by _buffer.
  _string.release();
  // Display diagnostics, one at level ERROR and some more at level DEV.
  DEBUG_WM(WM_DEBUG_ERROR, F("no free memory available for extension of page string"));
  DEBUG_WM(WM_DEBUG_VERBOSE, F(" required space="), (String)(length + additionaLength));
  DEBUG_WM(WM_DEBUG_VERBOSE, F(" existing capacity="), (String)capacity);
  DEBUG_WM(WM_DEBUG_VERBOSE, F(" existing length="), (String)length);
  DEBUG_WM(WM_DEBUG_VERBOSE, F(" additional length="), (String)additionaLength);
  if (definedAtLineNumber)
    DEBUG_WM(WM_DEBUG_DEV, F(" page instantiated at line number="), (String)definedAtLineNumber);
  #else
  // Not debugging, so we simply release any memory held by the string buffer.
  _string.release();
  (void)additionaLength;
  #endif
  return false;
}

bool Page::reportLineNumber(LineNumber calledAtLineNumber) {
  #ifdef WM_DEBUG_LEVEL
  if (errorAtLineNumber == 0 && calledAtLineNumber != 0) {
    errorAtLineNumber = calledAtLineNumber;
    DEBUG_WM(WM_DEBUG_DEV, F(" occured at line number="), (String)calledAtLineNumber);
  }
  #else
  (void)calledAtLineNumber;
  #endif
  return false;
}

// Page::replace() is Page::concat() with simultaneous replacement in the manner of String::replace().
// There are several overloads of it.

// This one is for content that is a traditional C/C++ string with a single target.
bool Page::replace(const char *cstr, const String& target, const String &substitute) {
  if (!cstr)
    return false;
  if (target.length() == 0) // Nothing to find, so do it the easier way.
    return concat(cstr);
  return _replace(ReplacementList(Replacement(target, &substitute)), strlen(cstr), [cstr](unsigned int offset) {return cstr[offset];});
}

// This one is for content that is a flash string with a single target.
bool Page::replace(const __FlashStringHelper *fstr, const String& target, const String &substitute) {
  if (!fstr)
    return false;
  if (target.length() == 0) // Nothing to find, so do it the easier way.
    return concat(fstr);
  PGM_P const str = (PGM_P)fstr;
  return _replace(ReplacementList(Replacement(target, &substitute)), strlen_P(str), [str](unsigned int offset) { return (char)pgm_read_byte(str + offset); });
}

// This one is for content that is a traditional C/C++ string with multiple targets.
bool Page::replace(const char *cstr, const ReplacementList& replacements) {
  if (!cstr)
    return false;
  if (replacements.empty())
    return concat(cstr);
  return _replace(replacements, strlen(cstr), [cstr](unsigned int offset) { return cstr[offset]; });
}

// This one is for content that is a flash string with multiple targets.
bool Page::replace(const __FlashStringHelper *fstr, const ReplacementList& replacements) {
  if (!fstr)
    return false;
  if (replacements.empty())
    return concat(fstr);
  PGM_P const str = (PGM_P)fstr;
  return _replace(replacements, strlen_P(str), [str](unsigned int offset) { return (char)pgm_read_byte(str + offset); });
}

// common function for the above overloads of replace()
// We detect and optimise the case where all targets are of the form "{...}" because it is the common case in WiFiManager.
bool Page::_replace(const ReplacementList& replacements, unsigned int stringLength, std::function<char (unsigned int offset)> getCharacter)
{
  if (stringLength == 0)
    return true;
  begin();
  #if OPTIMIZE_CURLY_TOKENS
  bool allCurlyTargets = true;
  for (auto replacement = replacements.begin(); replacement != replacements.end() && allCurlyTargets; replacement++)
    if (!(replacement->target.startsWith(FPSTR(T_ss)) && replacement->target.endsWith(FPSTR(T_es))))
      allCurlyTargets = false;
  if (allCurlyTargets)
    return _replaceCurlyTargets(replacements, stringLength, getCharacter);
  #endif
  return _replaceGeneralTargets(replacements, stringLength, getCharacter);
}

// This is for the general case of replace() when one or more of the targets in replacements[] are not of the form "{...}".
bool Page::_replaceGeneralTargets(const ReplacementList& replacements, unsigned int stringLength, std::function<char (unsigned int offset)> getCharacter) {
  const size_t replacementCount = replacements.size();
  size_t targetIndex[replacementCount];
  bool anyPartialMatch; // Whether any members of targetIndex[] are non-zero.
  auto reset = [&targetIndex, &anyPartialMatch, replacementCount]() {
    for (size_t j = 0; j != replacementCount; j++)
      targetIndex[j] = 0;
    anyPartialMatch = false;
  };
  reset();
  for (size_t i = 0; i != stringLength; i++) {
    bool fullMatch = false;
    bool partialMatch = false;
    const char c = getCharacter(i);
    const Replacement *result = nullptr;
    size_t j = 0;
    for (auto replacement = replacements.begin(); !fullMatch && replacement != replacements.end(); ++replacement, j++) {
      if ((bool)*replacement && c == replacement->target[targetIndex[j]] && stringLength - i >= replacement->target.length() - targetIndex[j]) {
        partialMatch = true;
        anyPartialMatch = true;
        if (++targetIndex[j] == replacement->target.length()) { // we have a full match
          fullMatch = true;
          result = &*replacement;
        }
      }
    }
    if (fullMatch) {
      ASSERT(result);
      result->substitute.concat(*this);
      reset();
    }
    else
    if (!partialMatch) {
      if (anyPartialMatch) {
        ASSERT(replacements.begin() != replacements.end());
        // Undo the longest previous partial match.
        size_t maxTargetIndex = 0;
        const Replacement *result = nullptr;
        size_t j = 0;
        for (auto replacement = replacements.begin(); replacement != replacements.end(); ++replacement, j++) {
          if (targetIndex[j] > maxTargetIndex) {
            maxTargetIndex = targetIndex[j];
            result = &*replacement;
          }
        }
        if (maxTargetIndex) {
          ASSERT(result);
          if (!concat2(result->target.c_str(), maxTargetIndex)) // targets are very short so making a String copy is OK.
            return false;
        }
        reset();
      }
      if (!concat(c))
        return false;
    }
  }
  return true;
}

#if OPTIMIZE_CURLY_TOKENS
// This method is for the special case of replace() when all of the targets in replacements[] are of the form "{...}".
// It is faster than replaceGeneralTargets() above because it searches the replacements[] only when a token of the form "{...}" has been seen in the string,
// rather than for every character in the string.
//
// We parse the string using a simple (two) state machine, extracting substrings of the form "{...}", looking them up in the replacements[] targets,
// and if found sending the corresponding substitute string instead. If not found, we send the "{...}" as is. If there is a '{' without a closing '}' then
// we just send what we got as is.
bool Page::_replaceCurlyTargets(const ReplacementList& replacements, unsigned int stringLength, std::function<char (unsigned int offset)> getCharacter) {
  // First, check our assumptions about the length of T_ss and T_es.
  ASSERT(strlen_P((PGM_P)T_ss) == 1);
  ASSERT(strlen_P((PGM_P)T_es) == 1);
  const char ss = (char)pgm_read_byte((PGM_P)T_ss);
  const char es = (char)pgm_read_byte((PGM_P)T_es);

  std::function<bool (const String& target, const String& token)> match;

  match = [] (const String& target, const String& token) {
    if (1 + token.length() + 1 != target.length())
      return false;
    for (size_t i = 0; i != token.length(); i++)
      if (target[1 + i] != token[i])
        return false;
    return true;
  };

  #if OPTIMIZE_CURLY_ONECHAR_TOKENS
  bool allOneCharTargets = true;
  for (auto replacement = replacements.begin(); replacement != replacements.end() && allOneCharTargets; replacement++)
    if (replacement->target.length() != 1+1+1)
      allOneCharTargets = false;
  if (allOneCharTargets)
    match = [] (const String& target, const String& token) {
      return token.length() == 1 && target[1] == token[0];
    };
  #endif

  auto find = [&replacements, &match](const String& token) {
    const Replacement *result = nullptr;
    for (auto replacement = replacements.begin(); replacement != replacements.end() && !result; replacement++)
      if (match(replacement->target, token))
        result = &*replacement;
    return result;
  };

  enum State {
    Normal,
    Curling,
  } state = Normal;

  String token;
  token.reserve(2); // "xy" part of "{xy}"

  for (size_t i = 0; i != stringLength; i++) {
    const char c = getCharacter(i);
    switch (state) {
      case Normal:
        if (c == ss) {
          state = Curling;
          token.clear();
        }
        else
        if (!concat(c))
          return false;
        break;
      case Curling:
        if (c == es) {
          const Replacement *const replacement = find(token);
          if (replacement) // replacement found
            replacement->substitute.concat(*this);
          else // replacement not found so send the token without replacement
          if (!(concat(es) && concat(token) && concat(ss)))
            return false;
          state = Normal;
        }
        else
          token += c;
        break;
    }
  }
  if (state == Curling) { // '}' at end of last token not found so send what we got without replacement
    return concat(ss) && concat(token);
  }
  return true;
}
#endif // OPTIMIZE_CURLY_TOKENS

void Page::sendStatusCode(uint16_t code) {
  #ifdef WM_DEBUG_LEVEL
  DEBUG_WM(WM_DEBUG_DEV, F("Page::sendStatusCode() code="), (String)code);
  #endif
  #if ARDUINO_ARCH_ESP32
  // The ESP32 WebServer library (as of 3.0.7) has a ?bug in send_P() whereby when given zero length content it disables chunking. Conversely, send() does not do this.
  // As a workaround, we use send() rather than send_P(). This requires making a normal memory copy of HTTP_HEAD_CT - fortunately it is short.
  wifiManager->server->send(code, String(FPSTR((PGM_P)HTTP_HEAD_CT)), emptyString);
  #else
  // Using send_P() works with the ESP8266WebServer library (as of 3.1.2).
  wifiManager->server->send_P(code, (PGM_P)HTTP_HEAD_CT, nullptr, 0);
  #endif
}

void Page::end() {
  if (!begun) // Either we have already been here, or we never got started.
    return;
  begun = false;
  if (_error) {
    _string.release();
    wifiManager->server->send(503, FPSTR(HTTP_HEAD_CT2), F("out of memory")); 
    return;
  }
  if (!chunking) {
    #ifdef WM_DEBUG_LEVEL
    DEBUG_WM(WM_DEBUG_DEV, F("Page::end() !chunking: length()="), (String)length());
    if (definedAtLineNumber)
      DEBUG_WM(WM_DEBUG_DEV, F(" page instantiated at line number="), (String)definedAtLineNumber);
    #endif
    wifiManager->server->setContentLength(_string.length());
    sendStatusCode(_code);
    wifiManager->server->sendContent(_string.c_str(), _string.length());
  } else { // chunking
    if (_string.length()) {
      sendChunk(_string.c_str(), _string.length());
      _string.clear();
    }
    endChunking();
    #ifdef WM_DEBUG_LEVEL
    DEBUG_WM(WM_DEBUG_DEV, F("Page::end() chunking: total length()="), (String)length());
    if (definedAtLineNumber)
      DEBUG_WM(WM_DEBUG_DEV, F(" page instantiated at line number="), (String)definedAtLineNumber);
    if (_capacity != _string.capacity()) {
      DEBUG_WM(WM_DEBUG_DEV, String(F("Page::end() _capacity=")), String(_capacity, DEC));
      DEBUG_WM(WM_DEBUG_DEV, String(F("Page::end() _string.capacity()=")), String(_string.capacity(), DEC));
    }
    if (_sentUnbuffered + _string.getCount() != _sentTotal)
      DEBUG_WM(WM_DEBUG_DEV, String(F(" _sentUnbuffered=")) + String(_sentUnbuffered, DEC) + String(F(" sent buffered=")) + String(_string.getCount(), DEC) + String(F(" _sentTotal=")) + String(_sentTotal, DEC));
    ASSERT(_capacity == _string.capacity());
    ASSERT(_sentUnbuffered + _string.getCount() == _sentTotal);
    #endif
  }
}

// class _String
Page::_String::_String(bool chunking) {
  #ifdef WM_DEBUG_LEVEL
  _chunking = chunking;
  #else
  (void)chunking;
  #endif
}

#ifdef _PAGE_WRAP_STRING
// These class _String methods wrap the corresponding class String methods for the purpose of debugging our chunking logic and/or testing handling of out-of-memory.
// If we are chunking, the calls to String::concat() should never fail. If we are not chunking, they may fail.
bool Page::_String::concat(const String& string) {
  return concat(string.c_str(), string.length());
}

bool Page::_String::concat(const char *string) {
  return concat(string, strlen(string));
}

bool Page::_String::concat(const char *string, unsigned int length) {
  _count += length;
  #if _PAGE_TEST_MEMORY_FAILURE
  if (!_chunking && this->length() + length >= _PAGE_TEST_MEMORY_FAILURE) // fabricate a memory failure
    return false;
  #endif
  const bool result = String::concat(string, length);
  ASSERT(!_chunking || result);
  return result;
}

bool Page::_String::concat(char c) {
  _count++;
  const bool result = String::concat(c);
  ASSERT(!_chunking || result);
  return result;
}

void Page::_String::clear() {
  String::clear();
  ASSERT(length() == 0);
}
#endif // _PAGE_WRAP_STRING

// class Page::Replacement

// constructors
// We have the full set to avoid making any unecessary copies.
// Normal constructors are defined in page.h

// Copy constructor
Page::Replacement::Replacement(const Replacement& rhs) {
  copy(rhs);
}

// Move constructor
Page::Replacement::Replacement(Replacement&& rhs)  noexcept {
  move(rhs);
}

// copy assignment
Page::Replacement& Page::Replacement::operator = (const Page::Replacement& rhs) {
  copy(rhs);
  return *this;
}

// move assignment
Page::Replacement& Page::Replacement::operator = (Page::Replacement&& rhs) noexcept {
  move(rhs);
  return *this;
}

// copy helper
void Page::Replacement::copy(const Replacement& rhs) {
  if (this == &rhs)
    return;
  target = rhs.target;
  substitute = rhs.substitute;
}

// move helper
void Page::Replacement::move(Replacement& rhs) {
  if (this == &rhs)
    return;
  target = std::move(rhs.target);
  substitute = std::move(rhs.substitute);
}

// class ReplacementList

// constructors

Page::ReplacementList::ReplacementList(const Replacement& replacement) {
  *this += replacement;
}

Page::ReplacementList::ReplacementList(Replacement&& replacement) {
  *this += std::move(replacement);
}

// std::forward_list<> does not define size() so we do it, albeit the slow way because it is simpler, uses less memory and its speed doesn't matter.
size_t Page::ReplacementList::size() const {
  size_t size = 0;
  for (auto x: *this)
    size++;
  return size;
}

// class Page::Replacement::MultiTypeString

// constructors

Page::Replacement::MultiTypeString::MultiTypeString():
  value(noValue)
{
  ASSERT(getType() == Type::None);
}

Page::Replacement::MultiTypeString::MultiTypeString(char c):
  value(c)
{
  ASSERT(getType() == Type::Char);
}

Page::Replacement::MultiTypeString::MultiTypeString(const char *cstring):
  value(cstring ? cstring : "")
{
  ASSERT(getType() == Type::Cstring);
}

Page::Replacement::MultiTypeString::MultiTypeString(const __FlashStringHelper *fstring):
  value(fstring ? fstring : F(""))
{
  ASSERT(getType() == Type::Fstring);
}

// The argument is an rvalue reference to a String.
// We move it rather than copy it.
Page::Replacement::MultiTypeString::MultiTypeString(String&& sstring)
{
  value = std::move(sstring);

  ASSERT(getType() == Type::Sstring);
}

// The argument is an lvalue reference to a String.
// We have to copy it because we don't know its lifetime or whether it will change before we read it.
Page::Replacement::MultiTypeString::MultiTypeString(const String& sstring):
  value(sstring)
{
  ASSERT(getType() == Type::Sstring);
}

Page::Replacement::MultiTypeString::MultiTypeString(const String *pstring):
  value(pstring ? pstring : &emptyString)
{
  ASSERT(getType() == Type::Pstring);
}

// copy constructor
Page::Replacement::MultiTypeString::MultiTypeString(const MultiTypeString& rhs) {
  copy(rhs);
}

// move constructor
Page::Replacement::MultiTypeString::MultiTypeString(MultiTypeString&& rhs) noexcept {
  move(rhs);
}

// copy assignment
Page::Replacement::MultiTypeString& Page::Replacement::MultiTypeString::operator = (const MultiTypeString& rhs) {
  copy(rhs);
  return *this;
}

// move assignment
Page::Replacement::MultiTypeString& Page::Replacement::MultiTypeString::operator = (MultiTypeString&& rhs) noexcept {
  move(rhs);
  return *this;
}

char Page::Replacement::MultiTypeString::operator [] (unsigned int i) const {
  ASSERT(i < length());
  switch (getType()) {
    case Type::None:
      ASSERT(getType() != Type::None);
      break;
    case Type::Char:
      ASSERT(i == 0);
      return std::get<char>(value);
    case Type::Cstring:
      ASSERT(std::get<const char *>(value));
      return std::get<const char *>(value)[i];
    case Type::Fstring:
      return (char)pgm_read_byte((PGM_P)std::get<const __FlashStringHelper *>(value) + i);
    case Type::Sstring:
      return std::get<String>(value)[i];
    case Type::Pstring:
      ASSERT(std::get<const String *>(value));
      return (*(std::get<const String *>(value)))[i];
  }
  return '*'; // cannot happen
}

unsigned int Page::Replacement::MultiTypeString::length() const {
  switch (getType()) {
    case Type::None:
      ASSERT(getType() != Type::None);
      break;
    case Type::Char:
      return 1;
    case Type::Cstring:
      ASSERT(std::get<const char *>(value));
      return strlen(std::get<const char *>(value));
    case Type::Fstring:
      ASSERT(std::get<const __FlashStringHelper *>(value));
      return strlen_P((PGM_P)std::get<const __FlashStringHelper *>(value));
    case Type::Sstring:
      return std::get<String>(value).length();
    case Type::Pstring:
      ASSERT(std::get<const String *>(value));
      return std::get<const String *>(value)->length();
  }
  return 0;
}

void Page::Replacement::MultiTypeString::concat(Page& page) const {
  switch (getType()) {
    case Type::None:
      break;
    case Type::Char:
      page.concat(std::get<char>(value));
      break;
    case Type::Cstring:
      ASSERT(std::get<const char *>(value));
      page.concat(std::get<const char *>(value));
      break;
    case Type::Fstring:
      ASSERT(std::get<const __FlashStringHelper *>(value));
      page.concat(std::get<const __FlashStringHelper *>(value));
      break;
    case Type::Sstring:
      page.concat(std::get<String>(value));
      break;
    case Type::Pstring:
      ASSERT(std::get<const String *>(value));
      page.concat(*std::get<const String *>(value));
      break;
    }
}

// copy helper
void Page::Replacement::MultiTypeString::copy(const MultiTypeString& rhs) {
  if (this == &rhs)
      return;

  value = rhs.value;

  ASSERT(getType() == rhs.getType());
  ASSERT(length() == rhs.length());
}

// move helper
// The benefit of defining move constructors/assignments is only for the case of Type::Sstring because (we hope that) it avoids copying its String.
// The other types only copy pointers anyway.
// In the case of a Type::Sstring, after its resources are moved, we invalidate the rhs because we have changed it.
void Page::Replacement::MultiTypeString::move(MultiTypeString& rhs) {
  if (this == &rhs)
      return;

  value = std::move(rhs.value);

  ASSERT(value.index() == rhs.value.index());
  ASSERT(getType() == rhs.getType());

  // @todo Is this the best way to invalidate the RHS or does std::variant<> have a better way?
  rhs.value = noValue;
  ASSERT(rhs.getType() == Type::None);
}

#ifdef WM_DEBUG_LEVEL
// The method MultiTypeString::toString() is for debugging purposes only (otherwise, we avoid making String copies of things that may be large).
String Page::Replacement::MultiTypeString::toString() const {
  switch (getType())
  {
  case Type::None:
    break;
  case Type::Char:
    return String(std::get<char>(value));
  case Type::Cstring:
    return std::get<const char *>(value);
  case Type::Fstring:
    return std::get<const __FlashStringHelper *>(value);
  case Type::Sstring:
    return std::get<String>(value);
  case Type::Pstring:
    return *std::get<const String *>(value);
  }
  return emptyString;
}
#endif // WM_DEBUG_LEVEL
