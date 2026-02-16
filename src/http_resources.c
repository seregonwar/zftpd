/*
MIT License

Copyright (c) 2026 Seregon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
/**
 * @file http_resources.c
 * @brief Embedded web resources (index.html, style.css, app.js)
 *
 * All assets are stored as C string literals so the binary is
 * self-contained — no external file I/O needed at runtime.
 *
 * To regenerate from the web/ directory, keep the source-of-truth
 * files in web/ and copy the content into the strings below.
 */

#include <stddef.h>
#include <string.h>

/* Forward declaration — used by http_api.c via extern */
const char *http_get_resource(const char *path, size_t *size);

/*===========================================================================*
 *  INDEX.HTML
 *===========================================================================*/

static const char res_index_html[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
    "    <title>zftpd | File Explorer</title>\n"
    "    <link rel=\"stylesheet\" href=\"style.css\">\n"
    "    <!-- CSRF_TOKEN -->\n"
    "</head>\n"
    "<body>\n"
    "    <header class=\"topbar\">\n"
    "        <div class=\"topbar-left\">\n"
    "            <div class=\"brand\">\n"
    "                <div class=\"brand-title\">zftpd | File Explorer</div>\n"
    "                <div class=\"brand-subtitle\" id=\"current-path\">/</div>\n"
    "            </div>\n"
    "        </div>\n"
    "\n"
    "        <div class=\"topbar-right\">\n"
    "            <div id=\"status\" class=\"status-pill status-ok\">Connected</div>\n"
    "        </div>\n"
    "    </header>\n"
    "\n"
    "    <section class=\"toolbar\">\n"
    "        <button id=\"btn-up\" class=\"btn\" type=\"button\">Up</button>\n"
    "        <button id=\"btn-refresh\" class=\"btn\" type=\"button\">Refresh</button>\n"
    "        <button id=\"btn-upload\" class=\"btn\" type=\"button\">Upload File</button>\n"
    "        <button id=\"btn-create\" class=\"btn\" type=\"button\">Create File</button>\n"
    "        <div class=\"spacer\"></div>\n"
    "        <input id=\"search\" class=\"search\" type=\"text\" placeholder=\"Search...\" autocomplete=\"off\" autocapitalize=\"off\" spellcheck=\"false\">\n"
    "    </section>\n"
    "\n"
    "    <nav id=\"breadcrumb\" class=\"breadcrumb\"></nav>\n"
    "\n"
    "    <main class=\"content\">\n"
    "        <div id=\"file-list\" class=\"file-list\"></div>\n"
    "    </main>\n"
    "\n"
    "    <input id=\"file-input\" type=\"file\" multiple style=\"display:none\">\n"
    "    <div id=\"drop\" class=\"drop\">\n"
    "        <div class=\"drop-card\">\n"
    "            <div class=\"drop-title\">Drop files to upload</div>\n"
    "            <div id=\"drop-sub\" class=\"drop-sub\">Release to start upload</div>\n"
    "            <div class=\"drop-bar\"><div id=\"drop-bar\" class=\"drop-bar-fill\"></div></div>\n"
    "        </div>\n"
    "    </div>\n"
    "\n"
    "    <script src=\"app.js\"></script>\n"
    "</body>\n"
    "</html>\n";

/*===========================================================================*
 *  STYLE.CSS
 *===========================================================================*/

static const char res_style_css[] =
    "*{margin:0;padding:0;box-sizing:border-box}\n"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#0f1216;color:#e7edf5;min-height:100vh}\n"
    ".topbar{position:sticky;top:0;z-index:10;display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 16px;background:#171b21;border-bottom:2px solid #2b8cff}\n"
    ".brand-title{font-weight:700;font-size:16px}\n"
    ".brand-subtitle{margin-top:2px;font-size:12px;color:#9fb0c3;max-width:70vw;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
    ".status-pill{font-size:12px;padding:6px 10px;border-radius:999px;border:1px solid #2a3441;background:#1c222a}\n"
    ".status-ok{color:#2ecc71}\n"
    ".status-bad{color:#ff6b5f}\n"
    ".toolbar{display:flex;align-items:center;gap:8px;padding:10px 16px;background:#171b21;border-bottom:1px solid #2a3441}\n"
    ".btn{border:1px solid #2a3441;background:#1c222a;color:#e7edf5;border-radius:10px;padding:8px 10px;font-size:13px;cursor:pointer}\n"
    ".btn:active{transform:translateY(1px)}\n"
    ".spacer{flex:1}\n"
    ".search{width:60vw;max-width:340px;border:1px solid #2a3441;background:#0f1216;color:#e7edf5;border-radius:10px;padding:8px 10px;font-size:13px;outline:none}\n"
    ".breadcrumb{display:flex;gap:6px;padding:10px 16px;overflow-x:auto;border-bottom:1px solid #2a3441}\n"
    ".crumb{border:1px solid #2a3441;background:#171b21;color:#9fb0c3;border-radius:999px;padding:4px 8px;font-size:12px;cursor:pointer;white-space:nowrap}\n"
    ".crumb:hover{color:#e7edf5;border-color:#2b8cff}\n"
    ".content{padding:14px 16px}\n"
    ".file-list{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}\n"
    ".card{display:flex;align-items:center;gap:10px;padding:12px;border:1px solid #2a3441;background:#171b21;border-radius:14px;cursor:pointer}\n"
    ".card:hover{border-color:#2b8cff;background:#1c222a}\n"
    ".icon{width:38px;height:38px;border-radius:12px;display:flex;align-items:center;justify-content:center;background:rgba(43,140,255,.15);border:1px solid rgba(43,140,255,.25);font-size:18px}\n"
    ".meta{min-width:0;flex:1}\n"
    ".name{font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
    ".sub{margin-top:4px;font-size:12px;color:#9fb0c3;display:flex;gap:8px;align-items:center}\n"
    ".tag{border:1px solid #2a3441;border-radius:999px;padding:2px 8px;background:#0f1216}\n"
    ".loading,.empty,.error{padding:12px;border-radius:14px;border:1px solid #2a3441;background:#171b21;color:#9fb0c3}\n"
    ".error{border-color:rgba(196,43,28,.6);background:rgba(196,43,28,.12);color:#ffd6d1}\n"
    ".drop{position:fixed;inset:0;display:none;align-items:center;justify-content:center;background:rgba(15,18,22,.65);backdrop-filter:blur(4px);z-index:50}\n"
    ".drop.show{display:flex;animation:fadein .12s ease}\n"
    ".drop-card{width:min(520px,92vw);border:1px dashed rgba(43,140,255,.7);border-radius:16px;padding:18px;background:rgba(23,27,33,.95)}\n"
    ".drop-title{font-weight:700;font-size:16px}\n"
    ".drop-sub{margin-top:6px;color:#9fb0c3;font-size:13px}\n"
    ".drop-bar{margin-top:12px;height:10px;border-radius:999px;background:#0f1216;border:1px solid #2a3441;overflow:hidden}\n"
    ".drop-bar-fill{height:100%;width:0%;background:linear-gradient(90deg,#2b8cff,#00c2ff);transition:width .12s ease}\n"
    "@keyframes fadein{from{opacity:.6}to{opacity:1}}\n"
    "@media(max-width:860px){.file-list{grid-template-columns:1fr}}\n"
    ;

/*===========================================================================*
 *  APP.JS
 *===========================================================================*/

static const char res_app_js[] =
    "var D=document,$=D.getElementById.bind(D),E=encodeURIComponent,P=\"/\",L=[];\nfunction T(){var m=D.querySelector('meta[name=\"csrf-token\"]');return m?m.content:\"\"}\nfunction N(p){return!p||p[0]!==\"/\"?\"/\":p.length>1&&p[p.length-1]===\"/\"?p.slice(0,-1):p}\nfunction U(p){p=N(p);if(p===\"/\")return null;var i=p.lastIndexOf(\"/\");return i<=0?\"/\":p.slice(0,i)}\nfunction J(n){return P===\"/\"?\"/\"+n:P+\"/\"+n}\nfunction S(t,ok){var x=$(\"status\");x.textContent=t;x.className=\"status-pill \"+(ok?\"status-ok\":\"status-bad\")}\nfunction O(t,p){var x=$(\"drop\");if(t)$(\"drop-sub\").textContent=t;if(typeof p===\"number\")$(\"drop-bar\").style.width=p+\"%\";x.classList.add(\"show\")}\nfunction O0(){$(\"drop\").classList.remove(\"show\");$(\"drop-sub\").textContent=\"Release\";$(\"drop-bar\").style.width=\"0%\"}\nfunction B(){var b=$(\"breadcrumb\");b.innerHTML=\"\";var r=D.createElemen"
    "t(\"span\");r.className=\"crumb\";r.textContent=\"Root\";r.setAttribute(\"data-path\",\"/\");r.onclick=function(){L0(\"/\")};b.appendChild(r);var parts=N(P).split(\"/\"),a=\"\";for(var i=0;i<parts.length;i++){var p=parts[i];if(!p)continue;a+=\"/\"+p;var it=D.createElement(\"span\");it.className=\"crumb\";it.textContent=p;it.setAttribute(\"data-path\",a);it.onclick=function(){L0(this.getAttribute(\"data-path\"))};b.appendChild(it)}}\nfunction R(q){var fl=$(\"file-list\");fl.innerHTML=\"\";q=(q||\"\").trim().toLowerCase();var a=L||[],k=0;if(!a.length){fl.innerHTML='<div class=\"empty\">Empty</div>';return}for(var i=0;i<a.length;i++){var x=a[i];if(q&&x.name.toLowerCase().indexOf(q)<0)continue;k++;var dir=x.type===\"directory\",path=J(x.name),ic=dir?\"📁\":\"📄\";var c=D.createElement(\"div\");c.className=\"card\";c.setAttribute(\"data-path\",path);c.setAttribute(\"data-dir\",dir?\"1\":\"0\");"
    "c.onclick=function(){var p=this.getAttribute(\"data-path\");if(this.getAttribute(\"data-dir\")===\"1\")L0(p);else location.href=\"/api/download?path=\"+E(p)};c.innerHTML='<div class=\"icon\">'+ic+'</div><div class=\"meta\"><div class=\"name\">'+x.name+'</div></div>';fl.appendChild(c)}if(!k)fl.innerHTML='<div class=\"empty\">Empty</div>'}\nfunction L0(path){P=N(path);$(\"current-path\").textContent=P;$(\"file-list\").innerHTML='<div class=\"loading\">Loading...</div>';fetch(\"/api/list?path=\"+E(P)).then(function(r){if(!r.ok)throw new Error(\"HTTP \"+r.status);return r.json()}).then(function(d){L=d&&d.entries?d.entries:[];B();R($(\"search\").value);S(\"Connected\",1)}).catch(function(){ $(\"file-list\").innerHTML='<div class=\"error\">Error</div>';S(\"Error\",0)})}\nfunction U0(files){if(!files||!files.length)return;var i=0;function n(){if(i>=files.length){O0();L0(P);return}var f=files[i++];S(\"Upload\",0);O(\"Uploading\",0);var x=new XMLHttpRequest();x.open(\"POST\",\"/api/upload?path=\"+E(P)+\"&name=\"+E(f.name),1);var t=T();if(t)x.setRequestHeader(\"X-CSRF-Token\",t);x.upload.onprogress=function(e){if(e.lengthComputable)O(\"Uploading\",Math.floor(e.loaded/e.total*100))};x.onload=function(){x.status>=200&&x.status<300?n():(S(\"Error\",0),setTimeout(O0,800))};x.onerror=function(){S(\"Error\",0);setTimeout(O0,800)};x.send(f)}n()}\nfunction C0(){var name=prompt(\"File name\");if(!name)return;S(\"Creating...\",0);fetch(\"/api/create_file?path=\"+E(P)+\"&name=\"+E(name),{method:\"POST\",headers:{\"Content-Type\":\"text/plain\",\"X-CSRF-Token\":T()},body:\"\"}).then(function(r){if(!r.ok)throw new Error(\"HTTP \"+r.status);return r.json()}).then(function(){L0(P)}).catch(function(e){S(\"Error\",0);alert(\"Failed: \"+e.message)})}\nD.addEventListener(\"DOMContentLoaded\",functi"
    "on(){$(\"btn-up\").onclick=function(){var p=U(P);if(p!==null)L0(p)};$(\"btn-refresh\").onclick=function(){L0(P)};$(\"search\").oninput=function(){R(this.value)};$(\"btn-upload\").onclick=function(){$(\"file-input\").click()};$(\"file-input\").onchange=function(e){U0(e.target.files);e.target.value=\"\"};$(\"btn-create\").onclick=function(){C0()};var dr=0;D.addEventListener(\"dragenter\",function(e){e.preventDefault();dr++;O()});D.addEventListener(\"dragover\",function(e){e.preventDefault();O()});D.addEventListener(\"dragleave\",function(e){e.preventDefault();dr=Math.max(0,dr-1);if(dr===0)O0()});D.addEventListener(\"drop\",function(e){e.preventDefault();dr=0;O(\"Upload\",0);U0(e.dataTransfer.files)});L0(\"/\")});\n"
    ;

/*===========================================================================*
 *  RESOURCE LOOKUP
 *===========================================================================*/

const char *http_get_resource(const char *path, size_t *size) {
  if (path == NULL || size == NULL) {
    return NULL;
  }

  if (strcmp(path, "index.html") == 0) {
    *size = sizeof(res_index_html) - 1;
    return res_index_html;
  }

  if (strcmp(path, "style.css") == 0) {
    *size = sizeof(res_style_css) - 1;
    return res_style_css;
  }

  if (strcmp(path, "app.js") == 0) {
    *size = sizeof(res_app_js) - 1;
    return res_app_js;
  }

  return NULL;
}
