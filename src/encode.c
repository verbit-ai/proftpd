/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 2006-2008 The ProFTPD Project team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, The ProFTPD Project team and other respective
 * copyright holders give permission to link this program with OpenSSL, and
 * distribute the resulting executable, without including the source code for
 * OpenSSL in the source distribution.
 */

/* UTF8/charset encoding/decoding
 * $Id: encode.c,v 1.3 2008-04-03 03:18:31 castaglia Exp $
 */

#include "conf.h"

#ifdef PR_USE_NLS

#ifdef HAVE_ICONV_H
# include <iconv.h>
#endif

#ifdef HAVE_LANGINFO_H
# include <langinfo.h>
#endif

#ifdef HAVE_ICONV_H
static iconv_t decode_conv = (iconv_t) -1;
static iconv_t encode_conv = (iconv_t) -1;

static const char *local_charset = NULL;
static const char *encoding = "UTF-8";
static int supports_telnet_iac = TRUE;

static const char *trace_channel = "encode";

static int str_convert(iconv_t conv, char *inbuf, size_t *inbuflen,
    char *outbuf, size_t *outbuflen) {
# ifdef HAVE_ICONV
  char *start = inbuf;

  while (inbuflen > 0) {
    size_t nconv;

    pr_signals_handle();

    nconv = iconv(conv, &inbuf, inbuflen, &outbuf, outbuflen);
    if (nconv == (size_t) -1) {
      if (errno == EINVAL) {
        memmove(start, inbuf, *inbuflen);
        continue;

      } else
        return -1;
    }

    break;
  }
  return 0;
# else
  errno = ENOSYS;
  return -1;
# endif /* HAVE_ICONV */
}
#endif /* !HAVE_ICONV_H */

static void set_supports_telnet_iac(const char *codeset) {

  /* The full list of character sets which use 0xFF could be obtained from
   * the libiconv sources; for now, this list should contain the most
   * commonly used character sets.
   */

  if (strcasecmp(codeset, "CP1251") == 0 ||
      strcasecmp(codeset, "CP866") == 0 ||
      strcasecmp(codeset, "ISO-8859-1") == 0 ||
      strcasecmp(codeset, "KOI8-R") == 0 ||
      strcasecmp(codeset, "WINDOWS-1251") == 0) {
    supports_telnet_iac = FALSE;
  }

  supports_telnet_iac = FALSE;
}

int encode_free(void) {
# ifdef HAVE_ICONV
  int res = 0;

  /* Close the iconv handles. */
  if (iconv_close(encode_conv) < 0)
    res = -1;

  if (iconv_close(decode_conv) < 0)
    res = -1;

  return res;
# else
  errno = ENOSYS;
  return -1;
# endif
}

int encode_init(void) {

  if (encoding == NULL) {
    pr_trace_msg(trace_channel, 3, "no encoding configured");
    return 0;
  }

  if (local_charset == NULL) {
#ifdef HAVE_NL_LANGINFO
    /* Look up the current charset.  If there's a problem, default to
     * UCS-2.
     */
    local_charset = nl_langinfo(CODESET);
    if (!local_charset) {
      local_charset = "C";
      pr_trace_msg(trace_channel, 1,
        "unable to determine locale, defaulting to 'C' for %s conversion",
        encoding);

    } else {
      pr_trace_msg(trace_channel, 1,
        "converting %s to local character set '%s'", encoding, local_charset);
    }
#else
    local_charset = "C";
    pr_trace_msg(trace_channel, 1,
      "nl_langinfo(3) not supported, defaulting to using 'C' for %s conversion",
      encoding);
#endif /* HAVE_NL_LANGINFO */
  } else {
    pr_trace_msg(trace_channel, 3,
      "using '%s' as local charset for %s conversion", local_charset, encoding);
  }

# ifdef HAVE_ICONV
  /* Get the iconv handles. */
  encode_conv = iconv_open(encoding, local_charset);
  if (encode_conv == (iconv_t) -1) {
    pr_trace_msg(trace_channel, 1, "error opening conversion handle from '%s' "
      "to '%s': %s", local_charset, encoding, strerror(errno));
    return -1;
  }
 
  decode_conv = iconv_open(local_charset, encoding);
  if (decode_conv == (iconv_t) -1) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 1, "error opening conversion handle from '%s' "
      "to '%s': %s", encoding, local_charset, strerror(errno));

    (void) iconv_close(encode_conv);
    encode_conv = (iconv_t) -1;

    errno = xerrno;
    return -1;
  }

  set_supports_telnet_iac(encoding);
  return 0;
# else
  errno = ENOSYS;
  return -1;
# endif /* HAVE_ICONV */
}

char *pr_decode_str(pool *p, const char *in, size_t inlen, size_t *outlen) {
#ifdef HAVE_ICONV_H
  size_t inbuflen, outbuflen;
  char *inbuf, outbuf[PR_TUNABLE_PATH_MAX*2], *res = NULL;

  if (!p || !in || !outlen) {
    errno = EINVAL;
    return NULL;
  }

  if (decode_conv == (iconv_t) -1) {
    errno = EPERM;
    return NULL;
  }

  inbuf = pcalloc(p, inlen);
  memcpy(inbuf, in, inlen);
  inbuflen = inlen;

  outbuflen = sizeof(outbuf);

  if (str_convert(decode_conv, inbuf, &inbuflen, outbuf, &outbuflen) < 0)
    return NULL;

  *outlen = sizeof(outbuf) - outbuflen;
  res = pcalloc(p, *outlen);
  memcpy(res, outbuf, *outlen);

  return res;
#else
  pr_trace_msg(trace_channel, 1,
    "missing iconv support, no %s decoding possible", encoding);
  return pstrdup(p, in);
#endif /* !HAVE_ICONV_H */
}

char *pr_encode_str(pool *p, const char *in, size_t inlen, size_t *outlen) {
#ifdef HAVE_ICONV_H
  size_t inbuflen, outbuflen;
  char *inbuf, outbuf[PR_TUNABLE_PATH_MAX*2], *res;

  if (!p || !in || !outlen) {
    errno = EINVAL;
    return NULL;
  }

  if (encode_conv == (iconv_t) -1) {
    errno = EPERM;
    return NULL;
  }

  inbuf = pcalloc(p, inlen);
  memcpy(inbuf, in, inlen);
  inbuflen = inlen;

  outbuflen = sizeof(outbuf);

  if (str_convert(encode_conv, inbuf, &inbuflen, outbuf, &outbuflen) < 0)
    return NULL;

  *outlen = sizeof(outbuf) - outbuflen;
  res = pcalloc(p, *outlen);
  memcpy(res, outbuf, *outlen);

  return res;
#else
  pr_trace_msg(trace_channel, 1,
    "missing iconv support, no %s encoding possible", encoding);
  return pstrdup(p, in);
#endif /* !HAVE_ICONV_H */
}

void pr_encode_disable_encoding(void) {
#ifdef HAVE_ICONV_H
  pr_trace_msg(trace_channel, 8, "%s encoding disabled", encoding);
  (void) encode_free();
  encoding = NULL;
#endif
}

/* Enables runtime use of encoding using the specified character set (assuming
 * NLS is supported).  Note that "UTF8", "utf8", "utf-8", and "UTF-8" are
 * accepted "character set" designations.
 */
int pr_encode_enable_encoding(const char *codeset) {
#ifdef HAVE_ICONV_H
  int res;

  if (codeset == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (encoding) {
    pr_trace_msg(trace_channel, 5,
      "attempting to switch encoding from %s to %s", encoding, codeset);

  } else {
    pr_trace_msg(trace_channel, 5, "attempting to enable %s encoding", codeset);
  }

  (void) encode_free();
  encoding = pstrdup(permanent_pool, codeset);

  res = encode_init();
  if (res < 0) {
    pr_trace_msg(trace_channel, 1,
      "failed to initialize encoding for %s, disabling encoding", codeset);
    encoding = NULL;
  }

  return res;

#else
  errno = ENOSYS;
  return -1;
#endif /* !HAVE_ICONV_H */
}

const char *pr_encode_get_charset(void) {
#ifdef HAVE_ICONV_H
  return local_charset;

#else
  errno = ENOSYS;
  return NULL;
#endif /* !HAVE_ICONV_H */
}

const char *pr_encode_get_encoding(void) {
#ifdef HAVE_ICONV_H
  return encoding;

#else
  errno = ENOSYS;
  return NULL;
#endif /* !HAVE_ICONV_H */
}

int pr_encode_set_charset_encoding(const char *charset, const char *codeset) {
#ifdef HAVE_ICONV_H
  int res;

  if (charset == NULL ||
      codeset == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (local_charset) {
    pr_trace_msg(trace_channel, 5,
      "attempting to switch local charset from %s to %s", local_charset,
      charset);

  } else {
    pr_trace_msg(trace_channel, 5, "attempting to use %s as local charset",
      charset);
  }

  if (encoding) {
    pr_trace_msg(trace_channel, 5,
      "attempting to switch encoding from %s to %s", encoding, codeset);

  } else {
    pr_trace_msg(trace_channel, 5, "attempting to use %s encoding", codeset);
  }

  (void) encode_free();

  local_charset = pstrdup(permanent_pool, charset);
  encoding = pstrdup(permanent_pool, codeset);

  res = encode_init();
  if (res < 0) {
    pr_trace_msg(trace_channel, 1,
      "failed to initialize encoding for local charset %s, encoding %s, "
      "disabling encoding", charset, codeset);
    local_charset = NULL;
    encoding = NULL;
  }

  return res;

#else
  errno = ENOSYS;
  return -1;
#endif /* !HAVE_ICONV_H */
}

int pr_encode_is_utf8(const char *codeset) {
  if (codeset == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (strcasecmp(codeset, "UTF8") == 0 ||
      strcasecmp(codeset, "UTF-8") == 0) {
    return TRUE;
  }

  return FALSE;
}

int pr_encode_supports_telnet_iac(void) {
  return supports_telnet_iac;
}

#endif /* PR_USE_NLS */
