// Decodes a nano::Binary log to text on stdout: nanolog_decode FILE

#include "nanologger_binary.hpp"

#include <cstdio>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s FILE\n", argv[0]);
    return 2;
  }
  std::FILE* in = std::fopen(argv[1], "rb");
  if (!in) {
    std::perror(argv[1]);
    return 1;
  }
  const bool ok = nano::decode_binary(in, stdout);
  std::fclose(in);
  if (!ok) {
    std::fprintf(stderr, "%s: malformed or truncated input\n", argv[1]);
    return 1;
  }
}
