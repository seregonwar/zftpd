/* Embedded web resource serving. */
#include "http_api_internal.h"
#include "http_config.h"
#include "http_csrf.h"
#include "http_resources.h"
#include "http_response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*===========================================================================*
 * STATIC RESOURCE SERVING
 *===========================================================================*/

/*---------------------------------------------------------------------------*
 * serve_static — serve files from embedded http_resources.c (compiled-in).
 *
 * All web assets are embedded directly in the binary.  The frontend patch
 * (disabling legacy Stream UI and blocking analytics) and the CSRF token
 * are injected at request time for index.html.
 *
 * Path traversal is prevented by rejecting any URI containing "..".
 *---------------------------------------------------------------------------*/
http_response_t *http_static_serve(const http_request_t *request) {
  static const char *k_frontend_patch =
      "<style>.nav-tab[data-view=\"stream\"],#view-stream{display:none !important;}</style>"
      "<script>(function(){function __zftpd_fix(){var b=document.body;if(!b)return;"
      "var h=document.querySelector('header.topbar');if(h){for(var n=b.firstChild;n&&n!==h;){var nx=n.nextSibling;"
      "if(n.nodeType===3){b.removeChild(n);}n=nx;}}"
      "var t=document.querySelector('.nav-tab[data-view=\\\"stream\\\"]');if(t&&t.parentNode)t.parentNode.removeChild(t);"
      "var v=document.getElementById('view-stream');if(v&&v.parentNode)v.parentNode.removeChild(v);}"
      "function __zftpd_is_blocked_url(u){return /google-analytics\\.com\\/mp\\/collect/i.test(String(u||''));}"
      "function __zftpd_patch_net(){"
      "var sb=navigator.sendBeacon;if(sb){navigator.sendBeacon=function(u,d){if(__zftpd_is_blocked_url(u))return true;return sb.apply(this,arguments);};}"
      "var of=window.fetch;if(of){window.fetch=function(input,init){var u=(typeof input==='string')?input:(input&&input.url?input.url:'');"
      "if(__zftpd_is_blocked_url(u)){return new Promise(function(resolve){resolve({ok:true,status:204,text:function(){return Promise.resolve('');},json:function(){return Promise.resolve({});}});});}"
      "return of.apply(this,arguments);};}"
      "var xo=XMLHttpRequest&&XMLHttpRequest.prototype&&XMLHttpRequest.prototype.open;"
      "var xs=XMLHttpRequest&&XMLHttpRequest.prototype&&XMLHttpRequest.prototype.send;"
      "if(xo&&xs){XMLHttpRequest.prototype.open=function(m,u){this.__zftpd_block=__zftpd_is_blocked_url(u);return xo.apply(this,arguments);};"
      "XMLHttpRequest.prototype.send=function(){if(this.__zftpd_block){try{this.readyState=4;this.status=204;if(this.onreadystatechange)this.onreadystatechange();if(this.onload)this.onload();}catch(e){}return;}return xs.apply(this,arguments);};}"
      "}"
      "__zftpd_patch_net();"
      "if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',__zftpd_fix);}else{__zftpd_fix();}})();</script>";

  const char *path = request->uri;
  if (path[0] == '/') {
    path++;
  }
  if (path[0] == '\0') {
    path = "index.html";
  }

  /* ── Strip query string (e.g. "?v=3" cache busting) ──
   *
   *  "css/base.css?v=3"  →  "css/base.css"
   *                 ^── stop here                        */
  char clean_path[1024];
  {
    const char *qmark = strchr(path, '?');
    size_t plen = qmark ? (size_t)(qmark - path) : strlen(path);
    if (plen >= sizeof(clean_path)) plen = sizeof(clean_path) - 1;
    memcpy(clean_path, path, plen);
    clean_path[plen] = '\0';
  }
  path = clean_path;

  /* Block path traversal */
  if (strstr(path, "..") != NULL) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

  /* Look up embedded resource */
  const http_resource_t *resource = NULL;
  if (!http_resource_get(path, &resource) || resource == NULL) {
    http_response_t *resp = http_response_create(HTTP_STATUS_404_NOT_FOUND);
    const char *msg = "404 Not Found";
    http_response_set_body(resp, msg, strlen(msg));
    return resp;
  }

  const unsigned char *raw_data = resource->data;
  size_t raw_size = resource->size;
  int is_html = (strstr(path, "index.html") != NULL);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type",
                           is_html ? "text/html; charset=utf-8" : resource->content_type);

  if (!is_html) {
    if (http_response_set_body(resp, (const char *)raw_data, raw_size) != 0) {
      (void)http_response_set_body_ref(resp, (const char *)raw_data, raw_size);
    }
    return resp;
  }

  /* ── index.html: inject frontend patch + optional CSRF token ── */
  {
    size_t patch_len = strlen(k_frontend_patch);
    size_t buf_size = raw_size + patch_len + 256U;
    char *buf = (char *)malloc(buf_size);
    if (buf == NULL) {
      http_response_destroy(resp);
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }

    memcpy(buf, (const char *)raw_data, raw_size);
    size_t out_len = raw_size;

    /* Inject frontend patch before </head> */
    {
      char *insert_at = strstr(buf, "</head>");
      if (insert_at != NULL) {
        size_t prefix_len = (size_t)(insert_at - buf);
        size_t suffix_len = out_len - prefix_len;
        memmove(insert_at + patch_len, insert_at, suffix_len);
        memcpy(insert_at, k_frontend_patch, patch_len);
        out_len += patch_len;
      }
    }

#if ENABLE_WEB_UPLOAD
    {
      const char *token = http_csrf_get_token();
      char meta_tag[128];
      snprintf(meta_tag, sizeof(meta_tag),
               "<meta name=\"csrf-token\" content=\"%s\">", token);

      const char *placeholder = "<!-- CSRF_TOKEN -->";
      char *found = strstr(buf, placeholder);
      if (found != NULL) {
        size_t prefix_len = (size_t)(found - buf);
        size_t placelen = strlen(placeholder);
        size_t taglen = strlen(meta_tag);
        size_t suffix_len = out_len - prefix_len - placelen;

        if (taglen <= placelen) {
          memcpy(found, meta_tag, taglen);
          if (taglen < placelen) {
            memmove(found + taglen, found + placelen, suffix_len);
          }
          out_len = out_len - placelen + taglen;
        } else {
          size_t extra = taglen - placelen;
          if (buf_size < out_len + extra + 1U) {
            size_t new_size = out_len + extra + 256U;
            char *tmp = (char *)realloc(buf, new_size);
            if (tmp == NULL) {
              free(buf);
              http_response_destroy(resp);
              return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
            }
            buf = tmp;
            buf_size = new_size;
            found = buf + prefix_len;
          }
          memmove(found + taglen, found + placelen, suffix_len);
          memcpy(found, meta_tag, taglen);
          out_len = out_len - placelen + taglen;
        }
      }
    }
#endif

    if (http_response_set_body_owned(resp, buf, out_len) == 0) {
      return resp;
    }
    free(buf);
    http_response_destroy(resp);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Body allocation failed");
  }
}

