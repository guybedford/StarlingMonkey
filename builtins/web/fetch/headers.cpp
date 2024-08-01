#include "headers.h"
#include "builtin.h"
#include "decode.h"
#include "encode.h"
#include "fetch-errors.h"
#include "sequence.hpp"

#include "js/Conversions.h"
#include <numeric>

namespace builtins::web::fetch {
namespace {

const char VALID_NAME_CHARS[128] = {
    0, 0, 0, 0, 0, 0, 0, 0, //   0
    0, 0, 0, 0, 0, 0, 0, 0, //   8
    0, 0, 0, 0, 0, 0, 0, 0, //  16
    0, 0, 0, 0, 0, 0, 0, 0, //  24

    0, 1, 0, 1, 1, 1, 1, 1, //  32
    0, 0, 1, 1, 0, 1, 1, 0, //  40
    1, 1, 1, 1, 1, 1, 1, 1, //  48
    1, 1, 0, 0, 0, 0, 0, 0, //  56

    0, 1, 1, 1, 1, 1, 1, 1, //  64
    1, 1, 1, 1, 1, 1, 1, 1, //  72
    1, 1, 1, 1, 1, 1, 1, 1, //  80
    1, 1, 1, 0, 0, 0, 1, 1, //  88

    1, 1, 1, 1, 1, 1, 1, 1, //  96
    1, 1, 1, 1, 1, 1, 1, 1, // 104
    1, 1, 1, 1, 1, 1, 1, 1, // 112
    1, 1, 1, 0, 1, 0, 1, 0  // 120
};

#define NORMALIZE_NAME(name, fun_name)                                                             \
  bool name_changed;                                                                               \
  auto name_chars = normalize_header_name(cx, name, &name_changed, fun_name);                      \
  if (!name_chars) {                                                                               \
    return false;                                                                                  \
  }

#define NORMALIZE_VALUE(value, fun_name)                                                           \
  bool value_changed;                                                                              \
  auto value_chars = normalize_header_value(cx, value, &value_changed, fun_name);                  \
  if (!value_chars.ptr) {                                                                          \
    return false;                                                                                  \
  }

host_api::HttpHeadersReadOnly *get_handle(JSObject *self) {
  MOZ_ASSERT(Headers::is_instance(self));
  auto handle =
      JS::GetReservedSlot(self, static_cast<uint32_t>(Headers::Slots::Handle)).toPrivate();
  return static_cast<host_api::HttpHeadersReadOnly *>(handle);
}

/**
 * Validates and normalizes the given header name, by
 * - checking for invalid characters
 * - converting to lower-case
 *
 * See
 * https://searchfox.org/mozilla-central/rev/9f76a47f4aa935b49754c5608a1c8e72ee358c46/netwerk/protocol/http/nsHttp.cpp#172-215
 * For details on validation.
 */
host_api::HostString normalize_header_name(JSContext *cx, HandleValue name_val, bool *named_changed,
                                           const char *fun_name) {
  *named_changed = !name_val.isString();
  JS::RootedString name_str(cx, JS::ToString(cx, name_val));
  if (!name_str) {
    return nullptr;
  }

  auto name = core::encode(cx, name_str);
  if (!name) {
    return nullptr;
  }

  if (name.len == 0) {
    api::throw_error(cx, FetchErrors::EmptyHeaderName, fun_name);
    return nullptr;
  }

  char *name_chars = name.begin();
  for (size_t i = 0; i < name.len; i++) {
    const unsigned char ch = name_chars[i];
    if (ch > 127 || !VALID_NAME_CHARS[ch]) {
      api::throw_error(cx, FetchErrors::InvalidHeaderName, fun_name, name_chars);
      return nullptr;
    }

    if (ch >= 'A' && ch <= 'Z') {
      *named_changed = true;
      name_chars[i] = ch - 'A' + 'a';
    }
  }

  return name;
}

/*
 * Validates a header name by checking for invalid characters
 */
// bool validate_header_name(JSContext *cx, host_api::HostString name, const char *fun_name) {
//   if (name.len == 0) {
//     api::throw_error(cx, FetchErrors::EmptyHeaderName, fun_name);
//     return false;
//   }
//   char *name_chars = name.begin();
//   for (size_t i = 0; i < name.len; i++) {
//     const unsigned char ch = name_chars[i];
//     if (ch > 127 || !VALID_NAME_CHARS[ch]) {
//       api::throw_error(cx, FetchErrors::InvalidHeaderName, fun_name, name_chars);
//       return false;
//     }
//   }
//   return true;
// }

/**
 * Validates and normalizes the given header value, by
 * - stripping leading and trailing whitespace
 * - checking for interior line breaks and `\0`
 *
 * See
 * https://searchfox.org/mozilla-central/rev/9f76a47f4aa935b49754c5608a1c8e72ee358c46/netwerk/protocol/http/nsHttp.cpp#247-260
 * For details on validation.
 */
host_api::HostString normalize_header_value(JSContext *cx, HandleValue value_val,
                                            bool *value_changed, const char *fun_name) {
  *value_changed = !value_val.isString();
  JS::RootedString value_str(cx, JS::ToString(cx, value_val));
  if (!value_str) {
    return nullptr;
  }

  auto value = core::encode(cx, value_str);
  if (!value.ptr) {
    return nullptr;
  }

  auto *value_chars = value.begin();
  size_t start = 0;
  size_t end = value.len;

  while (start < end) {
    unsigned char ch = value_chars[start];
    if (ch == '\t' || ch == ' ' || ch == '\r' || ch == '\n') {
      start++;
    } else {
      break;
    }
  }

  while (end > start) {
    unsigned char ch = value_chars[end - 1];
    if (ch == '\t' || ch == ' ' || ch == '\r' || ch == '\n') {
      end--;
    } else {
      break;
    }
  }

  if (start != 0 || end != value.len) {
    *value_changed = true;
  }

  for (size_t i = start; i < end; i++) {
    unsigned char ch = value_chars[i];
    if (ch == '\r' || ch == '\n' || ch == '\0') {
      api::throw_error(cx, FetchErrors::InvalidHeaderValue, fun_name, value_chars);
      return nullptr;
    }
  }

  if (*value_changed) {
    memmove(value_chars, value_chars + start, end - start);
    value.len = end - start;
  }

  return value;
}

JS::PersistentRooted<JSString *> comma;

bool retrieve_value_for_header_from_handle(JSContext *cx, JS::HandleObject self,
                                           const host_api::HostString &name,
                                           MutableHandleValue value) {
  auto handle = get_handle(self);
  auto ret = handle->get(name);

  if (auto *err = ret.to_err()) {
    HANDLE_ERROR(cx, *err);
    return false;
  }

  auto &values = ret.unwrap();
  if (!values.has_value()) {
    value.setNull();
    return true;
  }

  RootedString res_str(cx);
  RootedString val_str(cx);
  for (auto &str : values.value()) {
    val_str = JS_NewStringCopyN(cx, reinterpret_cast<char *>(str.ptr.get()), str.len);
    if (!val_str) {
      return false;
    }

    if (!res_str) {
      res_str = val_str;
    } else {
      res_str = JS_ConcatStrings(cx, res_str, comma);
      if (!res_str) {
        return false;
      }
      res_str = JS_ConcatStrings(cx, res_str, val_str);
      if (!res_str) {
        return false;
      }
    }
  }

  value.setString(res_str);
  return true;
}

// std::string_view special_chars = "=,;";

// std::vector<std::string_view> splitCookiesString(std::string_view cookiesString) {
//   std::vector<std::string_view> cookiesStrings;
//   std::size_t currentPosition = 0; // Current position in the string
//   std::size_t start;               // Start position of the current cookie
//   std::size_t lastComma;           // Position of the last comma found
//   std::size_t nextStart;           // Position of the start of the next cookie

//   // Iterate over the string and split it into cookies.
//   while (currentPosition < cookiesString.length()) {
//     start = currentPosition;

//     // Iterate until we find a comma that might be used as a separator.
//     while ((currentPosition = cookiesString.find_first_of(',', currentPosition)) !=
//            std::string_view::npos) {
//       // ',' is a cookie separator only if we later have '=', before having ';' or ','
//       lastComma = currentPosition;
//       nextStart = ++currentPosition;

//       // Check if the next sequence of characters is a non-special character followed by an
//       equals
//       // sign.
//       currentPosition = cookiesString.find_first_of(special_chars, currentPosition);

//       // If the current character is an equals sign, we have found a cookie separator.
//       if (currentPosition != std::string_view::npos && cookiesString.at(currentPosition) == '=')
//       {
//         // currentPosition is inside the next cookie, so back up and return it.
//         currentPosition = nextStart;
//         cookiesStrings.push_back(cookiesString.substr(start, lastComma - start));
//         start = currentPosition;
//       } else {
//         // The cookie contains ';' or ',' as part of the value
//         // so we need to keep accumulating characters
//         currentPosition = lastComma + 1;
//       }
//     }

//     // If we reach the end of the string without finding a separator, add the last cookie to the
//     // vector.
//     if (currentPosition >= cookiesString.length()) {
//       cookiesStrings.push_back(cookiesString.substr(start, cookiesString.length() - start));
//     }
//   }
//   return cookiesStrings;
// }

static std::vector<string_view> forbidden_request_headers;
static std::vector<string_view> forbidden_response_headers;

enum class Ordering { Less, Equal, Greater };

inline char header_lowercase(const char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

inline Ordering header_compare(const std::string_view a, const std::string_view b) {
  auto it_a = a.begin();
  auto it_b = b.begin();
  while (it_a != a.end() && it_b != b.end()) {
    char ca = header_lowercase(*it_a);
    char cb = header_lowercase(*it_b);
    if (ca < cb) {
      return Ordering::Less;
    } else if (ca > cb) {
      return Ordering::Greater;
    }
    ++it_a;
    ++it_b;
  }
  if (it_a == a.end()) {
    return it_b == b.end() ? Ordering::Equal : Ordering::Less;
  } else {
    return Ordering::Greater;
  }
}

struct HeaderCompare {
  bool operator()(const std::string_view a, const std::string_view b) {
    return header_compare(a, b) == Ordering::Less;
  }
};

class HeadersSortListCompare {
  const Headers::HeadersList *headers_;

public:
  HeadersSortListCompare(const Headers::HeadersList *headers) : headers_(headers) {}

  bool operator()(size_t a, size_t b) {
    auto header_a = &std::get<0>(headers_->at(a));
    auto header_b = &std::get<0>(headers_->at(b));
    return header_compare(*header_a, *header_b) == Ordering::Less;
  }
};

class HeadersSortListLookupCompare {
  const Headers::HeadersList *headers_;

public:
  HeadersSortListLookupCompare(const Headers::HeadersList *headers) : headers_(headers) {}

  bool operator()(size_t a, string_view b) {
    auto header_a = &std::get<0>(headers_->at(a));
    return header_compare(*header_a, b) == Ordering::Less;
  }
};

// Update the sort list
void ensure_updated_sort_list(const Headers::HeadersList *headers_list,
                              std::vector<size_t> *headers_sort_list) {
  // Empty length means we need to recompute.
  if (headers_sort_list->size() == 0) {
    headers_sort_list->resize(headers_list->size());
    std::iota(headers_sort_list->begin(), headers_sort_list->end(), 0);
    std::sort(headers_sort_list->begin(), headers_sort_list->end(),
              HeadersSortListCompare(headers_list));
  }

  MOZ_ASSERT(headers_sort_list->size() == headers_list->size());
}

// Clear the sort list, marking it as mutated so it will be recomputed on the next lookup.
void mark_for_sort(JS::HandleObject self) {
  MOZ_ASSERT(Headers::is_instance(self));
  std::vector<size_t> *headers_sort_list = static_cast<std::vector<size_t> *>(
      JS::GetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersSortList)).toPrivate());
  headers_sort_list->clear();
}

} // namespace

bool redecode_str_if_changed(JSContext *cx, HandleValue str_val, string_view chars, bool changed,
                             MutableHandleValue rval) {
  if (!changed) {
    rval.set(str_val);
    return true;
  }

  RootedString str(cx, core::decode(cx, chars));
  if (!str) {
    return false;
  }

  rval.setString(str);
  return true;
}

static bool switch_mode(JSContext *cx, HandleObject self, const Headers::Mode mode) {
  auto current_mode = Headers::mode(self);
  if (mode == current_mode) {
    return true;
  }

  if (current_mode == Headers::Mode::Uninitialized) {
    MOZ_ASSERT(mode == Headers::Mode::ContentOnly);
    RootedObject map(cx, JS::NewMapObject(cx));
    if (!map) {
      return false;
    }
    MOZ_ASSERT(static_cast<Headers::HeadersList *>(
                   JS::GetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersList))
                       .toPrivate()) == nullptr);
    SetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersList),
                    JS::PrivateValue(new Headers::HeadersList()));
    MOZ_ASSERT(static_cast<std::vector<size_t> *>(
                   JS::GetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersSortList))
                       .toPrivate()) == nullptr);
    SetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersSortList),
                    JS::PrivateValue(new std::vector<size_t>()));
    SetReservedSlot(self, static_cast<size_t>(Headers::Slots::Mode),
                    JS::Int32Value(static_cast<int32_t>(Headers::Mode::ContentOnly)));
    return true;
  }

  if (current_mode == Headers::Mode::ContentOnly) {
    MOZ_ASSERT(mode == Headers::Mode::CachedInContent,
               "Switching from ContentOnly to HostOnly is wasteful and not implemented");
    Headers::HeadersList *list = Headers::get_list(cx, self);
    MOZ_ASSERT(list);

    auto handle = host_api::HttpHeaders::FromEntries(*list);
    if (handle.is_err()) {
      return api::throw_error(cx, FetchErrors::HeadersCloningFailed);
    }
    SetReservedSlot(self, static_cast<size_t>(Headers::Slots::Handle),
                    PrivateValue(handle.unwrap()));
  }

  // Regardless of whether we're switching to CachedInContent or ContentOnly,
  // get all entries into content.
  if (current_mode == Headers::Mode::HostOnly) {
    auto *list = static_cast<Headers::HeadersList *>(
        JS::GetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersList)).toPrivate());

    auto handle = get_handle(self);
    MOZ_ASSERT(handle);
    auto res = handle->entries();
    if (res.is_err()) {
      HANDLE_ERROR(cx, *res.to_err());
      return false;
    }

    for (auto &entry : std::move(res.unwrap())) {
      list->emplace_back(std::move(std::get<0>(entry)), std::move(std::get<1>(entry)));
    }
  }

  if (mode == Headers::Mode::ContentOnly) {
    auto handle = get_handle(self);
    delete handle;
    SetReservedSlot(self, static_cast<size_t>(Headers::Slots::Handle), PrivateValue(nullptr));
  }

  SetReservedSlot(self, static_cast<size_t>(Headers::Slots::Mode),
                  JS::Int32Value(static_cast<int32_t>(mode)));
  return true;
}

bool prepare_for_entries_modification(JSContext *cx, JS::HandleObject self) {
  auto mode = Headers::mode(self);
  if (mode == Headers::Mode::HostOnly) {
    auto handle = get_handle(self);
    if (!handle->is_writable()) {
      // TODO(headers): add guard removal to cloned handle
      auto new_handle = handle->clone();
      if (!new_handle) {
        return api::throw_error(cx, FetchErrors::HeadersCloningFailed);
      }
      delete handle;
      SetReservedSlot(self, static_cast<size_t>(Headers::Slots::Handle), PrivateValue(new_handle));
    }
  } else if (mode == Headers::Mode::CachedInContent || mode == Headers::Mode::Uninitialized) {
    return switch_mode(cx, self, Headers::Mode::ContentOnly);
  }
  return true;
}

bool append_valid_normalized_header_string(JSContext *cx, HandleObject self,
                                           string_view header_name, string_view header_val) {
  Headers::Mode mode = Headers::mode(self);
  if (mode == Headers::Mode::HostOnly) {
    auto handle = get_handle(self)->as_writable();
    MOZ_ASSERT(handle);
    auto res = handle->append(header_name, header_val);
    if (auto *err = res.to_err()) {
      HANDLE_ERROR(cx, *err);
      return false;
    }
  } else {
    MOZ_ASSERT(mode == Headers::Mode::ContentOnly);
    if (!Headers::check_guard(cx, self, header_name)) {
      return true;
    }

    Headers::HeadersList *list = Headers::get_list(cx, self);
    MOZ_ASSERT(list);

    list->emplace_back(host_api::HostString::from_copy(header_name),
                       host_api::HostString::from_copy(header_val));
    // add the new index to the sort list for sorting
    mark_for_sort(self);
  }

  return true;
}

bool Headers::append_header_value(JSContext *cx, JS::HandleObject self, JS::HandleValue name,
                                  JS::HandleValue value, const char *fun_name) {
  NORMALIZE_NAME(name, fun_name)
  NORMALIZE_VALUE(value, fun_name)

  if (!prepare_for_entries_modification(cx, self)) {
    return false;
  }

  // TODO(headers): These must be normalizing encodes
  // TODO(headers): name must come from existing name match if there is one
  host_api::HostString name_str = core::encode(cx, name);
  if (!name_str.ptr) {
    // TODO(headers): error handling
    return false;
  }
  host_api::HostString value_str = core::encode(cx, value);
  if (!name_str.ptr) {
    // TODO(headers): error handling
    return false;
  }

  // TODO:
  // if its an existing header, use the existing name casing
  // auto idx = lookup(cx, self, header_name);
  // if (idx) {
  //   header_name = std::get<0>(*Headers::get_index(cx, self, idx.value()));
  // }

  if (!append_valid_normalized_header_string(cx, self, name_str, value_str)) {
    return false;
  }

  return true;
}

JSObject *Headers::create(JSContext *cx, HeadersGuard guard) {
  JSObject *self = JS_NewObjectWithGivenProto(cx, &class_, proto_obj);
  if (!self) {
    return nullptr;
  }

  SetReservedSlot(self, static_cast<uint32_t>(Slots::Guard),
                  JS::Int32Value(static_cast<int32_t>(guard)));
  SetReservedSlot(self, static_cast<uint32_t>(Slots::Mode),
                  JS::Int32Value(static_cast<int32_t>(Mode::Uninitialized)));

  SetReservedSlot(self, static_cast<uint32_t>(Slots::HeadersList), JS::PrivateValue(nullptr));
  SetReservedSlot(self, static_cast<uint32_t>(Slots::HeadersSortList), JS::PrivateValue(nullptr));

  return self;
}

JSObject *Headers::create(JSContext *cx, host_api::HttpHeadersReadOnly *handle,
                          HeadersGuard guard) {
  RootedObject self(cx, create(cx, guard));
  if (!self) {
    return nullptr;
  }

  MOZ_ASSERT(Headers::mode(self) == Headers::Mode::Uninitialized);
  SetReservedSlot(self, static_cast<uint32_t>(Headers::Slots::Mode),
                  JS::Int32Value(static_cast<int32_t>(Headers::Mode::HostOnly)));
  SetReservedSlot(self, static_cast<uint32_t>(Headers::Slots::Handle), PrivateValue(handle));
  return self;
}

JSObject *Headers::create(JSContext *cx, HandleValue init_headers, HeadersGuard guard) {
  RootedObject self(cx, create(cx, guard));
  if (!self) {
    return nullptr;
  }
  if (!init_entries(cx, self, init_headers)) {
    return nullptr;
  }
  MOZ_ASSERT(mode(self) == Headers::Mode::ContentOnly ||
             mode(self) == Headers::Mode::Uninitialized);
  return self;
}

bool Headers::init_entries(JSContext *cx, HandleObject self, HandleValue initv) {
  // TODO: check if initv is a Headers instance and clone its handle if so.
  // TODO: But note: forbidden headers have to be applied correctly.
  bool consumed = false;
  if (!core::maybe_consume_sequence_or_record<append_header_value>(cx, initv, self, &consumed,
                                                                   "Headers")) {
    return false;
  }

  if (!consumed) {
    api::throw_error(cx, api::Errors::InvalidSequence, "Headers", "");
    return false;
  }

  return true;
}

bool Headers::get(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(1)

  NORMALIZE_NAME(args[0], "Headers.get")

  Mode mode = Headers::mode(self);
  if (mode == Headers::Mode::Uninitialized) {
    args.rval().setNull();
    return true;
  }

  if (mode == Mode::HostOnly) {
    return retrieve_value_for_header_from_handle(cx, self, name_chars, args.rval());
  }

  auto idx = Headers::lookup(cx, self, name_chars);
  if (!idx) {
    args.rval().setNull();
    return true;
  }

  args.rval().setString(get_combined_value(cx, self, &idx.value()));

  return true;
}

bool Headers::set(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(2)

  NORMALIZE_NAME(args[0], "Headers.set")
  NORMALIZE_VALUE(args[1], "Headers.set")

  if (!prepare_for_entries_modification(cx, self)) {
    return false;
  }

  Mode mode = Headers::mode(self);
  if (mode == Mode::HostOnly) {
    auto handle = get_handle(self)->as_writable();
    MOZ_ASSERT(handle);
    auto res = handle->set(name_chars, value_chars);
    if (auto *err = res.to_err()) {
      HANDLE_ERROR(cx, *err);
      return false;
    }
  } else {
    MOZ_ASSERT(mode == Mode::ContentOnly);
    MOZ_ASSERT_UNREACHABLE("TODO HEADERS");
    // TODO(headers)
    // RootedObject entries(cx, get_list(cx, self));
    // if (!entries) {
    //   return false;
    // }

    // RootedValue name_val(cx);
    // if (!redecode_str_if_changed(cx, args[0], name_chars, name_changed, &name_val)) {
    //   return false;
    // }

    // RootedValue value_val(cx);
    // if (!redecode_str_if_changed(cx, args[1], value_chars, value_changed, &value_val)) {
    //   return false;
    // }

    // if (!MapSet(cx, entries, name_val, value_val)) {
    //   return false;
    // }
  }

  args.rval().setUndefined();
  return true;
}

bool Headers::has(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(1)

  NORMALIZE_NAME(args[0], "Headers.has")

  Mode mode = Headers::mode(self);
  if (mode == Mode::Uninitialized) {
    args.rval().setBoolean(false);
    return true;
  }

  if (mode == Mode::HostOnly) {
    auto handle = get_handle(self);
    MOZ_ASSERT(handle);
    auto res = handle->has(name_chars);
    MOZ_ASSERT(!res.is_err());
    args.rval().setBoolean(res.unwrap());
  } else {
    args.rval().setBoolean(Headers::lookup(cx, self, name_chars).has_value());
  }

  return true;
}

bool Headers::append(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(2)

  if (!append_header_value(cx, self, args[0], args[1], "Headers.append")) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

bool Headers::set_valid_if_undefined(JSContext *cx, HandleObject self, string_view name,
                                     string_view value) {
  if (!prepare_for_entries_modification(cx, self)) {
    return false;
  }

  if (mode(self) == Mode::HostOnly) {
    auto handle = get_handle(self)->as_writable();
    auto has = handle->has(name);
    MOZ_ASSERT(!has.is_err());
    if (has.unwrap()) {
      return true;
    }

    auto res = handle->append(name, value);
    if (auto *err = res.to_err()) {
      HANDLE_ERROR(cx, *err);
      return false;
    }
    return true;
  }

  MOZ_ASSERT(mode(self) == Mode::ContentOnly);

  if (Headers::lookup(cx, self, name)) {
    fprintf(stderr, "WE ALREADY HAVE %s", name.data());
    return true;
  }

  return append_valid_normalized_header_string(cx, self, name, value);
}

bool Headers::delete_(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER_WITH_NAME(1, "delete")

  if (!prepare_for_entries_modification(cx, self)) {
    return false;
  }

  NORMALIZE_NAME(args[0], "Headers.delete")
  Mode mode = Headers::mode(self);
  if (mode == Mode::HostOnly) {
    auto handle = get_handle(self)->as_writable();
    MOZ_ASSERT(handle);
    std::string_view name = name_chars;
    auto res = handle->remove(name);
    if (auto *err = res.to_err()) {
      HANDLE_ERROR(cx, *err);
      return false;
    }
  } else {
    MOZ_ASSERT(mode == Mode::ContentOnly);

    auto idx = Headers::lookup(cx, self, name_chars);
    if (idx) {
      size_t index = idx.value();
      // The lookup above will guarantee that sort_list is up to date.
      std::vector<size_t> *headers_sort_list = static_cast<std::vector<size_t> *>(
          JS::GetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersSortList))
              .toPrivate());
      MOZ_ASSERT(headers_sort_list);

      HeadersList *headers_list = static_cast<Headers::HeadersList *>(
          JS::GetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersList)).toPrivate());
      MOZ_ASSERT(headers_list);

      // Delete all case-insensitively equal names.
      // Each deletion will naturally shift the next sorted same name into index, with the
      // deletions retaining the integrity of both headers_list and headers_sort_list.
      do {
        headers_list->erase(headers_list->begin() + headers_sort_list->at(index));
        headers_sort_list->erase(headers_sort_list->begin() + index);
      } while (header_compare(std::get<0>(headers_list->at(headers_sort_list->at(index))),
                              name_chars) == Ordering::Equal);
    }
  }

  args.rval().setUndefined();
  return true;
}

const JSFunctionSpec Headers::static_methods[] = {
    JS_FS_END,
};

const JSPropertySpec Headers::static_properties[] = {
    JS_PS_END,
};

const JSFunctionSpec Headers::methods[] = {
    JS_FN("get", Headers::get, 1, JSPROP_ENUMERATE),
    JS_FN("has", Headers::has, 1, JSPROP_ENUMERATE),
    JS_FN("set", Headers::set, 2, JSPROP_ENUMERATE),
    JS_FN("append", Headers::append, 2, JSPROP_ENUMERATE),
    JS_FN("delete", Headers::delete_, 1, JSPROP_ENUMERATE),
    JS_FN("forEach", Headers::forEach, 1, JSPROP_ENUMERATE),
    JS_FN("entries", Headers::entries, 0, JSPROP_ENUMERATE),
    JS_FN("keys", Headers::keys, 0, JSPROP_ENUMERATE),
    JS_FN("values", Headers::values, 0, JSPROP_ENUMERATE),
    // [Symbol.iterator] added in init_class.
    JS_FS_END,
};

const JSPropertySpec Headers::properties[] = {
    JS_PS_END,
};

bool Headers::constructor(JSContext *cx, unsigned argc, JS::Value *vp) {
  CTOR_HEADER("Headers", 0);
  HandleValue headersInit = args.get(0);
  RootedObject self(cx, JS_NewObjectForConstructor(cx, &class_, args));
  if (!self) {
    return false;
  }
  SetReservedSlot(self, static_cast<uint32_t>(Slots::Guard),
                  JS::Int32Value(static_cast<int32_t>(HeadersGuard::None)));
  if (!init_entries(cx, self, headersInit)) {
    return false;
  }

  SetReservedSlot(self, static_cast<uint32_t>(Slots::HeadersList), JS::PrivateValue(nullptr));
  SetReservedSlot(self, static_cast<uint32_t>(Slots::HeadersSortList), JS::PrivateValue(nullptr));

  args.rval().setObject(*self);
  return true;
}

bool Headers::init_class(JSContext *cx, JS::HandleObject global) {
  // get the host forbidden headers for guard checks
  forbidden_request_headers = host_api::HttpHeaders::get_forbidden_request_headers();
  forbidden_response_headers = host_api::HttpHeaders::get_forbidden_response_headers();

  // sort the forbidden headers with the lowercase-invariant comparator
  std::sort(forbidden_request_headers.begin(), forbidden_request_headers.end(), HeaderCompare());
  std::sort(forbidden_response_headers.begin(), forbidden_response_headers.end(), HeaderCompare());

  if (!init_class_impl(cx, global)) {
    return false;
  }

  auto comma_str = JS_NewStringCopyN(cx, ", ", 2);
  if (!comma_str) {
    return false;
  }
  comma.init(cx, comma_str);

  if (!HeadersIterator::init_class(cx, global)) {
    return false;
  }

  JS::RootedValue entries(cx);
  if (!JS_GetProperty(cx, proto_obj, "entries", &entries))
    return false;

  JS::SymbolCode code = JS::SymbolCode::iterator;
  JS::RootedId iteratorId(cx, JS::GetWellKnownSymbolKey(cx, code));
  return JS_DefinePropertyById(cx, proto_obj, iteratorId, entries, 0);
}

Headers::HeadersList *Headers::get_list(JSContext *cx, HandleObject self) {
  MOZ_ASSERT(is_instance(self));
  if (mode(self) == Mode::Uninitialized && !switch_mode(cx, self, Mode::ContentOnly)) {
    return nullptr;
  }
  if (mode(self) == Mode::HostOnly && !switch_mode(cx, self, Mode::CachedInContent)) {
    return nullptr;
  }

  return static_cast<HeadersList *>(
      GetReservedSlot(self, static_cast<size_t>(Slots::HeadersList)).toPrivate());
}

unique_ptr<host_api::HttpHeaders> Headers::handle_clone(JSContext *cx, HandleObject self) {
  auto mode = Headers::mode(self);

  // If this instance uninitialized, return an empty handle without initializing this instance.
  if (mode == Mode::Uninitialized) {
    return std::make_unique<host_api::HttpHeaders>();
  }

  if (mode == Mode::ContentOnly && !switch_mode(cx, self, Mode::CachedInContent)) {
    // Switch to Mode::CachedInContent to ensure that the latest data is available on the handle,
    // but without discarding the existing entries, in case content reads them later.
    return nullptr;
  }

  auto handle = unique_ptr<host_api::HttpHeaders>(get_handle(self)->clone());
  if (!handle) {
    api::throw_error(cx, FetchErrors::HeadersCloningFailed);
    return nullptr;
  }
  return handle;
}

bool Headers::check_guard(JSContext *cx, HandleObject self, string_view name) {
  // TODO(headers): guard filter
  //   std::vector<const char*>* forbidden_headers = nullptr;
  //   switch (guard) {
  //   case HeadersGuard::None:
  //     return true;
  //   case HeadersGuard::Request:
  //     forbidden_headers = &forbidden_request_headers;
  //     break;
  //   case HeadersGuard::Response:
  //     forbidden_headers = &forbidden_response_headers;
  //     break;
  //   default:
  //     MOZ_ASSERT_UNREACHABLE();
  //   }

  //   if (!forbidden_headers) {
  //     return true;
  //   }

  //   for (auto header : *forbidden_headers) {
  //     if (header_name.compare(header) == 0) {
  //       return false;
  //     }
  //   }

  //   return true;
  return true;
}

void Headers::guard_filter(host_api::HttpHeaders &handle, HeadersGuard guard) {}

void Headers::guard_filter(JSContext *cx, HandleObject self, HeadersGuard guard) {
  // TODO(headers): implement guard filter against appropriate forbidden headers list
  //   std::vector<const char*>* forbidden_headers = nullptr;
  //   switch (guard) {
  //   case HeadersGuard::Request:
  //     forbidden_headers = &forbidden_request_headers;
  //     break;
  //   case HeadersGuard::Response:
  //     break;
  //   case HeadersGuard::None:
  //     break;
  //   default:
  //     MOZ_ASSERT_UNREACHABLE();
  //   }

  //   if (!forbidden_headers) {
  //     return headers;
  //   }

  //   for (auto header : *forbidden_headers) {
  //     if (headers->has(header).unwrap()) {
  //       headers->remove(header).unwrap();
  //     }
  //   }
}

BUILTIN_ITERATOR_METHODS(Headers)

// Headers Iterator
JSObject *HeadersIterator::create(JSContext *cx, HandleObject headers, uint8_t type) {
  JSObject *self = JS_NewObjectWithGivenProto(cx, &class_, proto_obj);
  if (!self) {
    return nullptr;
  }
  SetReservedSlot(self, static_cast<uint32_t>(Slots::Type),
                  JS::Int32Value(static_cast<int32_t>(type)));
  SetReservedSlot(self, static_cast<uint32_t>(Slots::Cursor), JS::Int32Value(0));
  SetReservedSlot(self, static_cast<uint32_t>(Slots::Headers), JS::ObjectValue(*headers));
  return self;
}

const JSFunctionSpec HeadersIterator::static_methods[] = {
    JS_FS_END,
};

const JSPropertySpec HeadersIterator::static_properties[] = {
    JS_PS_END,
};

const JSFunctionSpec HeadersIterator::methods[] = {
    JS_FN("next", HeadersIterator::next, 0, JSPROP_ENUMERATE),
    JS_FS_END,
};

const JSPropertySpec HeadersIterator::properties[] = {
    JS_PS_END,
};

bool HeadersIterator::init_class(JSContext *cx, JS::HandleObject global) {
  JS::RootedObject iterator_proto(cx, JS::GetRealmIteratorPrototype(cx));
  if (!iterator_proto)
    return false;

  if (!init_class_impl(cx, global, iterator_proto))
    return false;

  // Delete both the `HeadersIterator` global property and the
  // `constructor` property on `HeadersIterator.prototype`. The latter
  // because Iterators don't have their own constructor on the prototype.
  return JS_DeleteProperty(cx, global, class_.name) &&
         JS_DeleteProperty(cx, proto_obj, "constructor");
}

JS::HandleString Headers::get_combined_value(JSContext *cx, JS::HandleObject self, size_t *index) {
  MOZ_ASSERT(is_instance(self));
  HeadersList *headers_list = static_cast<Headers::HeadersList *>(
      JS::GetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersList)).toPrivate());
  MOZ_ASSERT(headers_list);
  const host_api::HostString *val = &std::get<1>(*Headers::get_index(cx, self, *index));
  // check if we need to join with the next value if it is the same key, comma-separated
  JS::RootedString str(cx,
                       JS_NewStringCopyN(cx, reinterpret_cast<char *>(val->ptr.get()), val->len));
  while (*index + 1 < headers_list->size()) {
    const host_api::HostString *next_val = &std::get<1>(*Headers::get_index(cx, self, *index + 1));
    if (header_compare(*next_val, *val) != Ordering::Equal) {
      break;
    }
    // unless it is set-cookie, in which case we don't
    if (header_compare(*val, "set-cookie") == Ordering::Equal) {
      break;
    }
    str = JS_ConcatStrings(cx, str, comma);
    JS::RootedString next_str(
        cx, JS_NewStringCopyN(cx, reinterpret_cast<char *>(next_val->ptr.get()), next_val->len));
    str = JS_ConcatStrings(cx, str, next_str);
    *index = *index + 1;
  }
  return str;
}

const std::tuple<host_api::HostString, host_api::HostString> *
Headers::get_index(JSContext *cx, JS::HandleObject self, size_t index) {
  MOZ_ASSERT(is_instance(self));
  std::vector<size_t> *headers_sort_list = static_cast<std::vector<size_t> *>(
      JS::GetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersSortList)).toPrivate());
  MOZ_ASSERT(headers_sort_list);

  const HeadersList *headers_list = static_cast<Headers::HeadersList *>(
      JS::GetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersList)).toPrivate());
  MOZ_ASSERT(headers_list);

  ensure_updated_sort_list(headers_list, headers_sort_list);
  MOZ_RELEASE_ASSERT(index < headers_sort_list->size());

  return &headers_list->at(headers_sort_list->at(index));
}

std::optional<size_t> Headers::lookup(JSContext *cx, HandleObject self, string_view key) {
  MOZ_ASSERT(is_instance(self));
  const HeadersList *headers_list = static_cast<Headers::HeadersList *>(
      JS::GetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersList)).toPrivate());
  MOZ_ASSERT(headers_list);
  std::vector<size_t> *headers_sort_list = static_cast<std::vector<size_t> *>(
      JS::GetReservedSlot(self, static_cast<size_t>(Headers::Slots::HeadersSortList)).toPrivate());
  MOZ_ASSERT(headers_sort_list);

  ensure_updated_sort_list(headers_list, headers_sort_list);

  // Now we know its sorted, we can binary search.
  auto it = std::lower_bound(headers_sort_list->begin(), headers_sort_list->end(), key,
                             HeadersSortListLookupCompare(headers_list));
  if (it == headers_sort_list->end() ||
      header_compare(std::get<0>(headers_list->at(*it)), key) != Ordering::Equal) {
    return std::nullopt;
  }
  return it - headers_sort_list->begin();
}

bool HeadersIterator::next(JSContext *cx, unsigned argc, Value *vp) {
  METHOD_HEADER(0)
  JS::RootedObject headers(cx, &JS::GetReservedSlot(self, Slots::Headers).toObject());
  size_t index = JS::GetReservedSlot(self, Slots::Cursor).toInt32();
  uint8_t type = static_cast<uint8_t>(JS::GetReservedSlot(self, Slots::Type).toInt32());

  JS::RootedObject result(cx, JS_NewPlainObject(cx));
  if (!result)
    return false;

  Headers::HeadersList *list = Headers::get_list(cx, headers);
  MOZ_ASSERT(list);
  if (index >= list->size()) {
    JS_DefineProperty(cx, result, "done", true, JSPROP_ENUMERATE);
    JS_DefineProperty(cx, result, "value", JS::UndefinedHandleValue, JSPROP_ENUMERATE);

    args.rval().setObject(*result);
    return true;
  }

  JS_DefineProperty(cx, result, "done", false, JSPROP_ENUMERATE);

  JS::RootedValue key_val(cx);
  JS::RootedValue val_val(cx);

  if (type != ITER_TYPE_VALUES) {
    const host_api::HostString *key = &std::get<0>(*Headers::get_index(cx, headers, index));
    char16_t *chars = reinterpret_cast<char16_t *>(malloc(key->len * 2));
    for (int i = 0; i < key->len; ++i) {
      const unsigned char ch = key->ptr[i];
      // headers should already be validated by here
      MOZ_ASSERT(ch <= 127 && VALID_NAME_CHARS[ch]);
      if (ch >= 'A' && ch <= 'Z') {
        chars[i] = ch - 'A' + 'a';
      } else {
        chars[i] = ch;
      }
    }
    JS::RootedString str(cx, JS_NewUCString(cx, JS::UniqueTwoByteChars{chars}, key->len));
    if (!str) {
      free(chars);
      return false;
    }
    key_val = JS::StringValue(str);
  }

  if (type != ITER_TYPE_KEYS) {
    size_t start_index = index;
    JS::RootedString str(cx, Headers::get_combined_value(cx, headers, &index));
    // combining can alter the cursor for multi-entry cases
    if (index != start_index) {
      JS::SetReservedSlot(self, Slots::Cursor, JS::Int32Value(index));
    }
    if (!str)
      return false;
    val_val = JS::StringValue(str);
  }

  JS::RootedValue result_val(cx);

  switch (type) {
  case ITER_TYPE_ENTRIES: {
    JS::RootedObject pair(cx, JS::NewArrayObject(cx, 2));
    if (!pair)
      return false;
    JS_DefineElement(cx, pair, 0, key_val, JSPROP_ENUMERATE);
    JS_DefineElement(cx, pair, 1, val_val, JSPROP_ENUMERATE);
    result_val = JS::ObjectValue(*pair);
    break;
  }
  case ITER_TYPE_KEYS: {
    result_val = key_val;
    break;
  }
  case ITER_TYPE_VALUES: {
    result_val = val_val;
    break;
  }
  default:
    MOZ_RELEASE_ASSERT(false, "Invalid iter type");
  }

  JS_DefineProperty(cx, result, "value", result_val, JSPROP_ENUMERATE);

  JS::SetReservedSlot(self, Slots::Cursor, JS::Int32Value(index + 1));
  args.rval().setObject(*result);
  return true;
}

} // namespace builtins::web::fetch
