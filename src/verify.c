/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "defines.h"
#include "cookie.h"
#include "verify.h"

#include <stdio.h>
#include <string.h>
#define APR_WANT_STRFUNC
#include "apr_want.h"
#include "apr_strings.h"
#include "apr_uuid.h"
#include "apr_tables.h"

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"       /* for ap_hook_(check_user_id | auth_checker) */
#include "apr_base64.h"
#include <curl/curl.h>
#include <curl/easy.h>

/* Helper struct for CURL response */
struct MemoryStruct
{
  char *memory;
  size_t size;
  size_t realsize;
  request_rec *r;
};

static const char *jsonErrorResponse =
  "{\"status\":\"failure\", \"reason\": \"%s: %s\"}";


/** Callback function for streaming CURL response */
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb,
                                  void *userp)
{
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *) userp;

  if (mem->size + realsize >= mem->realsize) {
    mem->realsize = mem->size + realsize + 256;
    void *tmp = apr_palloc(mem->r->pool, mem->size + realsize + 256);
    memcpy(tmp, mem->memory, mem->size);
    mem->memory = tmp;
  }

  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
  return realsize;
}

/*XXX: Needs length to be returned, signatures are binary blobs */
static char *base64url_decode(apr_pool_t * p, const char *input)
{
  int len = apr_base64_decode_len(input);
  char *decoded = apr_pcalloc(p, len);
  len = apr_base64_decode(decoded, input);
  return decoded;
}

VerifyResult verify_assertion_local(request_rec *r, const char *assertion)
{
  char *pair;
  char *last = NULL;
  VerifyResult res = apr_pcalloc(r->pool, sizeof(struct _VerifyResult));
  char *assertion_string = apr_pstrdup(r->pool, assertion);

  char *delim = ".";
  const char *assertions[16];

  int i = 0;

  /* XXX: Need to make sure we don't overrun assertions[] */
  for (pair = apr_strtok(assertion_string, delim, &last);
       pair; pair = apr_strtok(NULL, delim, &last)) {
    assertions[i++] = pair;
  }

  ap_log_rerror(APLOG_MARK, APLOG_WARNING | APLOG_NOERRNO, 0, r,
                ERRTAG
                "Local Assertion Verification enabled but not implemented yet");

  int assertion_count = i;

  json_object *json_assertions[16];
  enum json_tokener_error jerr;

  for (i = 0; i < assertion_count; i++) {
    assertions[i] = base64url_decode(r->pool, assertions[i]);
    json_assertions[i] = json_tokener_parse_verbose(assertions[i], &jerr);
    if (json_tokener_success != jerr) {
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r,
                    ERRTAG "json parse error %s",
                    json_tokener_error_desc(jerr));
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r,
                    ERRTAG "Raw Pair %d is %s", i, assertions[i]);
    }
    else {
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r,
                    ERRTAG "JSON is %s",
                    json_object_to_json_string(json_assertions[i]));
    }
  }

  json_object *principal =
    json_object_object_get(json_assertions[1], "principal");
  json_object *issuer = json_object_object_get(json_assertions[1], "iss");
  json_object *expiry = json_object_object_get(json_assertions[3], "exp");
  json_object *audience = json_object_object_get(json_assertions[3], "aud");
  json_object *email = NULL;

  /* Not fully implemented yet */
  res->errorResponse = "Local verification enabled but not implemented yet";

  if (principal && json_object_is_type(principal, json_type_object)) {
    email = json_object_object_get(principal, "email");
    if (email && json_object_is_type(email, json_type_string)) {
      res->verifiedEmail = json_object_get_string(email);
    }
  }
  if (issuer && json_object_is_type(issuer, json_type_string)) {
    res->identityIssuer = json_object_get_string(issuer);
  }
  if (audience && json_object_is_type(audience, json_type_string)) {
    const char *audience_str = json_object_get_string(audience);
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r,
                  ERRTAG "Audience is %s", audience_str);
  }

  if (expiry && json_object_is_type(expiry, json_type_int)) {
    int64_t expiry_time = json_object_get_int64(expiry);
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r,
                  ERRTAG "Expiry is %" APR_INT64_T_FMT, expiry_time);
  }

  /* Fake success */
  if (email && issuer) {
    res->errorResponse = NULL;
  }

  return res;
}



/* Pass the assertion to the verification service defined in the config,
 * and return the result to the caller */
static char *verifyAssertionRemote(request_rec *r, const char *verifier_url,
                                   char *assertionText)
{
  CURL *curl = curl_easy_init();

  curl_easy_setopt(curl, CURLOPT_URL, verifier_url);
  curl_easy_setopt(curl, CURLOPT_POST, 1);

  ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r,
                ERRTAG "Requesting verification with audience %s via %s",
                r->server->server_hostname, verifier_url);

  // XXX: audience should be an origin, see docs or issue mozilla/browserid#82
  char *body = apr_psprintf(r->pool, "assertion=%s&audience=%s",
                            assertionText, r->server->server_hostname);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  /** XXX set certificate for SSL negotiation */

  struct MemoryStruct chunk;
  chunk.memory = apr_pcalloc(r->pool, 1024);
  chunk.size = 0;
  chunk.realsize = 1024;
  chunk.r = r;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &chunk);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "libcurl-mod_authn_persona-agent/1.0");

  CURLcode result = curl_easy_perform(curl);
  if (result != 0) {
    ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, r,
                  ERRTAG
                  "Error while communicating with Persona verification server: %s",
                  curl_easy_strerror(result));
    curl_easy_cleanup(curl);
    return NULL;
  }
  long responseCode;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
  if (responseCode != 200) {
    ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, r,
                  ERRTAG
                  "Error while communicating with Persona verification server: result code %ld",
                  responseCode);
    curl_easy_cleanup(curl);
    return NULL;
  }
  curl_easy_cleanup(curl);
  return chunk.memory;
}

/*
 * process an assertion using the hosted verifier.
 *
 * TODO: local verification
 */
VerifyResult processAssertion(request_rec *r, const char *verifier_url,
                              const char *assertion)
{
  VerifyResult res = apr_pcalloc(r->pool, sizeof(struct _VerifyResult));
  json_tokener *tok = json_tokener_new();
  json_object *jobj = NULL;
  enum json_tokener_error jerr;

  char *assertionResult =
    verifyAssertionRemote(r, verifier_url, (char *) assertion);

  if (assertionResult) {
    jobj =
      json_tokener_parse_ex(tok, assertionResult, strlen(assertionResult));
    jerr = json_tokener_get_error(tok);

    if (json_tokener_success != jerr) {

      res->errorResponse = apr_psprintf(r->pool, jsonErrorResponse,
                                        "malformed payload",
                                        json_tokener_error_desc(jerr));
      json_tokener_free(tok);
      return res;
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r,
                  ERRTAG
                  "Assertion (parsed) recieved is : %s",
                  json_object_to_json_string(jobj));
  }
  else {
    // XXX: verifyAssertionRemote should return specific error message.
    res->errorResponse = apr_psprintf(r->pool, jsonErrorResponse,
                                      "communication error",
                                      "can't contact verification server");
    return res;
  }

  struct json_object_iterator it = json_object_iter_begin(jobj);
  struct json_object_iterator itEnd = json_object_iter_end(jobj);
  const char *reason = NULL;
  const char *status = "unknown";
  int success = 0;

  while (!json_object_iter_equal(&it, &itEnd)) {
    const char *key = json_object_iter_peek_name(&it);
    json_object *val = json_object_iter_peek_value(&it);

    if (strncmp("email", key, 6) == 0) {
      res->verifiedEmail = apr_pstrdup(r->pool, json_object_get_string(val));
    }
    else if (strncmp("issuer", key, 7) == 0) {
      res->identityIssuer = apr_pstrdup(r->pool, json_object_get_string(val));
    }
    else if (strncmp("audience", key, 9) == 0) {
      res->audience = apr_pstrdup(r->pool, json_object_get_string(val));
    }
    else if (strncmp("expires", key, 8) == 0) {
      apr_time_ansi_put(&res->expires, json_object_get_int64(val) / 1000);
    }
    else if (strncmp("reason", key, 7) == 0) {
      reason = json_object_get_string(val);
    }
    else if (strncmp("status", key, 7) == 0) {
      status = json_object_get_string(val);
      if (strncmp("okay", status, 5) == 0) {
        success = 1;
      }
    }
    json_object_iter_next(&it);
  }

  json_tokener_free(tok);

  // XXX: This is bad, doesn't catch multiple missing bits
  if (!res->verifiedEmail) {
    res->errorResponse = apr_pstrdup(r->pool, "Missing e-mail in assertion");
  }
  if (!res->identityIssuer) {
    res->errorResponse = apr_pstrdup(r->pool, "Missing issuer in assertion");
  }
  if (res->audience
      && strncmp(res->audience, r->server->server_hostname,
                 strlen(r->server->server_hostname)) != 0) {
    res->errorResponse =
      apr_psprintf(r->pool, "Audience %s doesn't match %s", res->audience,
                   r->server->server_hostname);
  }

  apr_time_t now = apr_time_now();
  if (res->expires && res->expires <= now) {
    char exp_time[APR_RFC822_DATE_LEN];
    apr_rfc822_date(exp_time, res->expires);
    res->errorResponse =
      apr_psprintf(r->pool, "Assertion expired on %s", exp_time);
  }

  if (!success) {
    if (reason) {
      res->errorResponse = apr_pstrdup(r->pool, reason);
    }
    else {
      res->errorResponse =
        apr_psprintf(r->pool, "Assertion failed with status '%s'", status);
    }
  }

  return res;
}
