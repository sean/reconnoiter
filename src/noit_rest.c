/*
 * Copyright (c) 2007, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name OmniTI Computer Consulting, Inc. nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "noit_defines.h"
#include "noit_listener.h"
#include "noit_http.h"
#include "noit_rest.h"

#include <pcre.h>
#include <errno.h>

struct rest_xml_payload {
  char *buffer;
  xmlDocPtr indoc;
  int len;
  int allocd;
  int complete;
};

struct rest_url_dispatcher {
  char *method;
  pcre *expression;
  pcre_extra *extra;
  rest_request_handler handler;
  /* Chain to the next one */
  struct rest_url_dispatcher *next;
};

struct rule_container {
  char *base;
  struct rest_url_dispatcher *rules;
  struct rest_url_dispatcher *rules_endptr;
};
noit_hash_table dispatch_points = NOIT_HASH_EMPTY;

static rest_request_handler
noit_http_get_handler(noit_http_rest_closure_t *restc) {
  struct rule_container *cont = NULL;
  struct rest_url_dispatcher *rule;
  char *eoq, *eob;
  eoq = strchr(restc->http_ctx->req.uri_str, '?');
  if(!eoq)
    eoq = restc->http_ctx->req.uri_str + strlen(restc->http_ctx->req.uri_str);
  eob = eoq - 1;

  /* find the right base */
  while(1) {
    void *vcont;
    while(eob >= restc->http_ctx->req.uri_str && *eob != '/') eob--;
    if(eob < restc->http_ctx->req.uri_str) break; /* off the front */
    if(noit_hash_retrieve(&dispatch_points, restc->http_ctx->req.uri_str,
                          eob - restc->http_ctx->req.uri_str + 1, &vcont)) {
      cont = vcont;
      eob++; /* move past the determined base */
      break;
    }
    eob--;
  }
  if(!cont) return NULL;
  for(rule = cont->rules; rule; rule = rule->next) {
    int ovector[30];
    int cnt;
    if(strcmp(rule->method, restc->http_ctx->req.method_str)) continue;
    if((cnt = pcre_exec(rule->expression, rule->extra, eob, eoq - eob, 0, 0,
                        ovector, sizeof(ovector)/sizeof(*ovector))) > 0) {
      /* We match, set 'er up */
      restc->fastpath = rule->handler;
      restc->nparams = cnt - 1;
      if(restc->nparams) {
        restc->params = calloc(restc->nparams, sizeof(*restc->params));
        for(cnt = 0; cnt < restc->nparams; cnt++) {
          int start = ovector[(cnt+1)*2];
          int end = ovector[(cnt+1)*2+1];
          restc->params[cnt] = malloc(end - start + 1);
          memcpy(restc->params[cnt], eob + start, end - start);
          restc->params[cnt][end - start] = '\0';
        }
      }
      return restc->fastpath;
    }
  }
  return NULL;
}
int
noit_http_rest_register(const char *method, const char *base,
                        const char *expr, rest_request_handler f) {
  void *vcont;
  struct rule_container *cont;
  struct rest_url_dispatcher *rule;
  const char *error;
  int erroffset;
  pcre *pcre_expr;
  int blen = strlen(base);
  /* base must end in a /, 'cause I said so */
  if(blen == 0 || base[blen-1] != '/') return -1;
  pcre_expr = pcre_compile(expr, 0, &error, &erroffset, NULL);
  if(!pcre_expr) {
    noitL(noit_error, "Error in rest expr(%s) '%s'@%d: %s\n",
          base, expr, erroffset, error);
    return -1;
  }
  rule = calloc(1, sizeof(*rule));
  rule->method = strdup(method);
  rule->expression = pcre_expr;
  rule->extra = pcre_study(rule->expression, 0, &error);
  rule->handler = f;

  /* Make sure we have a container */
  if(!noit_hash_retrieve(&dispatch_points, base, strlen(base), &vcont)) {
    cont = calloc(1, sizeof(*cont));
    cont->base = strdup(base);
    noit_hash_store(&dispatch_points, cont->base, strlen(cont->base), cont);
  }
  else cont = vcont;

  /* Append the rule */
  if(cont->rules_endptr) {
    cont->rules_endptr->next = rule;
    cont->rules_endptr = cont->rules_endptr->next;
  }
  else
    cont->rules = cont->rules_endptr = rule;
  return 0;
}

static noit_http_rest_closure_t *
noit_http_rest_closure_alloc() {
  noit_http_rest_closure_t *restc;
  restc = calloc(1, sizeof(*restc));
  return restc;
}
static void
noit_http_rest_clean_request(noit_http_rest_closure_t *restc) {
  int i;
  if(restc->nparams) {
    for(i=0;i<restc->nparams;i++) free(restc->params[i]);
    free(restc->params);
  }
  if(restc->call_closure_free) restc->call_closure_free(restc->call_closure);
  restc->call_closure_free = NULL;
  restc->call_closure = NULL;
  restc->nparams = 0;
  restc->params = NULL;
  restc->fastpath = NULL;
}
void
noit_http_rest_closure_free(noit_http_rest_closure_t *restc) {
  free(restc->remote_cn);
  noit_http_rest_clean_request(restc);
  free(restc);
}

int
noit_rest_request_dispatcher(noit_http_session_ctx *ctx) {
  noit_http_rest_closure_t *restc = ctx->dispatcher_closure;
  rest_request_handler handler = restc->fastpath;
  if(!handler) handler = noit_http_get_handler(restc);
  if(handler) {
    int rv;
    rv = handler(restc, restc->nparams, restc->params);
    if(ctx->res.closed) noit_http_rest_clean_request(restc);
    return rv;
  }
  noit_http_response_status_set(ctx, 404, "NOT FOUND");
  noit_http_response_option_set(ctx, NOIT_HTTP_CHUNKED);
  noit_http_rest_clean_request(restc);
  noit_http_response_end(ctx);
  return 0;
}

int
noit_http_rest_handler(eventer_t e, int mask, void *closure,
                       struct timeval *now) {
  int newmask = EVENTER_READ | EVENTER_EXCEPTION;
  acceptor_closure_t *ac = closure;
  noit_http_rest_closure_t *restc = ac->service_ctx;

  if(mask & EVENTER_EXCEPTION || (restc && restc->wants_shutdown)) {
socket_error:
    /* Exceptions cause us to simply snip the connection */
    eventer_remove_fd(e->fd);
    e->opset->close(e->fd, &newmask, e);
    if(restc) noit_http_rest_closure_free(restc);
    if(ac) acceptor_closure_free(ac);
    return 0;
  }

  if(!ac->service_ctx) {
    const char *primer = "";
    ac->service_ctx = restc = noit_http_rest_closure_alloc();
    restc->ac = ac;
    restc->remote_cn = strdup(ac->remote_cn ? ac->remote_cn : "");
    restc->http_ctx =
        noit_http_session_ctx_new(noit_rest_request_dispatcher,
                                  restc, e);
    
    switch(ac->cmd) {
      case NOIT_CONTROL_DELETE:
        primer = "DELE";
        break;
      case NOIT_CONTROL_GET:
        primer = "GET ";
        break;
      case NOIT_CONTROL_HEAD:
        primer = "HEAD";
        break;
      case NOIT_CONTROL_POST:
        primer = "POST";
        break;
      case NOIT_CONTROL_PUT:
        primer = "PUT ";
        break;
      default:
        goto socket_error;
    }
    noit_http_session_prime_input(restc->http_ctx, primer, 4);
  }
  return restc->http_ctx->drive(e, mask, restc->http_ctx, now);
}

int
noit_http_rest_raw_handler(eventer_t e, int mask, void *closure,
                           struct timeval *now) {
  int newmask = EVENTER_READ | EVENTER_EXCEPTION;
  acceptor_closure_t *ac = closure;
  noit_http_rest_closure_t *restc = ac->service_ctx;

  if(mask & EVENTER_EXCEPTION || (restc && restc->wants_shutdown)) {
    /* Exceptions cause us to simply snip the connection */
    eventer_remove_fd(e->fd);
    e->opset->close(e->fd, &newmask, e);
    if(restc) noit_http_rest_closure_free(restc);
    if(ac) acceptor_closure_free(ac);
    return 0;
  }
  if(!ac->service_ctx) {
    ac->service_ctx = restc = noit_http_rest_closure_alloc();
    restc->ac = ac;
    restc->http_ctx =
        noit_http_session_ctx_new(noit_rest_request_dispatcher,
                                  restc, e);
  }
  return restc->http_ctx->drive(e, mask, restc->http_ctx, now);
}

static void
rest_xml_payload_free(void *f) {
  struct rest_xml_payload *xmlin = f;
  if(xmlin->buffer) free(xmlin->buffer);
  if(xmlin->indoc) xmlFreeDoc(xmlin->indoc);
}

xmlDocPtr
rest_get_xml_upload(noit_http_rest_closure_t *restc,
                    int *mask, int *complete) {
  struct rest_xml_payload *rxc;
  if(restc->call_closure == NULL) {
    rxc = restc->call_closure = calloc(1, sizeof(*rxc));
    restc->call_closure_free = rest_xml_payload_free;
  }
  rxc = restc->call_closure;
  while(!rxc->complete) {
    int len;
    if(rxc->len == rxc->allocd) {
      char *b;
      rxc->allocd += 32768;
      b = rxc->buffer ? realloc(rxc->buffer, rxc->allocd) :
                        malloc(rxc->allocd);
      if(!b) {
        *complete = 1;
        return NULL;
      }
      rxc->buffer = b;
    }
    len = noit_http_session_req_consume(restc->http_ctx,
                                        rxc->buffer + rxc->len,
                                        rxc->allocd - rxc->len,
                                        mask);
    if(len > 0) rxc->len += len;
    if(len < 0 && errno == EAGAIN) return NULL;
    else if(len < 0) {
      *complete = 1;
      return NULL;
    }
    if(rxc->len == restc->http_ctx->req.content_length) {
      rxc->indoc = xmlParseMemory(rxc->buffer, rxc->len);
      rxc->complete = 1;
    }
  }

  *complete = 1;
  return rxc->indoc;
}
void noit_http_rest_init() {
  eventer_name_callback("noit_wire_rest_api/1.0", noit_http_rest_handler);
  eventer_name_callback("http_rest_api", noit_http_rest_raw_handler);
  noit_control_dispatch_delegate(noit_control_dispatch,
                                 NOIT_CONTROL_DELETE,
                                 noit_http_rest_handler);
  noit_control_dispatch_delegate(noit_control_dispatch,
                                 NOIT_CONTROL_GET,
                                 noit_http_rest_handler);
  noit_control_dispatch_delegate(noit_control_dispatch,
                                 NOIT_CONTROL_HEAD,
                                 noit_http_rest_handler);
  noit_control_dispatch_delegate(noit_control_dispatch,
                                 NOIT_CONTROL_POST,
                                 noit_http_rest_handler);
  noit_control_dispatch_delegate(noit_control_dispatch,
                                 NOIT_CONTROL_PUT,
                                 noit_http_rest_handler);
}
