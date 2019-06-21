#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#if defined(__LP64__)
  // We are on 64-bit already => No need to emulate anything special
  #define timegm(x) timegm64(x)
#else
  // Loading time64.h fails with #error on 64-bit => Only load it on 32-bit
  #include <time64.h>

  #define CHAR_BIT 8
  // From http://code.metager.de/source/xref/chromium/base/os_compat_android.cc
  // 32-bit Android has only timegm64() and not timegm().
  // We replicate the behaviour of timegm() when the result overflows time_t.
  time_t timegm(struct tm* const t) {
    // time_t is signed on Android.
    static const time_t kTimeMax = ~(1L << (sizeof(time_t) * CHAR_BIT - 1));
    static const time_t kTimeMin = (1L << (sizeof(time_t) * CHAR_BIT - 1));
    time64_t result = timegm64(t);
    if (result < kTimeMin || result > kTimeMax)
      return -1;
    return result;
  }
#endif

// https://android.googlesource.com/platform/external/elfutils/+/android-4.3.1_r1/bionic-fixup/getline.c
ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
  char *ptr;
  ptr = fgetln(stream, n);
  if (ptr == NULL) {
    return -1;
  }
  /* Free the original ptr */
  if (*lineptr != NULL) free(*lineptr);
  /* Add one more space for '\0' */
  size_t len = n[0] + 1;
  /* Update the length */
  n[0] = len;
  /* Allocate a new buffer */
  *lineptr = malloc(len);
  /* Copy over the string */
  memcpy(*lineptr, ptr, len-1);
  /* Write the NULL character */
  (*lineptr)[len-1] = '\0';
  /* Return the length of the new buffer */
  return len;
}
