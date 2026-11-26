#ifndef MOLKIE_ATTR_H
#define MOLKIE_ATTR_H

/// Compile-time printf format checking for wrappers.
/// PRINTF_LIKE(fmt_idx, args_idx): 1-based parameter positions.
/// For `va_list` variants pass 0 as args_idx.
#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(fmt_idx, args_idx)                                         \
  __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define PRINTF_LIKE(fmt_idx, args_idx)
#endif

#endif // MOLKIE_ATTR_H
