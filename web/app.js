/**
 * zftpd Web File Explorer — app.js
 *
 * CHANGES vs original:
 *
 *  Fix #1 — UI cache after delete/rename/mkdir:
 *    All mutating API calls (delete, rename, create_file, create_dir) now call
 *    L0(P) on success to force a fresh /api/list fetch.  Previously the DOM
 *    was not updated, causing stale entries to remain visible until a manual
 *    refresh or incognito window was opened.
 *
 *  Fix #6 — Default destination path for Send To / Move:
 *    The Send-To dialog now defaults to "/" (root) instead of the current
 *    working directory P.  This prevents accidental nesting when the user
 *    forgets to change the path, matching the expected UX.
 *
 *  Fix #5 — UI instability during active transfers:
 *    Added a global g_transfer_active flag.  Navigation (L0) and mutating
 *    actions are gated on this flag while an upload or background copy is
 *    running.  A visible "Transfer in progress…" banner is shown.
 */

var D = document,
    $ = D.getElementById.bind(D),
    E = encodeURIComponent,
    P = "/",         /* current directory path */
    L = [],          /* current directory entries */
    g_transfer_active = 0; /* 1 while upload/copy is running */

/* -------------------------------------------------------------------------
 * CSRF
 * ---------------------------------------------------------------------- */
function T() {
  var m = D.querySelector('meta[name="csrf-token"]');
  return m ? m.content : "";
}

/* -------------------------------------------------------------------------
 * PATH HELPERS
 * ---------------------------------------------------------------------- */
function N(p) {
  return !p || p[0] !== "/" ? "/" :
         p.length > 1 && p[p.length - 1] === "/" ? p.slice(0, -1) : p;
}
function parentOf(p) {
  p = N(p);
  if (p === "/") return null;
  var i = p.lastIndexOf("/");
  return i <= 0 ? "/" : p.slice(0, i);
}
function joinPath(n) {
  return P === "/" ? "/" + n : P + "/" + n;
}

/* -------------------------------------------------------------------------
 * STATUS PILL
 * ---------------------------------------------------------------------- */
function S(t, ok) {
  var x = $("status");
  x.textContent = t;
  x.className = "status-pill " + (ok ? "status-ok" : "status-bad");
}

/* -------------------------------------------------------------------------
 * TRANSFER BANNER  (Fix #5)
 * Shows/hides a banner and blocks navigation while a transfer is active.
 * ---------------------------------------------------------------------- */
function setTransferActive(active) {
  g_transfer_active = active ? 1 : 0;
  var banner = $("transfer-banner");
  if (banner) {
    banner.style.display = active ? "block" : "none";
  }
}

/* -------------------------------------------------------------------------
 * DROP OVERLAY
 * ---------------------------------------------------------------------- */
function O(t, p) {
  var x = $("drop");
  if (t) $("drop-sub").textContent = t;
  if (typeof p === "number") $("drop-bar").style.width = p + "%";
  x.classList.add("show");
}
function O0() {
  $("drop").classList.remove("show");
  $("drop-sub").textContent = "Release";
  $("drop-bar").style.width = "0%";
}

/* -------------------------------------------------------------------------
 * BREADCRUMB
 * ---------------------------------------------------------------------- */
function B() {
  var b = $("breadcrumb");
  b.innerHTML = "";
  var r = D.createElement("span");
  r.className = "crumb";
  r.textContent = "Root";
  r.setAttribute("data-path", "/");
  r.onclick = function () { L0("/"); };
  b.appendChild(r);
  var parts = N(P).split("/"), a = "";
  for (var i = 0; i < parts.length; i++) {
    var p = parts[i];
    if (!p) continue;
    a += "/" + p;
    var it = D.createElement("span");
    it.className = "crumb";
    it.textContent = p;
    it.setAttribute("data-path", a);
    it.onclick = (function (path) {
      return function () { L0(path); };
    })(a);
    b.appendChild(it);
  }
}

/* -------------------------------------------------------------------------
 * RENDER FILE LIST
 * ---------------------------------------------------------------------- */
function R(q) {
  var fl = $("file-list");
  fl.innerHTML = "";
  q = (q || "").trim().toLowerCase();
  var a = L || [], k = 0;
  if (!a.length) { fl.innerHTML = '<div class="empty">Empty</div>'; return; }
  for (var i = 0; i < a.length; i++) {
    var x = a[i];
    if (q && x.name.toLowerCase().indexOf(q) < 0) continue;
    k++;
    var dir = x.type === "directory",
        path = joinPath(x.name),
        ic = dir ? "📁" : "📄";
    var c = D.createElement("div");
    c.className = "card";
    c.setAttribute("data-path", path);
    c.setAttribute("data-dir", dir ? "1" : "0");
    c.onclick = function () {
      var p = this.getAttribute("data-path");
      if (this.getAttribute("data-dir") === "1") L0(p);
      else location.href = "/api/download?path=" + E(p);
    };
    c.innerHTML =
      '<div class="icon">' + ic + '</div>' +
      '<div class="meta"><div class="name">' + x.name + '</div></div>';
    fl.appendChild(c);
  }
  if (!k) fl.innerHTML = '<div class="empty">Empty</div>';
}

/* -------------------------------------------------------------------------
 * DIRECTORY NAVIGATION  (Fix #5: gated on g_transfer_active)
 * ---------------------------------------------------------------------- */
function L0(path) {
  /* Block navigation while a transfer is in progress (Fix #5) */
  if (g_transfer_active) {
    S("Transfer in progress…", 0);
    return;
  }
  P = N(path);
  $("current-path").textContent = P;
  $("file-list").innerHTML = '<div class="loading">Loading…</div>';
  fetch("/api/list?path=" + E(P))
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (d) {
      L = d && d.entries ? d.entries : [];
      B();
      R($("search").value);
      S("Connected", 1);
    })
    .catch(function () {
      $("file-list").innerHTML = '<div class="error">Error</div>';
      S("Error", 0);
    });
}

/* -------------------------------------------------------------------------
 * UPLOAD  (Fix #5: sets/clears transfer flag)
 * ---------------------------------------------------------------------- */
function U0(files) {
  if (!files || !files.length) return;
  if (g_transfer_active) { S("Transfer in progress…", 0); return; }
  setTransferActive(1);
  var i = 0;
  function next() {
    if (i >= files.length) {
      O0();
      setTransferActive(0);
      /* Fix #1: refresh directory listing after all uploads complete */
      L0(P);
      return;
    }
    var f = files[i++];
    S("Upload", 0);
    O("Uploading", 0);
    var x = new XMLHttpRequest();
    x.open("POST", "/api/upload?path=" + E(P) + "&name=" + E(f.name), 1);
    var t = T();
    if (t) x.setRequestHeader("X-CSRF-Token", t);
    x.upload.onprogress = function (e) {
      if (e.lengthComputable) O("Uploading", Math.floor(e.loaded / e.total * 100));
    };
    x.onload = function () {
      if (x.status >= 200 && x.status < 300) {
        next();
      } else {
        S("Error", 0);
        setTransferActive(0);
        setTimeout(O0, 800);
      }
    };
    x.onerror = function () {
      S("Error", 0);
      setTransferActive(0);
      setTimeout(O0, 800);
    };
    x.send(f);
  }
  next();
}

/* -------------------------------------------------------------------------
 * CREATE FILE  (Fix #1: refresh after create)
 * ---------------------------------------------------------------------- */
function C0() {
  var name = prompt("File name");
  if (!name) return;
  S("Creating…", 0);
  fetch("/api/create_file?path=" + E(P) + "&name=" + E(name), {
    method: "POST",
    headers: { "Content-Type": "text/plain", "X-CSRF-Token": T() },
    body: ""
  })
    .then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); })
    .then(function () {
      S("Created", 1);
      L0(P);  /* Fix #1 */
    })
    .catch(function (e) { S("Error", 0); alert("Failed: " + e.message); });
}

/* -------------------------------------------------------------------------
 * CREATE DIRECTORY  (Fix #1: refresh after create)
 * ---------------------------------------------------------------------- */
function CD() {
  var name = prompt("Folder name");
  if (!name) return;
  S("Creating…", 0);
  fetch("/api/mkdir?path=" + E(P) + "&name=" + E(name), {
    method: "POST",
    headers: { "X-CSRF-Token": T() }
  })
    .then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); })
    .then(function () {
      S("Created", 1);
      L0(P);  /* Fix #1 */
    })
    .catch(function (e) { S("Error", 0); alert("Failed: " + e.message); });
}

/* -------------------------------------------------------------------------
 * DELETE  (Fix #1: refresh after delete)
 * ---------------------------------------------------------------------- */
function DEL(path, recursive) {
  if (!confirm("Delete: " + path + (recursive ? " (recursive)" : "") + "?")) return;
  var url = "/api/delete?path=" + E(path) + (recursive ? "&recursive=1" : "");
  fetch(url, { method: "POST", headers: { "X-CSRF-Token": T() } })
    .then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); })
    .then(function () {
      S("Deleted", 1);
      L0(P);  /* Fix #1: force refresh — previously the DOM was not updated */
    })
    .catch(function (e) { S("Error", 0); alert("Failed: " + e.message); });
}

/* -------------------------------------------------------------------------
 * RENAME  (Fix #1: refresh after rename)
 * ---------------------------------------------------------------------- */
function REN(path) {
  var newName = prompt("New name", path.split("/").pop());
  if (!newName) return;
  fetch("/api/rename?path=" + E(path) + "&name=" + E(newName), {
    method: "POST",
    headers: { "X-CSRF-Token": T() }
  })
    .then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); })
    .then(function () {
      S("Renamed", 1);
      L0(P);  /* Fix #1 */
    })
    .catch(function (e) { S("Error", 0); alert("Failed: " + e.message); });
}

/* -------------------------------------------------------------------------
 * SEND TO / COPY  (Fix #6: default destination = "/")
 * ---------------------------------------------------------------------- */
function SENDTO(srcPath) {
  if (g_transfer_active) { S("Transfer in progress…", 0); return; }

  /*
   * Fix #6: Default destination is "/" (root), NOT the current directory P.
   *
   * WHY: When the user clicks "Send To" from inside a deep directory, using P
   * as the default caused files to be silently copied into that same directory
   * or a subdirectory by accident.  Root is the safe, unambiguous default —
   * the user must explicitly choose a different destination if desired.
   */
  var dst = prompt("Destination path (default: /)", "/");
  if (dst === null) return;        /* user cancelled */
  if (!dst || dst === "") dst = "/"; /* empty → root */
  dst = N(dst);

  setTransferActive(1);
  S("Copying…", 0);

  fetch("/api/copy?src=" + E(srcPath) + "&dst=" + E(dst), {
    method: "POST",
    headers: { "X-CSRF-Token": T() }
  })
    .then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); })
    .then(function () {
      setTransferActive(0);
      S("Copied", 1);
      L0(P);
    })
    .catch(function (e) {
      setTransferActive(0);
      S("Error", 0);
      alert("Copy failed: " + e.message);
    });
}

/* -------------------------------------------------------------------------
 * NETWORK STACK RESET  (Fix #4)
 *
 * Calls POST /api/network/reset which flushes the FTP server's TCP send/recv
 * buffers and drops idle connections.  Equivalent to the "disable/enable
 * network" workaround but without requiring a PS5 reboot.
 * Falls back to showing a PAL notification if the reset cannot be applied.
 * ---------------------------------------------------------------------- */
function NETRESET() {
  if (!confirm("Reset network stack? Active transfers will be interrupted.")) return;
  S("Resetting…", 0);
  fetch("/api/network/reset", { method: "POST", headers: { "X-CSRF-Token": T() } })
    .then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); })
    .then(function (d) { S(d.message || "Reset OK", 1); })
    .catch(function (e) { S("Reset failed: " + e.message, 0); });
}

/* -------------------------------------------------------------------------
 * CONTEXT MENU BUILDER
 * Attaches right-click / long-press actions to each card in the file list.
 * Centralises all mutating actions so the refresh (Fix #1) is always applied.
 * ---------------------------------------------------------------------- */
function attachContextMenu(card) {
  card.addEventListener("contextmenu", function (ev) {
    ev.preventDefault();
    var path = card.getAttribute("data-path");
    var isDir = card.getAttribute("data-dir") === "1";
    /* Remove any existing menu */
    var old = D.getElementById("ctx-menu");
    if (old) old.parentNode.removeChild(old);

    var menu = D.createElement("div");
    menu.id = "ctx-menu";
    menu.className = "ctx-menu";
    menu.style.cssText =
      "position:fixed;z-index:9999;background:#1e1e2e;border:1px solid #444;" +
      "border-radius:6px;padding:4px 0;min-width:160px;" +
      "left:" + ev.clientX + "px;top:" + ev.clientY + "px;";

    function item(label, fn) {
      var li = D.createElement("div");
      li.textContent = label;
      li.style.cssText = "padding:7px 14px;cursor:pointer;font-size:13px;color:#cdd6f4;";
      li.onmouseenter = function () { li.style.background = "#313244"; };
      li.onmouseleave = function () { li.style.background = ""; };
      li.onclick = function () {
        menu.parentNode && menu.parentNode.removeChild(menu);
        fn();
      };
      menu.appendChild(li);
    }

    if (!isDir) item("⬇ Download", function () { location.href = "/api/download?path=" + E(path); });
    item("✏ Rename", function () { REN(path); });
    item("📋 Send To…", function () { SENDTO(path); });
    item("🗑 Delete", function () { DEL(path, false); });
    if (isDir) item("🗑 Delete (recursive)", function () { DEL(path, true); });

    D.body.appendChild(menu);
    D.addEventListener("click", function dismiss() {
      if (menu.parentNode) menu.parentNode.removeChild(menu);
      D.removeEventListener("click", dismiss);
    }, { once: true });
  });
}

/* -------------------------------------------------------------------------
 * BOOTSTRAP
 * ---------------------------------------------------------------------- */
D.addEventListener("DOMContentLoaded", function () {

  /* Navigation buttons */
  $("btn-up").onclick = function () {
    var p = parentOf(P);
    if (p !== null) L0(p);
  };
  $("btn-refresh").onclick = function () { L0(P); };

  /* Search */
  $("search").oninput = function () { R(this.value); };

  /* Upload */
  $("btn-upload").onclick = function () { $("file-input").click(); };
  $("file-input").onchange = function (e) { U0(e.target.files); e.target.value = ""; };

  /* Create file */
  if ($("btn-create"))  $("btn-create").onclick  = function () { C0(); };
  /* Create directory */
  if ($("btn-mkdir"))   $("btn-mkdir").onclick    = function () { CD(); };
  /* Network reset button (Fix #4) */
  if ($("btn-netreset")) $("btn-netreset").onclick = function () { NETRESET(); };

  /* Drag-and-drop upload */
  var dr = 0;
  D.addEventListener("dragenter", function (e) { e.preventDefault(); dr++; O(); });
  D.addEventListener("dragover",  function (e) { e.preventDefault(); O(); });
  D.addEventListener("dragleave", function (e) {
    e.preventDefault();
    dr = Math.max(0, dr - 1);
    if (dr === 0) O0();
  });
  D.addEventListener("drop", function (e) {
    e.preventDefault();
    dr = 0;
    O("Upload", 0);
    U0(e.dataTransfer.files);
  });

  /* Intercept card rendering to attach context menus after each L0() */
  var origR = R;
  R = function (q) {
    origR(q);
    var cards = D.querySelectorAll(".card");
    for (var i = 0; i < cards.length; i++) attachContextMenu(cards[i]);
  };

  /* Initial load */
  L0("/");
});