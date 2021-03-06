/**
 * parser.c -- web log parsing
 * Copyright (C) 2009-2014 by Gerardo Orellana <goaccess@prosoftcorp.com>
 * GoAccess - An Ncurses apache weblog analyzer & interactive viewer
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * A copy of the GNU General Public License is attached to this
 * source distribution for its full text.
 *
 * Visit http://goaccess.prosoftcorp.com for new releases.
 */

/*
 * "_XOPEN_SOURCE" is required for the GNU libc to export "strptime(3)"
 * correctly.
 */
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <errno.h>

#if HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_NCURSESW_NCURSES_H
#include <ncursesw/ncurses.h>
#elif HAVE_NCURSES_NCURSES_H
#include <ncurses/ncurses.h>
#elif HAVE_NCURSES_H
#include <ncurses.h>
#elif HAVE_CURSES_H
#include <curses.h>
#endif

#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef HAVE_LIBTOKYOCABINET
#include "tcabinet.h"
#else
#include "glibht.h"
#endif

#ifdef HAVE_LIBGEOIP
#include "geolocation.h"
#endif

#include "parser.h"

#include "browsers.h"
#include "goaccess.h"
#include "error.h"
#include "opesys.h"
#include "settings.h"
#include "util.h"
#include "xmalloc.h"

/* allocate memory for ht raw data */
GRawData *
new_grawdata (void)
{
  GRawData *raw_data = xmalloc (sizeof (GRawData));
  memset (raw_data, 0, sizeof *raw_data);

  return raw_data;
}

/* allocate memory for raw data items */
GRawDataItem *
new_grawdata_item (unsigned int size)
{
  GRawDataItem *item = xcalloc (size, sizeof (GRawDataItem));
  return item;
}

/* free memory allocated in raw data */
void
free_raw_data (GRawData * raw_data)
{
#ifdef HAVE_LIBTOKYOCABINET
  int i;
  for (i = 0; i < raw_data->size; i++) {
    if (raw_data->items[i].key != NULL)
      free (raw_data->items[i].key);
    if (raw_data->items[i].value != NULL)
      free (raw_data->items[i].value);
  }
#endif
  free (raw_data->items);
  free (raw_data);
}

void
reset_struct (GLog * logger)
{
  logger->invalid = 0;
  logger->process = 0;
  logger->resp_size = 0LL;
}

GLog *
init_log (void)
{
  GLog *glog = xmalloc (sizeof (GLog));
  memset (glog, 0, sizeof *glog);

  return glog;
}

GLogItem *
init_log_item (GLog * logger)
{
  GLogItem *glog;
  logger->items = xmalloc (sizeof (GLogItem));
  glog = logger->items;
  memset (glog, 0, sizeof *glog);

  glog->agent = NULL;
  glog->date = NULL;
  glog->host = NULL;
  glog->ref = NULL;
  glog->method = NULL;
  glog->protocol = NULL;
  glog->req = NULL;
  glog->status = NULL;
  glog->resp_size = 0LL;
  glog->serve_time = 0;

  strncpy (glog->site, "", REF_SITE_LEN);
  glog->site[REF_SITE_LEN - 1] = '\0';

  return glog;
}

static void
free_logger (GLogItem * glog)
{
  if (glog->agent != NULL)
    free (glog->agent);
  if (glog->date != NULL)
    free (glog->date);
  if (glog->host != NULL)
    free (glog->host);
  if (glog->ref != NULL)
    free (glog->ref);
  if (glog->method != NULL)
    free (glog->method);
  if (glog->protocol != NULL)
    free (glog->protocol);
  if (glog->req != NULL)
    free (glog->req);
  if (glog->status != NULL)
    free (glog->status);
  if (glog->req_key != NULL)
    free (glog->req_key);
  free (glog);
}

#define BASE16_TO_10(x) (((x) >= '0' && (x) <= '9') ? ((x) - '0') : \
    (toupper((x)) - 'A' + 10))

static void
decode_hex (char *url, char *out)
{
  char *ptr;
  const char *c;

  for (c = url, ptr = out; *c; c++) {
    if (*c != '%' || !isxdigit (c[1]) || !isxdigit (c[2])) {
      *ptr++ = *c;
    } else {
      *ptr++ = (BASE16_TO_10 (c[1]) * 16) + (BASE16_TO_10 (c[2]));
      c += 2;
    }
  }
  *ptr = 0;
}

static char *
decode_url (char *url)
{
  char *out, *decoded;

  if ((url == NULL) || (*url == '\0'))
    return NULL;

  out = decoded = xstrdup (url);
  decode_hex (url, out);
  if (conf.double_decode)
    decode_hex (decoded, out);
  strip_newlines (out);

  return trim_str (out);
}

/* Process keyphrases from Google search, cache, and translate.
 * Note that the referer hasn't been decoded at the entry point
 * since there could be '&' within the search query. */
static int
process_keyphrases (char *ref)
{
  char *r, *ptr, *pch, *referer;
  int encoded = 0;

  if (!(strstr (ref, "http://www.google.")) &&
      !(strstr (ref, "http://webcache.googleusercontent.com/")) &&
      !(strstr (ref, "http://translate.googleusercontent.com/")))
    return 1;

  /* webcache.googleusercontent */
  if ((r = strstr (ref, "/+&")) != NULL)
    return 1;
  /* webcache.googleusercontent */
  else if ((r = strstr (ref, "/+")) != NULL)
    r += 2;
  /* webcache.googleusercontent */
  else if ((r = strstr (ref, "q=cache:")) != NULL) {
    pch = strchr (r, '+');
    if (pch)
      r += pch - r + 1;
  }
  /* www.google.* or translate.googleusercontent */
  else if ((r = strstr (ref, "&q=")) != NULL ||
           (r = strstr (ref, "?q=")) != NULL)
    r += 3;
  else if ((r = strstr (ref, "%26q%3D")) != NULL ||
           (r = strstr (ref, "%3Fq%3D")) != NULL)
    encoded = 1, r += 7;
  else
    return 1;

  if (!encoded && (ptr = strchr (r, '&')) != NULL)
    *ptr = '\0';
  else if (encoded && (ptr = strstr (r, "%26")) != NULL)
    *ptr = '\0';

  referer = decode_url (r);
  if (referer == NULL || *referer == '\0')
    return 1;

  referer = char_replace (referer, '+', ' ');
  process_generic_data (ht_keyphrases, trim_str (referer));
  free (referer);

  return 0;
}

/* parses a URI and extracts the *host* part from it
 * i.e., //www.example.com/path?googleguy > www.example.com */
static int
extract_referer_site (const char *referer, char *host)
{
  char *url, *begin, *end;
  int len = 0;

  if ((referer == NULL) || (*referer == '\0'))
    return 1;

  url = strdup (referer);
  if ((begin = strstr (url, "//")) == NULL)
    goto clean;

  begin += 2;
  if ((len = strlen (begin)) == 0)
    goto clean;

  if ((end = strchr (begin, '/')) != NULL)
    len = end - begin;

  if (len == 0)
    goto clean;

  if (len >= REF_SITE_LEN)
    len = REF_SITE_LEN - 1;

  memcpy (host, begin, len);
  host[len] = '\0';
  free (url);
  return 0;
clean:
  free (url);

  return 1;
}

/* process referer */
static void
process_referrers (char *referrer, char *site)
{
  char *ref;

  if (referrer == NULL)
    return;

  if (site != NULL && *site != '\0')
    process_generic_data (ht_referring_sites, site);
  process_keyphrases (referrer);

  ref = decode_url (referrer);
  if (ref == NULL || *ref == '\0')
    return;

  process_generic_data (ht_referrers, ref);
  free (ref);
}

/* process data based on a unique key, this includes the following
 * modules, VISITORS, BROWSERS, OS */
static void
process_unique_data (GLogItem * glog)
{
#ifdef HAVE_LIBGEOIP
  char city[CITY_LEN] = "";
  char continent[CONTINENT_LEN] = "";
  char country[COUNTRY_LEN] = "";
#endif

  char *a = NULL, *browser_key = NULL, *browser = NULL;
  char *os_key = NULL, *opsys = NULL;

  char visitor_key[UKEY_BUFFER];
  char os_type[OPESYS_TYPE_LEN], browser_type[BROWSER_TYPE_LEN];

  a = deblank (xstrdup (glog->agent));
  snprintf (visitor_key, sizeof (visitor_key), "%s|%s|%s", glog->host,
            glog->date_key, a);
  (visitor_key)[sizeof (visitor_key) - 1] = '\0';
  free (a);

  /* Check if the unique visitor key exists, if not,
   * process hit as unique visitor. Includes, BROWSERS, OSs, VISITORS. */
  if (process_generic_data (ht_unique_visitors, visitor_key) == -1) {
    process_generic_data (ht_unique_vis, glog->date_key);
    browser_key = xstrdup (glog->agent);
    os_key = xstrdup (glog->agent);

    /* extract browser & OS from agent  */
    browser = verify_browser (browser_key, browser_type);
    if (browser != NULL)
      process_browser (ht_browsers, browser, browser_type);

    opsys = verify_os (os_key, os_type);
    if (opsys != NULL)
      process_opesys (ht_os, opsys, os_type);

#ifdef HAVE_LIBGEOIP
    if (geo_location_data != NULL) {
      if (conf.geoip_database)
        geoip_get_city (glog->host, city, glog->type_ip);

      geoip_get_country (glog->host, country, glog->type_ip);
      geoip_get_continent (glog->host, continent, glog->type_ip);
      process_geolocation (ht_countries, country, continent, city);
    }
#endif
  }

  if (browser != NULL)
    free (browser);
  if (browser_key != NULL)
    free (browser_key);
  if (os_key != NULL)
    free (os_key);
  if (opsys != NULL)
    free (opsys);
}

/**
 * Append HTTP method to the request
 * ###NOTE: the whole string will serve as a key
 */
static void
append_method_to_request (char **key, const char *method)
{
  char *s = NULL;

  if (*key == NULL || **key == '\0')
    return;

  if (method == NULL || *method == '\0')
    return;

  s = xmalloc (snprintf (NULL, 0, "%s %s", method, *key) + 1);
  sprintf (s, "%s %s", method, *key);

  free (*key);
  *key = s;
}

/**
 * Append HTTP protocol to the request
 * ###NOTE: the whole string will serve as a key
 */
static void
append_protocol_to_request (char **key, const char *protocol)
{
  char *s = NULL;

  if (*key == NULL || **key == '\0')
    return;

  if (protocol == NULL || *protocol == '\0')
    return;

  s = xmalloc (snprintf (NULL, 0, "%s %s", protocol, *key) + 1);
  sprintf (s, "%s %s", protocol, *key);

  free (*key);
  *key = s;
}

/* returns 1 if the request seems to be a static file */
static int
verify_static_content (char *req)
{
  char *nul = req + strlen (req), *ext = NULL;
  int elen = 0, i;

  if (strlen (req) < conf.static_file_max_len)
    return 0;

  for (i = 0; i < conf.static_file_idx; ++i) {
    ext = conf.static_files[i];
    if (ext == NULL || *ext == '\0')
      continue;

    elen = strlen (ext);
    if (!memcmp (nul - elen, ext, elen))
      return 1;
  }
  return 0;
}

static const char *
extract_method (const char *token)
{
  const char *lookfor = NULL;

  if ((lookfor = "OPTIONS", !memcmp (token, lookfor, 7)) ||
      (lookfor = "GET", !memcmp (token, lookfor, 3)) ||
      (lookfor = "HEAD", !memcmp (token, lookfor, 4)) ||
      (lookfor = "POST", !memcmp (token, lookfor, 4)) ||
      (lookfor = "PUT", !memcmp (token, lookfor, 3)) ||
      (lookfor = "DELETE", !memcmp (token, lookfor, 6)) ||
      (lookfor = "TRACE", !memcmp (token, lookfor, 5)) ||
      (lookfor = "CONNECT", !memcmp (token, lookfor, 7)) ||
      (lookfor = "PATCH", !memcmp (token, lookfor, 5)) ||
      (lookfor = "options", !memcmp (token, lookfor, 7)) ||
      (lookfor = "get", !memcmp (token, lookfor, 3)) ||
      (lookfor = "head", !memcmp (token, lookfor, 4)) ||
      (lookfor = "post", !memcmp (token, lookfor, 4)) ||
      (lookfor = "put", !memcmp (token, lookfor, 3)) ||
      (lookfor = "delete", !memcmp (token, lookfor, 6)) ||
      (lookfor = "trace", !memcmp (token, lookfor, 5)) ||
      (lookfor = "connect", !memcmp (token, lookfor, 7)) ||
      (lookfor = "patch", !memcmp (token, lookfor, 5)))
    return lookfor;
  return NULL;
}

static int
invalid_protocol (const char *token)
{
  const char *lookfor = NULL;

  return !((lookfor = "HTTP/1.0", !memcmp (token, lookfor, 8)) ||
           (lookfor = "HTTP/1.1", !memcmp (token, lookfor, 8)));
}

static char *
parse_req (char *line, char **method, char **protocol)
{
  char *req = NULL, *request = NULL, *proto = NULL, *dreq = NULL;
  const char *meth;
  ptrdiff_t rlen;

  meth = extract_method (line);

  /* couldn't find a method, so use the whole request line */
  if (meth == NULL) {
    request = xstrdup (line);
  }
  /* method found, attempt to parse request */
  else {
    req = line + strlen (meth);
    if ((proto = strstr (line, " HTTP/1.0")) == NULL &&
        (proto = strstr (line, " HTTP/1.1")) == NULL) {
      return alloc_string ("-");
    }

    req++;
    if ((rlen = proto - req) <= 0)
      return alloc_string ("-");

    request = xmalloc (rlen + 1);
    strncpy (request, req, rlen);
    request[rlen] = 0;

    if (conf.append_method)
      (*method) = strtoupper (xstrdup (meth));

    if (conf.append_protocol)
      (*protocol) = strtoupper (xstrdup (++proto));
  }

  if ((dreq = decode_url (request)) && dreq != '\0') {
    free (request);
    return dreq;
  }

  return request;
}

static char *
parse_string (char **str, char end, int cnt)
{
  int idx = 0;
  char *pch = *str, *p;
  do {
    if (*pch == end)
      idx++;
    if ((*pch == end && cnt == idx) || *pch == '\0') {
      size_t len = (pch - *str + 1);
      p = xmalloc (len);
      memcpy (p, *str, (len - 1));
      p[len - 1] = '\0';
      *str += len - 1;
      return trim_str (p);
    }
    /* advance to the first unescaped delim */
    if (*pch == '\\')
      pch++;
  } while (*pch++);
  return NULL;
}

static int
parse_specifier (GLogItem * glog, const char *lfmt, const char *dfmt,
                 char **str, const char *p)
{
  char *pch, *sEnd, *bEnd, *tkn = NULL, *end = NULL;
  double serve_secs = 0.0;
  struct tm tm;
  unsigned long long bandw = 0, serve_time = 0;

  errno = 0;
  memset (&tm, 0, sizeof (tm));

  switch (*p) {
    /* date */
  case 'd':
    if (glog->date)
      return 1;
    /* parse date format including dates containing spaces,
     * i.e., syslog date format (Jul 15 20:10:56) */
    tkn = parse_string (&(*str), p[1], count_matches (dfmt, ' ') + 1);
    if (tkn == NULL)
      return 1;
    end = strptime (tkn, dfmt, &tm);
    if (end == NULL || *end != '\0') {
      free (tkn);
      return 1;
    }
    glog->date = tkn;
    break;
    /* remote hostname (IP only) */
  case 'h':
    if (glog->host)
      return 1;
    tkn = parse_string (&(*str), p[1], 1);
    if (tkn == NULL)
      return 1;
    if (invalid_ipaddr (tkn, &glog->type_ip)) {
      free (tkn);
      return 1;
    }
    glog->host = tkn;
    break;
    /* request method */
  case 'm':
    if (glog->method)
      return 1;
    tkn = parse_string (&(*str), p[1], 1);
    if (tkn == NULL)
      return 1;
    if (!extract_method (tkn)) {
      free (tkn);
      return 1;
    }
    glog->method = tkn;
    break;
    /* request not including method or protocol */
  case 'U':
    if (glog->req)
      return 1;
    tkn = parse_string (&(*str), p[1], 1);
    if (tkn == NULL || *tkn == '\0')
      return 1;
    if ((glog->req = decode_url (tkn)) == NULL)
      return 1;
    free (tkn);
    break;
    /* request protocol */
  case 'H':
    if (glog->protocol)
      return 1;
    tkn = parse_string (&(*str), p[1], 1);
    if (tkn == NULL)
      return 1;
    if (invalid_protocol (tkn)) {
      free (tkn);
      return 1;
    }
    glog->protocol = tkn;
    break;
    /* request, including method + protocol */
  case 'r':
    if (glog->req)
      return 1;
    tkn = parse_string (&(*str), p[1], 1);
    if (tkn == NULL)
      return 1;
    glog->req = parse_req (tkn, &glog->method, &glog->protocol);
    free (tkn);
    break;
    /* Status Code */
  case 's':
    if (glog->status)
      return 1;
    tkn = parse_string (&(*str), p[1], 1);
    if (tkn == NULL)
      return 1;
    strtol (tkn, &sEnd, 10);
    if (tkn == sEnd || *sEnd != '\0' || errno == ERANGE) {
      free (tkn);
      return 1;
    }
    glog->status = tkn;
    break;
    /* size of response in bytes - excluding HTTP headers */
  case 'b':
    if (glog->resp_size)
      return 1;
    tkn = parse_string (&(*str), p[1], 1);
    if (tkn == NULL)
      return 1;
    bandw = strtol (tkn, &bEnd, 10);
    if (tkn == bEnd || *bEnd != '\0' || errno == ERANGE)
      bandw = 0;
    glog->resp_size = bandw;
    conf.bandwidth = 1;
    free (tkn);
    break;
    /* referrer */
  case 'R':
    if (glog->ref)
      return 1;
    tkn = parse_string (&(*str), p[1], 1);
    if (tkn == NULL)
      tkn = alloc_string ("-");
    if (tkn != NULL && *tkn == '\0') {
      free (tkn);
      tkn = alloc_string ("-");
    }
    if (strcmp (tkn, "-") != 0)
      extract_referer_site (tkn, glog->site);
    glog->ref = tkn;
    break;
    /* user agent */
  case 'u':
    if (glog->agent)
      return 1;
    tkn = parse_string (&(*str), p[1], 1);
    if (tkn != NULL && *tkn != '\0') {
      /* Make sure the user agent is decoded (i.e.: CloudFront)
       * and replace all '+' with ' ' (i.e.: w3c) */
      glog->agent = char_replace (decode_url (tkn), '+', ' ');
      free (tkn);
      break;
    } else if (tkn != NULL && *tkn == '\0') {
      free (tkn);
      tkn = alloc_string ("-");
    }
    /* must be null */
    else {
      tkn = alloc_string ("-");
    }
    glog->agent = tkn;
    break;
    /* time taken to serve the request, in seconds */
  case 'T':
    if (glog->serve_time)
      return 1;
    /* ignore seconds if we have microseconds */
    if (strstr (lfmt, "%D") != NULL)
      break;

    tkn = parse_string (&(*str), p[1], 1);
    if (tkn == NULL)
      return 1;
    if (strchr (tkn, '.') != NULL)
      serve_secs = strtod (tkn, &bEnd);
    else
      serve_secs = strtoull (tkn, &bEnd, 10);

    if (tkn == bEnd || *bEnd != '\0' || errno == ERANGE)
      serve_secs = 0;
    /* convert it to microseconds */
    if (serve_secs > 0)
      glog->serve_time = serve_secs * SECS;
    else
      glog->serve_time = 0;
    conf.serve_usecs = 1;
    free (tkn);
    break;
    /* time taken to serve the request, in microseconds */
  case 'D':
    if (glog->serve_time)
      return 1;
    tkn = parse_string (&(*str), p[1], 1);
    if (tkn == NULL)
      return 1;
    serve_time = strtoull (tkn, &bEnd, 10);
    if (tkn == bEnd || *bEnd != '\0' || errno == ERANGE)
      serve_time = 0;
    glog->serve_time = serve_time;
    conf.serve_usecs = 1;
    free (tkn);
    break;
    /* everything else skip it */
  default:
    if ((pch = strchr (*str, p[1])) != NULL)
      *str += pch - *str;
  }

  return 0;
}

static int
parse_format (GLogItem * glog, const char *lfmt, const char *dfmt, char *str)
{
  const char *p;
  int special = 0;

  if (str == NULL || *str == '\0')
    return 1;

  /* iterate over the log format */
  for (p = lfmt; *p; p++) {
    if (*p == '%') {
      special++;
      continue;
    }
    if (special && *p != '\0') {
      if ((str == NULL) || (*str == '\0'))
        return 0;

      /* attempt to parse format specifiers */
      if (parse_specifier (glog, lfmt, dfmt, &str, p) == 1)
        return 1;
      special = 0;
    } else if (special && isspace (p[0])) {
      return 1;
    } else {
      str++;
    }
  }

  return 0;
}

static int
valid_line (char *line)
{
  /* invalid line */
  if ((line == NULL) || (*line == '\0'))
    return 1;
  /* ignore comments */
  if (*line == '#' || *line == '\n')
    return 1;

  return 0;
}

static void
lock_spinner (void)
{
  if (parsing_spinner != NULL && parsing_spinner->state == SPN_RUN)
    pthread_mutex_lock (&parsing_spinner->mutex);
}

static void
unlock_spinner (void)
{
  if (parsing_spinner != NULL && parsing_spinner->state == SPN_RUN)
    pthread_mutex_unlock (&parsing_spinner->mutex);
}

static void
count_invalid (GLog * logger)
{
  logger->invalid++;
#ifdef TCB_BTREE
  process_generic_data (ht_general_stats, "failed_requests");
#endif
}

static void
count_process (GLog * logger)
{
  lock_spinner ();
  logger->process++;
#ifdef TCB_BTREE
  process_generic_data (ht_general_stats, "total_requests");
#endif
  unlock_spinner ();
}

static int
process_date (GLogItem * glog)
{
  /* make compiler happy */
  memset (glog->date_key, 0, sizeof *glog->date_key);
  convert_date (glog->date_key, glog->date, conf.date_format, "%Y%m%d",
                DATE_LEN);
  if (glog->date_key == NULL)
    return 1;
  return 0;
}

static int
exclude_ip (GLog * logger, GLogItem * glog)
{
  if (conf.ignore_ip_idx && ip_in_range (glog->host)) {
    logger->exclude_ip++;
#ifdef TCB_BTREE
    process_generic_data (ht_general_stats, "exclude_ip");
#endif
    return 0;
  }
  return 1;
}

static int
exclude_crawler (GLogItem * glog)
{
  return conf.ignore_crawlers && is_crawler (glog->agent) ? 0 : 1;
}

/* process visitors, browsers, and OS */
static void
unique_data (GLogItem * glog)
{
  int uniq = conf.client_err_to_unique_count;
  if (!glog->status || glog->status[0] != '4' ||
      (uniq && glog->status[0] == '4'))
    process_unique_data (glog);
}

static void
process_log (GLogItem * glog)
{
  char *qmark = NULL;
  int is404 = 0;

  /* is this a 404? */
  if (glog->status && !memcmp (glog->status, "404", 3))
    is404 = 1;
  /* treat 444 as 404? */
  else if (glog->status && !memcmp (glog->status, "444", 3) &&
           conf.code444_as_404)
    is404 = 1;
  /* check if we need to remove the request's query string */
  else if (conf.ignore_qstr && (qmark = strchr (glog->req, '?')) != NULL) {
    if ((qmark - glog->req) > 0)
      *qmark = '\0';
  }

  glog->req_key = xstrdup (glog->req);
  /* include HTTP method/protocol to request */
  if (conf.append_method && glog->method) {
    glog->method = strtoupper (glog->method);
    append_method_to_request (&glog->req_key, glog->method);
  }
  if (conf.append_protocol && glog->protocol) {
    glog->protocol = strtoupper (glog->protocol);
    append_protocol_to_request (&glog->req_key, glog->protocol);
  }
  if ((conf.append_method) || (conf.append_protocol))
    glog->req_key = deblank (glog->req_key);

  unique_data (glog);
  /* process agents that are part of a host */
  if (conf.list_agents)
    process_host_agents (glog->host, glog->agent);
  /* process status codes */
  if (glog->status)
    process_generic_data (ht_status_code, glog->status);
  /* process 404s */
  if (is404)
    process_request (ht_not_found_requests, glog->req_key, glog);
  /* process static files */
  else if (verify_static_content (glog->req))
    process_request (ht_requests_static, glog->req_key, glog);
  /* process regular files */
  else
    process_request (ht_requests, glog->req_key, glog);

  /* process referrers */
  process_referrers (glog->ref, glog->site);
  /* process hosts */
  process_generic_data (ht_hosts, glog->host);
  /* process bandwidth  */
  process_request_meta (ht_date_bw, glog->date_key, glog->resp_size);
  process_request_meta (ht_file_bw, glog->req_key, glog->resp_size);
  process_request_meta (ht_host_bw, glog->host, glog->resp_size);

  /* process time taken to serve the request, in microseconds */
  process_request_meta (ht_file_serve_usecs, glog->req_key, glog->serve_time);
  process_request_meta (ht_host_serve_usecs, glog->host, glog->serve_time);

#ifdef TCB_BTREE
  process_request_meta (ht_general_stats, "bandwidth", glog->resp_size);
#endif
}

/* process a line from the log and store it accordingly */
static int
pre_process_log (GLog * logger, char *line, int test)
{
  GLogItem *glog;

  if (valid_line (line)) {
    count_invalid (logger);
    return 0;
  }

  count_process (logger);
  glog = init_log_item (logger);
  /* parse a line of log, and fill structure with appropriate values */
  if (parse_format (glog, conf.log_format, conf.date_format, line)) {
    count_invalid (logger);
    goto cleanup;
  }

  /* must have the following fields */
  if (glog->host == NULL || glog->date == NULL || glog->req == NULL) {
    count_invalid (logger);
    goto cleanup;
  }
  /* agent will be null in cases where %u is not specified */
  if (glog->agent == NULL)
    glog->agent = alloc_string ("-");

  /* testing log only */
  if (test)
    goto cleanup;

  if (process_date (glog)) {
    count_invalid (logger);
    goto cleanup;
  }
  /* ignore host or crawlers */
  if (exclude_ip (logger, glog) == 0)
    goto cleanup;
  if (exclude_crawler (glog) == 0)
    goto cleanup;
  if (ignore_referer (glog->site))
    goto cleanup;

  logger->resp_size += glog->resp_size;
  process_log (glog);

cleanup:
  free_logger (glog);

  return 0;
}

static int
read_log (GLog ** logger, int n)
{
  FILE *fp = NULL;
  char line[LINE_BUFFER];
  int i = 0, test = -1 == n ? 0 : 1;

  /* no log file, assume STDIN */
  if (conf.ifile == NULL) {
    fp = stdin;
    (*logger)->piping = 1;
  }

  /* make sure we can open the log (if not reading from stdin) */
  if (!(*logger)->piping && (fp = fopen (conf.ifile, "r")) == NULL)
    FATAL ("Unable to open the specified log file. %s", strerror (errno));

  while (fgets (line, LINE_BUFFER, fp) != NULL) {
    if (n >= 0 && i++ == n)
      break;

    /* start processing log line */
    if (pre_process_log ((*logger), line, test)) {
      if (!(*logger)->piping)
        fclose (fp);
      return 1;
    }
  }

  /* definitely not portable! */
  if ((*logger)->piping)
    freopen ("/dev/tty", "r", stdin);

  /* close log file if not a pipe */
  if (!(*logger)->piping)
    fclose (fp);

  return 0;
}

/* entry point to parse the log line by line */
int
parse_log (GLog ** logger, char *tail, int n)
{
  int test = -1 == n ? 0 : 1;

  if (conf.date_format == NULL || *conf.date_format == '\0')
    FATAL ("No date format was found on your conf file.");

  if (conf.log_format == NULL || *conf.log_format == '\0')
    FATAL ("No log format was found on your conf file.");

  /* process tail data and return */
  if (tail != NULL) {
    if (pre_process_log ((*logger), tail, test))
      return 1;
    return 0;
  }

  return read_log (logger, n);
}

/* make sure we have valid hits */
int
test_format (GLog * logger)
{
  if (parse_log (&logger, NULL, 20))
    FATAL ("Error while processing file");

  if ((logger->process == 0) || (logger->process == logger->invalid))
    return 1;
  return 0;
}
