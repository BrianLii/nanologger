#pragma once

// Binary records: the consumer writes each record's argument bytes instead of
// formatting text, and decode_binary (the nanolog_decode tool) turns the file
// back into text later.
//
// Use nano::Binary<"printf format"> as the Formatter of try_log. The format is
// checked against the argument types at compile time. A consumer thread
// defines a site in its output just before that site's first record.
//
// Stream layout, native byte order:
//   definition: u32 0, u32 id, u32 format length, format bytes,
//               u8 argument count, then per argument u8 kind and u32 size
//   record:     u32 id (1, 2, ... in definition order), u64 ticks,
//               then each argument's bytes
// Kinds: 'u' unsigned integer, 'i' signed integer, 'f' float or double,
// 's' std::array<char, N>.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace nano {

static_assert(std::endian::native == std::endian::little);

template <std::size_t N>
struct FormatString {
  char text[N]{};
  consteval FormatString(const char (&s)[N]) {
    for (std::size_t i = 0; i < N; ++i) text[i] = s[i];
  }
  constexpr std::string_view view() const { return {text, N - 1}; }
};

namespace detail {

inline constexpr std::size_t max_decode_bytes = 1 << 16;

struct ArgType {
  char kind;
  std::uint32_t size;
};

template <typename T>
struct is_char_array : std::false_type {};
template <std::size_t N>
struct is_char_array<std::array<char, N>> : std::true_type {};

template <typename T>
consteval ArgType arg_type() {
  if constexpr (std::is_integral_v<T>)
    return {std::is_signed_v<T> ? 'i' : 'u', sizeof(T)};
  else if constexpr (std::is_floating_point_v<T> && sizeof(T) <= 8)
    return {'f', sizeof(T)};
  else if constexpr (is_char_array<T>::value)
    return {'s', sizeof(T)};
  else
    static_assert(sizeof(T) == 0,
                  "Binary arguments must be integers, float, double, or "
                  "std::array<char, N>");
}

// One printf conversion specification. `begin` is npos when the format has no
// further conversion; `type` is 0 when the specification is malformed.
struct Conversion {
  std::size_t begin = std::string_view::npos;
  std::size_t end = 0;
  char length = 0;  // 0, 'h', 'H' (hh), 'l', 'L' (ll), 'j', or 'z'
  char type = 0;
};

constexpr Conversion next_conversion(std::string_view f, std::size_t pos) {
  pos = f.find('%', pos);
  while (pos != std::string_view::npos && pos + 1 < f.size() &&
         f[pos + 1] == '%')
    pos = f.find('%', pos + 2);
  if (pos == std::string_view::npos) return {};

  Conversion c{.begin = pos};
  std::size_t i = pos + 1;
  const auto skip = [&](std::string_view chars) {
    while (i < f.size() && chars.find(f[i]) != std::string_view::npos) ++i;
  };
  skip("-+ #0");
  skip("0123456789");
  if (i < f.size() && f[i] == '.') {
    ++i;
    skip("0123456789");
  }
  if (i < f.size() && std::string_view("hljz").find(f[i]) != std::string_view::npos) {
    c.length = f[i++];
    if (i < f.size() && f[i] == c.length && (c.length == 'h' || c.length == 'l')) {
      c.length = c.length == 'h' ? 'H' : 'L';
      ++i;
    }
  }
  if (i < f.size()) c.type = f[i++];
  c.end = i;
  return c;
}

constexpr std::size_t int_width(char length) {
  switch (length) {
    case 0:
    case 'h':
    case 'H': return sizeof(int);
    case 'l': return sizeof(long);
    case 'L': return sizeof(long long);
    case 'j': return sizeof(std::intmax_t);
    case 'z': return sizeof(std::size_t);
  }
  return 0;
}

constexpr bool accepts(const Conversion& c, ArgType a) {
  const auto is = [&c](std::string_view types) {
    return types.find(c.type) != std::string_view::npos;
  };
  switch (a.kind) {
    case 'u':
    case 'i':
      return is("diouxXc") && std::has_single_bit(a.size) &&
             a.size <= int_width(c.length);
    case 'f':
      return is("fFeEgGaA") && c.length == 0 && (a.size == 4 || a.size == 8);
    case 's':
      return c.type == 's' && c.length == 0 && a.size <= max_decode_bytes;
  }
  return false;
}

// True when `format` has exactly one supported conversion per argument, in
// order. The decoder relies on this before handing a format to printf.
constexpr bool format_matches(std::string_view format,
                              std::span<const ArgType> args) {
  std::size_t pos = 0;
  for (const ArgType& a : args) {
    const Conversion c = next_conversion(format, pos);
    if (c.begin == std::string_view::npos || !accepts(c, a)) return false;
    pos = c.end;
  }
  return next_conversion(format, pos).begin == std::string_view::npos;
}

inline thread_local std::uint32_t site_count = 0;

template <typename T>
void put(std::FILE* out, const T& value) noexcept {
  std::fwrite(&value, sizeof value, 1, out);
}

inline std::uint32_t define_site(std::FILE* out, std::string_view format,
                                 std::span<const ArgType> args) noexcept {
  const std::uint32_t id = ++site_count;
  put(out, std::uint32_t{0});
  put(out, id);
  put(out, static_cast<std::uint32_t>(format.size()));
  std::fwrite(format.data(), 1, format.size(), out);
  put(out, static_cast<std::uint8_t>(args.size()));
  for (const ArgType& a : args) {
    put(out, a.kind);
    put(out, a.size);
  }
  return id;
}

}  // namespace detail

template <FormatString Format>
struct Binary {
  template <typename... Args>
  static void write(std::FILE* out, std::uint64_t ticks,
                    const Args&... args) noexcept {
    static constexpr std::array<detail::ArgType, sizeof...(Args)> types{
        detail::arg_type<Args>()...};
    static_assert(sizeof...(Args) <= 255 &&
                      detail::format_matches(Format.view(), types),
                  "format does not match the argument types");
    // ponytail: sites are defined once per consumer thread, which assumes a
    // consumer thread writes to one output (true for AsyncLogger). Move this
    // into a sink object once the consumer stops writing through FILE*.
    thread_local const std::uint32_t id =
        detail::define_site(out, Format.view(), types);

    std::array<std::byte, sizeof id + sizeof ticks + (sizeof(Args) + ... + 0)>
        record;
    std::byte* p = record.data();
    const auto append = [&p](const auto& value) {
      std::memcpy(p, &value, sizeof value);
      p += sizeof value;
    };
    append(id);
    append(ticks);
    (append(args), ...);
    std::fwrite(record.data(), 1, record.size(), out);
  }
};

namespace detail {

struct Piece {
  std::string literal;  // text before the conversion, with %% unescaped
  std::string spec;     // the conversion specification alone, e.g. "%.4f"
  Conversion conversion;
  ArgType type;
};

struct Site {
  std::vector<Piece> pieces;
  std::string tail;
};

// Literal text between conversions only contains '%' as "%%".
inline std::string unescape(std::string_view text) {
  std::string s;
  for (std::size_t i = 0; i < text.size(); ++i) {
    s += text[i];
    if (text[i] == '%') ++i;
  }
  return s;
}

template <typename Signed>
void print_int(std::FILE* out, const char* spec, bool is_signed,
               std::uint64_t bits) {
  if (is_signed)
    std::fprintf(out, spec, static_cast<Signed>(bits));
  else
    std::fprintf(out, spec, static_cast<std::make_unsigned_t<Signed>>(bits));
}

inline bool print_arg(std::FILE* in, std::FILE* out, const Piece& p) {
  const char* spec = p.spec.c_str();
  const std::size_t size = p.type.size;
  if (p.type.kind == 's') {
    std::string text(size, '\0');
    if (std::fread(text.data(), 1, size, in) != size) return false;
    std::fprintf(out, spec, text.c_str());
    return true;
  }

  std::uint64_t bits = 0;
  if (std::fread(&bits, 1, size, in) != size) return false;
  if (p.type.kind == 'f') {
    if (size == sizeof(float)) {
      float value;
      std::memcpy(&value, &bits, sizeof value);
      std::fprintf(out, spec, static_cast<double>(value));
    } else {
      double value;
      std::memcpy(&value, &bits, sizeof value);
      std::fprintf(out, spec, value);
    }
    return true;
  }

  if (p.type.kind == 'i' && size < 8) {
    const auto shift = static_cast<int>(64 - 8 * size);
    bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(bits << shift) >> shift);
  }
  const char type = p.conversion.type;
  const bool is_signed = type == 'd' || type == 'i' || type == 'c';
  switch (p.conversion.length) {
    case 'l': print_int<long>(out, spec, is_signed, bits); break;
    case 'L': print_int<long long>(out, spec, is_signed, bits); break;
    case 'j': print_int<std::intmax_t>(out, spec, is_signed, bits); break;
    case 'z': print_int<std::make_signed_t<std::size_t>>(out, spec, is_signed, bits); break;
    default: print_int<int>(out, spec, is_signed, bits); break;
  }
  return true;
}

inline bool read_site(std::FILE* in, std::vector<Site>& sites) {
  const auto read = [in](void* data, std::size_t size) {
    return std::fread(data, 1, size, in) == size;
  };
  std::uint32_t id = 0;
  std::uint32_t length = 0;
  std::uint8_t count = 0;
  if (!read(&id, sizeof id) || id != sites.size() + 1 ||
      !read(&length, sizeof length) || length > max_decode_bytes)
    return false;
  std::string format(length, '\0');
  if (!read(format.data(), length) || !read(&count, sizeof count)) return false;
  std::vector<ArgType> types(count);
  for (ArgType& t : types)
    if (!read(&t.kind, sizeof t.kind) || !read(&t.size, sizeof t.size))
      return false;
  if (!format_matches(format, types)) return false;

  Site site;
  std::size_t pos = 0;
  for (const ArgType& t : types) {
    const Conversion c = next_conversion(format, pos);
    site.pieces.push_back({unescape(std::string_view(format).substr(pos, c.begin - pos)),
                           format.substr(c.begin, c.end - c.begin), c, t});
    pos = c.end;
  }
  site.tail = unescape(std::string_view(format).substr(pos));
  sites.push_back(std::move(site));
  return true;
}

}  // namespace detail

// Writes one line per record: the raw ticks, a space, then the formatted
// arguments. Returns false on malformed or truncated input.
inline bool decode_binary(std::FILE* in, std::FILE* out) {
  std::vector<detail::Site> sites;
  for (;;) {
    std::uint32_t id = 0;
    const std::size_t got = std::fread(&id, 1, sizeof id, in);
    if (got == 0 && std::feof(in)) return true;
    if (got != sizeof id) return false;
    if (id == 0) {
      if (!detail::read_site(in, sites)) return false;
      continue;
    }
    if (id > sites.size()) return false;

    std::uint64_t ticks = 0;
    if (std::fread(&ticks, 1, sizeof ticks, in) != sizeof ticks) return false;
    std::fprintf(out, "%llu ", static_cast<unsigned long long>(ticks));
    const detail::Site& site = sites[id - 1];
    for (const detail::Piece& p : site.pieces) {
      std::fwrite(p.literal.data(), 1, p.literal.size(), out);
      if (!detail::print_arg(in, out, p)) return false;
    }
    std::fwrite(site.tail.data(), 1, site.tail.size(), out);
    std::fputc('\n', out);
  }
}

}  // namespace nano
