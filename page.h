#ifndef _WIFIMANAGER_PAGE_H_
#define _WIFIMANAGER_PAGE_H_
#include <variant>
#include <forward_list>
#include "WiFiManager.h"

// Whether WM_DEFAULTCHUNKING is defined or not in WiFiManager.h determines the default state of chunking for all instances of Page.
// Individual instances can still chunk or not chunk based on the constructor's second parameter.
#ifdef WM_DEFAULTCHUNKING
#define _PAGE_DEFAULT_CHUNKING true
#else
#define _PAGE_DEFAULT_CHUNKING false
#endif

// For testing: when not chunking only, a failure will be induced if a page string reaches this size, if it is defined and non-zero.
// #define _PAGE_TEST_MEMORY_FAILURE 4000 // bytes

// See PAGE.md for a description of this class.
class Page
{
public:
  typedef uint16_t LineNumber;

  // The line number, if any, passed to the constructor is the line number at which the construction of an instance of class Page occurs. Zero means unknown.
  Page(WiFiManager *wifiManager, bool chunking = _PAGE_DEFAULT_CHUNKING, unsigned int chunkSize = 0, LineNumber definedAtLineNumber = 0);
  ~Page();

  // begin() initiates the WebServer's HTTP chunking protocol and sends the first line of the HTTP response.
  // begin() is automatically called when the first content is added to an instance of Page.
  // It can be explicitly called, e.g. if an HTTP status code other than 200 is desired (but should not be called before any calls to sendHeader()).
  void begin(uint16_t code = 200);
  // end() terminates the WebServer's HTTP chunking protocol, sending any remaining content.
  void end();
  // length() returns the total number of bytes added to the page so far, whether sent or waiting to send.
  unsigned int length() const;
  // sendHeader() wraps WebServer::sendHeader() with a pre-check that it is not to late to add a header.
  void sendHeader(const String& name, const String& value);

  // The concat() methods have signatures to match the String::concat() methods and hence the data types supported on the RHS of operator +=
  // The first two concat()s below actually do the work for us, the others just wrap them.
  bool concat2(const char *cstr, unsigned int length);
  bool concat(const __FlashStringHelper *fstr);

  bool concat(const String &str) { return concat2(str.c_str(), str.length()); }
  bool concat(const char *cstr) { return cstr && concat2(cstr, strlen(cstr)); }
  bool concat(char c) { return concat2(&c, 1); }
  inline bool concat(unsigned char c) { return concat((char)c); }
  bool concat(int num) { return concat(String(num)); }
  bool concat(unsigned int num) { return concat(String(num)); }
  bool concat(long num) { return concat(String(num)); }
  bool concat(unsigned long num) { return concat(String(num)); }
  bool concat(long long num) { return concat(String(num)); }
  bool concat(unsigned long long num) { return concat(String(num)); }
  bool concat(float num) { return concat(String(num)); }
  bool concat(double num) { return concat(String(num)); }

  template <typename T>
  Page &operator +=(const T &rhs) {
      concat(rhs);
      return *this;
  }

  // These replace() methods are not the same as the String::replace() methods because they replace() and concat() at the same time.
  // The classes Replacement/MultiTypeString may appear messy but that is because they cater for four different data types for the substitute.
  // The goal is to avoid making copies of strings that may be large. In essence an instance of Replacement is a (target, substitute) pair of strings.

  class Replacement {
    friend class ReplacementList;

  public:
    // We declare constructors for targets of type String only, because that is what we need to support.
    // If targets were ever single characters, flash strings or pointers to String, those constructors could be added here.

    // Constructor for use with traditional C/C++ strings e.g. WiFiManager::_customHTML
    Replacement(const String& target, const char *cstring): target(target), substitute(cstring) {}
    // Constructor for use with FPSTR()s
    Replacement(const String& target, const __FlashStringHelper *fstring) :target(target), substitute(fstring) {}
    // Constructor for use with temporary Strings - it moves rather than copies the String
    Replacement(const String& target, String&& sstring): target(target), substitute(std::move(sstring)) {}
    // Constructor for use with stored Strings - it copies the String
    Replacement(const String& target, const String& sstring): target(target), substitute(sstring) {}
    // Constructor for use with non-temporary Strings because it does not make a copy of the String, just of the pointer
    Replacement(const String& target, const String *pstring): target(target), substitute(pstring) {}
    // Default constructor
    Replacement(): substitute() {}
    // Copy constructor
    Replacement(const Replacement& rhs);
    // Move constructor
    Replacement(Replacement&& rhs) noexcept;

    // bool operator - an instance of Replacement tests as true if it has a valid substitute.
    explicit operator bool() const { return (bool)substitute; } // Declared explicit so that a Replacement is not implicitly cast to an int-type.
    // copy assigment operator
    Replacement& operator = (const Replacement& rhs);
    // move assigment operator
    Replacement& operator = (Replacement&& rhs) noexcept;

    // MultiTypeString is a fancy "union" of different types of character string.
    // We use it as the type for the "substitute" parameter in order to avoid having many overloads of the replace methods.
    class MultiTypeString {
    public:
      enum Type
      {
        // We use a traditional enum rather than an "enum class" so that they are interchangable with std::variant<>::index()
        // We don't actually declare a data member of type Type - the current Type is given by value.index()
        // It is important that the members of Type are in the same order as the template parameters for value.
        None,    // none                          - indicates an invalid instance as constructed by the default constructor
        Char,    // char                          - for use with a single character (not used anywhere?)
        Cstring, // const char *                  - for use with traditional C/C++ strings e.g. WiFiManager::_customHTML
        Fstring, // const __FlashStringHelper *   - for use with FPSTR()s
        Sstring, // String                        - for use with temporary strings because it keeps a copy
        Pstring, // const String *                - for sharing non-temporary strings because it does not make a copy
      };
      Type getType() const { return (Type)value.index(); }

    private:
      // There is only one data member.
      // It is important that the template parameters for value are in the same order as enum Type above.
      // Note that bool is an unused type we chose to correspond to Type::None.
      std::variant<bool, char, const char *, const __FlashStringHelper *, String, const String *> value;
      const static bool noValue = false; // Type is bool because of above, value doesn't matter.

    public:
      // constructors
      // In the case of a the constructors that take String parameters, the overloading resolution rules should choose the rvalue version
      // (which does a String move) for a temporary object and the lvalue version (which copies) for a stored object. If you have a stored String
      // with a lifetime no shorter than the MultiTypeString, using Pstring will avoid a copy.
      MultiTypeString();                                            // None
      explicit MultiTypeString(char c);                             // Char
      explicit MultiTypeString(const char *cstring);                // Cstring (only pointer copied so be sure it is safe to use this one)
      explicit MultiTypeString(const __FlashStringHelper *fstring); // Fstring (only pointer copied)
      explicit MultiTypeString(String &&sstring);                   // Sstring from an rvalue reference (moved)
      explicit MultiTypeString(const String& sstring);              // Sstring from an lvalue reference (copied)
      explicit MultiTypeString(const String *pstring);              // Pstring (only pointer copied so be sure it is safe to use this one)

      MultiTypeString(const MultiTypeString& rhs);                  // copy constructor (copies the String if type Sstring)
      MultiTypeString& operator = (const MultiTypeString& rhs);     // copy assignment  (copies the String if type Sstring)
      MultiTypeString(MultiTypeString&& rhs) noexcept;              // move constructor (moves rather than copies the String if type Sstring)
      MultiTypeString& operator = (MultiTypeString&& rhs) noexcept; // move assignment  (ditto)

      // A instance of Replacement tests as true if it has a valid type.
      explicit operator bool() const { return getType() != Type::None; } // Declared explicit so that a MultiTypeString is not implicitly cast to an int-type.

      // The [] operator picks an indexed character out of the string (as you would expect).
      char operator [] (unsigned int i) const;

      // How many characters in a MultiTypeString?
      unsigned int length() const;
      // Given a MultiTypeString, its value can be set by concat().
      void concat(Page& page) const;

      // Debugging support
      #ifdef WM_DEBUG_LEVEL
      String toString() const;
      #endif

    private:
      // Helpers for the constructors and operators.
      void copy(const MultiTypeString& rhs);
      void move(MultiTypeString& rhs);
    }; // class MultiTypeString

    // Finally, the data members.
    String target;
    MultiTypeString substitute;

  private:
    void copy(const Replacement& rhs);
    void move(Replacement& rhs);
  }; // class Replacement

  typedef std::forward_list<Replacement> _ReplacementList;

  class ReplacementList: private _ReplacementList {
  public:
    ReplacementList() {}
    ReplacementList(const Replacement& replacement);
    ReplacementList(Replacement&& replacement);

    size_t size() const;

    using _ReplacementList::begin;
    using _ReplacementList::end;
    using _ReplacementList::empty;

    ReplacementList& operator += (const Replacement& rhs) { if (rhs) push_front(rhs); return *this; }
    ReplacementList& operator += (Replacement&& rhs)      { if (rhs) push_front(std::move(rhs)); return *this; }
  }; // class ReplacementList

  // class Page again ...
  // There are two general replace() methods, one for content in traditional C/C++ strings and one for flash strings.
  bool replace(const char *cstr, const ReplacementList& replacements);
  bool replace(const __FlashStringHelper *fstr, const ReplacementList& replacements);
  // These two are simplified overloads for use when there is only one target/substitute pair.
  bool replace(const char *cstr, const String& target, const String &substitute);
  bool replace(const __FlashStringHelper *fstr, const String& target, const String &substitute);
  // For content in a String, there are these two wrappers.
  inline bool replace(const String& str, const ReplacementList& replacements) {
    return replace(str.c_str(), replacements);
  }
  inline bool  replace(const String& str, const String& target, const String &substitute) {
    return replace(str.c_str(), target, substitute);
  }

private:
  // Page needs access to some members of WiFiManager, i.e. the web server and the debug methods.
  WiFiManager *const wifiManager;
  // Line number at which this instance was created is stored for use in error reports. Zero means no line number was provided to the constructor.
  const LineNumber definedAtLineNumber;
  // chunking determines whether we chunk or not.
  const bool chunking;
  // begun is true between calls to begin() and end(), otherwise it is false.
  bool begun = false;
  // outOfMemory() is called if we encounter an out of memory situation; it sets _error to true so that messages are not repeated.
  bool outOfMemory(unsigned int additionaLength);
  bool _error = false;

  // For non-chunking mode only ...
  uint16_t _code = 200; // The response status code stored by begin() to be sent by end() unless there was an error
  // For chunking mode only ...
  size_t chunkSize;     // chunkSize is the currently requested chunk size (actual chunk size used may be slightly larger depending on the String library)
  void beginChunking(); // put WebServer into chunking mode
  void endChunking();   // complete WebServer's chunking mode
  void sendChunk(const char *cstr, unsigned int len); // send one chunk
  unsigned int _sentTotal; // The total number of bytes already sent by sendChunk(). Needed by Page::length() when chunking.
  #ifdef WM_DEBUG_LEVEL
  // For debugging support ...
  unsigned int _capacity;           // _string.capacity() when it was first reserve()d
  unsigned int _sentUnbuffered = 0; // The number of bytes sent by sendChunk() without going via _string.
  #endif

  // sends the response status code
  void sendStatusCode(uint16_t code);

  // private helpers for public replace() methods
  bool _replace(const ReplacementList& replacements, unsigned int stringLength, std::function<char (unsigned int offset)> getCharacter);
  bool _replaceGeneralTargets(const ReplacementList& replacements, unsigned int stringLength, std::function<char (unsigned int offset)> getCharacter);
  bool _replaceCurlyTargets(const ReplacementList& replacements, unsigned int stringLength, std::function<char (unsigned int offset)> getCharacter);

  // The currently-being-assembled string is stored in an instance of _String. It is used as the buffer in both the chunking and the non-chunking modes.
  // Class _String is derived from class String for the purposes of:
  // 1. exposing the protected method String::capacity() so that know how long a string can be without triggering an increase in buffer size.
  // 2. wrapping some of the String::concat() methods with debugging and/or testing code.
  // Notes:
  // 1. _String adds one int (_count) and one bool (_chunking) to the size of String when WM_DEBUG_LEVEL or _PAGE_TEST_MEMORY_FAILURE is defined, otherwise
  //    its size is the same as String.
  // 2. Using capacity() relies on an internal detail of class String. However, because it is *protected* by class String rather
  //    than *private*, we decided that it is safe to use it.
  // 3. _String methods have assert()s that need to know whether we are chunking or not, hence the constructor parameter.
  #ifdef WM_DEBUG_LEVEL
  #define _PAGE_WRAP_STRING
  #endif
  #if _PAGE_TEST_MEMORY_FAILURE
  #define _PAGE_WRAP_STRING
  #endif
  
  class _String: public String {
  public:
    _String(bool chunking);

     // The capacity() method is protected in class String but we want access to it for _string below.
    using String::capacity;
    // The release() method frees any dynamic memory assigned to the string buffer.
    void release() { clear(); reserve(0); }

    // These methods wrap the corresponding String methods for the purpose of debugging our chunking logic (as opposed to WiFiManager or the app using WiFiManager),
    // or for testing handling of out-of-memory situations.
    // If we do not have either enabled, the inherited String methods will be called, saving code space.
  #ifdef _PAGE_WRAP_STRING
  public:
    bool concat(const String& string);
    bool concat(const char *string);
    bool concat(const char *string, unsigned int length);
    bool concat(char c);
    void clear();
  private:
    bool _chunking;             // copy of Page::chunking for the assert()s used in debug mode
    unsigned int _count = 0;    // the total number of bytes added to this instance of _String since Page::begin()
  #else
  public:
    inline void resetCount() {}
  #endif

  #ifdef WM_DEBUG_LEVEL
  public:
    // setters and getter for _count - only used when chunking
    unsigned int getCount() const { return _count; }
    void resetCount() { _count = 0; }
  #endif // WM_DEBUG_LEVEL
  };
  _String _string;              // the partial page content buffer when chunking; the full page content buffer when not chunking

  // Define DEBUG_WM() as shorthand for wifiManager->DEBUG_WM()
  void DEBUG_WM(wm_debuglevel_t level, const String& text1, const String& text2 = emptyString) {
    wifiManager->DEBUG_WM(level, text1, text2);
  }

public:
  // reportLineNumber() is used by the macros below. The first time only, it sets errorAtLineNumber to calledAtLineNumber.
  bool reportLineNumber(LineNumber calledAtLineNumber);
private:
  LineNumber errorAtLineNumber = 0;
};

// The macro PAGE defines an instance of type Page with line number captured if debugging, otherwise without line number so as to reduce code size. The
// line number is of the line where the instance is defined, *not* where it is subsequently used.
//   PAGE(page);             // default chunking with default chunk size
//   PAGE2(page, 256);       // default chunking with a non-default chunk size of 256 (if chunking)
//   PAGE3(page, true, 256); // chunking with a non-default chunk size of 256 
// Note that PAGE/PAGE2/PAGE3 can only be used in a member method of class Page because of their use of "this".
// The APPEND macro is an alternative to the += operator for use when an out of memory condition may occur and detail of *where* that occured is required.
// The REPLACE macros are alternatives to the Page::replace() methods for use when an out of memory condition may occur and detail of *where* that occured is required.
#ifdef WM_DEBUG_LEVEL
#define PAGE(p)           Page p(this, _PAGE_DEFAULT_CHUNKING, 0, __LINE__)  // default chunking, default chunk size
#define PAGE2(p,s)        Page p(this, _PAGE_DEFAULT_CHUNKING, (s), __LINE__)  // Define instance x of type Page with line number of instantiation for debugging purposes.
#define PAGE3(p,c,s)      Page p(this, (c), (s), __LINE__)  // Ditto, with a specific chunking state.
#define APPEND(p,s)       ((p).concat((s)) || (p).reportLineNumber(__LINE__))
#define APPEND2(p,s,n)    ((p).concat2((s), (n)) || (p).reportLineNumber(__LINE__))
// The next two macros are alternatives to the replace() methods for use when an out of memory condition may occur and detail of where it occured is required.
#define REPLACE2(p,s,r)   ((p).replace((s), (r)) || (p).reportLineNumber(__LINE__))
#define REPLACE3(p,s,t,r) ((p).replace((s), (t), (r)) || (p).reportLineNumber(__LINE__))
#else // !WM_DEBUG_LEVEL
#define PAGE(p)           Page p(this)
#define PAGE2(p,s)        Page p(this, _PAGE_DEFAULT_CHUNKING, (s))
#define PAGE3(p,c,s)      Page p(this, (c), (s))
#define APPEND(p,s)       ((p).concat((s)))
#define APPEND2(p,s,n)    ((p).concat2((s), (n)))
#define REPLACE2(p,s,r)   ((p).replace((s), (r)))
#define REPLACE3(p,s,t,r) ((p).replace((s), (t), (r)))
#endif

#endif // _WIFIMANAGER_PAGE_H_
