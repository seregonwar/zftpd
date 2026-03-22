/* ══ FILE EXPLORER VIEW ═══════════════════════════════════════════════════
 * Classic file browser with grid/list/details views, breadcrumb navigation,
 * context menu, upload, and file operations.
 * ES5 compatible for PS5 browser.
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var D = document;
  var $ = Z.$;
  var E = Z.E;
  var ICO = Z.ICO;

  var explorer = {};
  var _view = 'grid'; /* grid | list | details */
  var _sortKey = 'name';
  var _sortAsc = true;

  /* ── Navigate to directory ── */
  var _viewInitialized = false;
  explorer.nav = function (path) {
    if (!_viewInitialized) {
      /* Apply the defaultView user setting initially */
      _view = (Z.settings && Z.settings.defaultView) ? Z.settings.defaultView : 'grid';
      _viewInitialized = true;
    }
    if (Z.state.transferActive) {
      Z.toast('Transfer in progress\u2026', 'wn');
      return;
    }
    Z.state.path = Z.norm(path);
    updatePath();
    renderBreadcrumb();

    var fl = $('file-list');
    if (fl) fl.innerHTML = '<div class="s-card"><div class="loader"></div><div>Loading\u2026</div></div>';

    Z.api.list(Z.state.path).then(function (d) {
      Z.state.entries = (d && Array.isArray(d.entries)) ? d.entries : [];
      sortEntries();
      render($('search') ? $('search').value : '');
      updateStatus(true);
      updateCount();
    }).catch(function () {
      if (fl) fl.innerHTML = '<div class="s-card s-err"><div class="s-ico">' + ICO.alert + '</div><div>Failed to load directory</div></div>';
      updateStatus(false);
    });
  };

  /* ── Render file list ── */
  function render(query) {
    var fl = $('file-list');
    if (!fl) return;
    fl.innerHTML = '';
    fl.className = 'fl vg-' + _view;

    query = (query || '').trim().toLowerCase();
    var entries = Z.state.entries || [];
    var filtered = entries.filter(function (x) {
      /* ── Settings-based filters ──
       * Hide dangerous system folders and dotfiles unless the user
       * explicitly enabled them in Settings.                        */
      var isDir = x.type === 'directory';
      if (isDir && !Z.settings.showSystemFolders && Z.isDangerousFolder && Z.isDangerousFolder(x.name)) return false;
      if (!Z.settings.showHiddenFiles && Z.isHiddenFile && Z.isHiddenFile(x.name)) return false;

      return !query || x.name.toLowerCase().indexOf(query) >= 0;
    });

    if (!filtered.length) {
      fl.innerHTML = '<div class="s-card"><div class="s-ico">' + ICO.folder + '</div><div>Empty directory</div></div>';
      return;
    }

    if (_view === 'details') {
      renderDetails(fl, filtered);
    } else {
      for (var i = 0; i < filtered.length; i++) {
        var entry = filtered[i];
        var isDir = entry.type === 'directory';
        var path = Z.join(Z.state.path, entry.name);
        var cat = Z.fileCategory(entry.name, isDir);
        var card = createCard(entry, path, isDir, cat);
        fl.appendChild(card);
      }
    }
  }

  /* ── Create a card element ── */
  function createCard(entry, path, isDir, cat) {
    var card = D.createElement('div');
    card.className = 'card';
    card.setAttribute('data-path', path);
    card.setAttribute('data-dir', isDir ? '1' : '0');
    card.setAttribute('data-name', entry.name);

    var iconHtml = isDir ? ICO.folder : ICO.file;
    var iconClass = 'fi-' + cat;
    var ext = Z.extname(entry.name);
    var sizeStr = isDir ? '' : Z.bytes(entry.size || 0);

    if (_view === 'grid') {
      card.innerHTML =
        '<div class="c-ico"><div class="fi-wrap ' + iconClass + '">' + iconHtml + '</div></div>' +
        '<div class="c-name" title="' + entry.name + '">' + entry.name + '</div>' +
        '<div class="c-meta">' +
          (ext && !isDir ? '<span class="xb">' + ext + '</span>' : '') +
          (sizeStr ? '<span class="sb">' + sizeStr + '</span>' : '') +
        '</div>';
    } else { /* list */
      card.innerHTML =
        '<div class="c-ico"><div class="fi-wrap ' + iconClass + '">' + iconHtml + '</div></div>' +
        '<div class="c-name" title="' + entry.name + '">' + entry.name + '</div>' +
        '<div class="c-right">' +
          (ext && !isDir ? '<span class="xb">' + ext + '</span>' : '') +
          (sizeStr ? '<span class="sb">' + sizeStr + '</span>' : '') +
        '</div>';
    }

    /* Click: navigate or download */
    card.onclick = function () {
      if (isDir) explorer.nav(path);
      else window.location.href = Z.api.downloadUrl(path);
    };

    /* Context menu */
    card.addEventListener('contextmenu', function (ev) {
      ev.preventDefault();
      ev.stopPropagation();
      showCtx(ev, entry, path, isDir);
    });

    return card;
  }

  /* ── Details/table view ── */
  function renderDetails(fl, entries) {
    var wrap = D.createElement('div');
    wrap.className = 'dtbl-wrap';
    var tbl = D.createElement('table');
    tbl.className = 'dtbl';

    var thead = D.createElement('thead');
    thead.innerHTML =
      '<tr><th class="t-ic"></th>' +
      '<th data-sort="name">Name</th>' +
      '<th class="t-ex" data-sort="ext">Type</th>' +
      '<th class="t-sz" data-sort="size">Size</th>' +
      '<th class="t-dt" data-sort="mtime">Modified</th></tr>';
    tbl.appendChild(thead);

    /* Sort headers */
    var ths = thead.querySelectorAll('th[data-sort]');
    for (var h = 0; h < ths.length; h++) {
      (function (th) {
        th.onclick = function () {
          var key = th.getAttribute('data-sort');
          if (_sortKey === key) _sortAsc = !_sortAsc;
          else { _sortKey = key; _sortAsc = true; }
          sortEntries();
          render($('search') ? $('search').value : '');
        };
      })(ths[h]);
    }

    var tbody = D.createElement('tbody');
    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      var isDir = e.type === 'directory';
      var path = Z.join(Z.state.path, e.name);
      var cat = Z.fileCategory(e.name, isDir);
      var ext = Z.extname(e.name);

      var tr = D.createElement('tr');
      tr.setAttribute('data-path', path);
      tr.innerHTML =
        '<td class="t-ic"><span class="fi-' + cat + '">' + (isDir ? ICO.folder : ICO.file) + '</span></td>' +
        '<td class="t-nm" title="' + e.name + '">' + e.name + '</td>' +
        '<td class="t-ex">' + (ext ? '<span class="xb">' + ext + '</span>' : (isDir ? 'Folder' : '\u2014')) + '</td>' +
        '<td class="t-sz">' + (isDir ? '\u2014' : Z.bytes(e.size || 0)) + '</td>' +
        '<td class="t-dt">' + (e.mtime ? Z.relativeTime(e.mtime) : '\u2014') + '</td>';

      tr.onclick = function () {
        var p = this.getAttribute('data-path');
        var d = this.querySelector('.t-ex');
        var isDirRow = d && d.textContent === 'Folder';
        if (isDirRow) explorer.nav(p);
        else window.location.href = Z.api.downloadUrl(p);
      };

      tr.addEventListener('contextmenu', function (ev) {
        ev.preventDefault();
        ev.stopPropagation();
        var p = this.getAttribute('data-path');
        var n = this.querySelector('.t-nm').textContent;
        var d = this.querySelector('.t-ex').textContent === 'Folder';
        showCtx(ev, { name: n, type: d ? 'directory' : 'file', size: 0 }, p, d);
      });

      tbody.appendChild(tr);
    }
    tbl.appendChild(tbody);
    wrap.appendChild(tbl);
    fl.appendChild(wrap);
  }

  /* ── Sort entries ── */
  function sortEntries() {
    var entries = Z.state.entries;
    if (!entries) return;
    entries.sort(function (a, b) {
      /* Directories first */
      var da = a.type === 'directory' ? 0 : 1;
      var db = b.type === 'directory' ? 0 : 1;
      if (da !== db) return da - db;

      var va, vb;
      if (_sortKey === 'name') {
        va = (a.name || '').toLowerCase();
        vb = (b.name || '').toLowerCase();
        return _sortAsc ? (va < vb ? -1 : va > vb ? 1 : 0) : (vb < va ? -1 : vb > va ? 1 : 0);
      }
      if (_sortKey === 'size') {
        va = a.size || 0;
        vb = b.size || 0;
        return _sortAsc ? va - vb : vb - va;
      }
      if (_sortKey === 'ext') {
        va = Z.extname(a.name || '');
        vb = Z.extname(b.name || '');
        return _sortAsc ? (va < vb ? -1 : va > vb ? 1 : 0) : (vb < va ? -1 : vb > va ? 1 : 0);
      }
      if (_sortKey === 'mtime') {
        va = a.mtime || 0;
        vb = b.mtime || 0;
        return _sortAsc ? va - vb : vb - va;
      }
      return 0;
    });
  }

  /* ── Breadcrumb ── */
  function renderBreadcrumb() {
    var bc = $('breadcrumb');
    if (!bc) return;
    bc.innerHTML = '';

    var root = D.createElement('span');
    root.className = 'crumb' + (Z.state.path === '/' ? ' act' : '');
    root.innerHTML = ICO.home + ' Root';
    root.onclick = function () { explorer.nav('/'); };
    bc.appendChild(root);

    var parts = Z.norm(Z.state.path).split('/');
    var acc = '';
    for (var i = 0; i < parts.length; i++) {
      var p = parts[i];
      if (!p) continue;
      acc += '/' + p;
      var sep = D.createElement('span');
      sep.className = 'cr-sep';
      sep.textContent = '/';
      bc.appendChild(sep);

      var seg = D.createElement('span');
      seg.className = 'crumb' + (acc === Z.state.path ? ' act' : '');
      seg.textContent = p;
      (function (cp) { seg.onclick = function () { explorer.nav(cp); }; })(acc);
      bc.appendChild(seg);
    }
  }

  /* ── Context menu ── */
  function showCtx(ev, entry, path, isDir) {
    var ctx = $('ctx-menu');
    if (!ctx) return;
    ctx.innerHTML = '';
    ctx.style.left = ev.clientX + 'px';
    ctx.style.top = ev.clientY + 'px';
    ctx.classList.add('on');

    function item(ico, label, red, fn) {
      var el = D.createElement('div');
      el.className = 'ci' + (red ? ' red' : '');
      el.innerHTML = '<span class="ci-i">' + ico + '</span><span class="ci-l">' + label + '</span>';
      el.onclick = function () { ctx.classList.remove('on'); fn(); };
      ctx.appendChild(el);
    }

    if (entry) {
      var sec = D.createElement('div');
      sec.className = 'c-sec';
      sec.textContent = entry.name;
      ctx.appendChild(sec);
      var sepEl = D.createElement('div');
      sepEl.className = 'c-sep';
      ctx.appendChild(sepEl);
    }

    if (!isDir && path) {
      item(ICO.download, 'Download', false, function () {
        window.location.href = Z.api.downloadUrl(path);
      });
    }
    if (path) {
      item(ICO.edit, 'Rename', false, function () { doRename(path); });
      item(ICO.sendTo, 'Send To\u2026', false, function () { doSendTo(entry, path); });

      /* Extract option for archives */
      if (entry && !isDir) {
        var ext = Z.extname(entry.name);
        if (['zip', 'tar', 'gz', 'bz2', 'xz', '7z', 'rar'].indexOf(ext) >= 0) {
          item(ICO.extractBox, 'Extract Here', false, function () { doExtract(path, Z.state.path); });
        }
      }

      item(ICO.trash, 'Delete', true, function () { doDelete(path, false); });
      if (isDir) {
        item(ICO.trash, 'Delete (recursive)', true, function () { doDelete(path, true); });
      }
    }

    if (!entry) {
      /* Background context menu */
      item(ICO.newFile, 'New File', false, function () { doCreateFile(); });
      item(ICO.newFolder, 'New Folder', false, function () { doCreateDir(); });
      item(ICO.refresh, 'Refresh', false, function () { explorer.nav(Z.state.path); });
    }

    /* Dismiss on click outside */
    setTimeout(function () {
      D.addEventListener('click', function dismiss() {
        ctx.classList.remove('on');
        D.removeEventListener('click', dismiss);
      }, { once: true });
    }, 0);
  }

  /* ── File operations ── */
  function doRename(path) {
    Z.modal.prompt('Rename', Z.basename(path)).then(function (name) {
      if (!name) return;
      Z.api.rename(path, name).then(function () {
        Z.toast('Renamed', 'ok');
        explorer.nav(Z.state.path);
      }).catch(function (e) { Z.toast('Rename failed: ' + e.message, 'er'); });
    });
  }

  function doDelete(path, recursive) {
    var msg = path + (recursive ? ' (recursive)' : '');
    Z.modal.confirm('Delete', msg, true).then(function (ok) {
      if (!ok) return;
      Z.api.del(path, recursive).then(function () {
        Z.toast('Deleted', 'ok');
        explorer.nav(Z.state.path);
      }).catch(function (e) { Z.toast('Delete failed: ' + e.message, 'er'); });
    });
  }

  function doCreateFile() {
    Z.modal.prompt('New File', '').then(function (name) {
      if (!name) return;
      Z.api.createFile(Z.state.path, name).then(function () {
        Z.toast('Created', 'ok');
        explorer.nav(Z.state.path);
      }).catch(function (e) { Z.toast('Failed: ' + e.message, 'er'); });
    });
  }

  function doCreateDir() {
    Z.modal.prompt('New Folder', '').then(function (name) {
      if (!name) return;
      Z.api.mkdir(Z.state.path, name).then(function () {
        Z.toast('Created', 'ok');
        explorer.nav(Z.state.path);
      }).catch(function (e) { Z.toast('Failed: ' + e.message, 'er'); });
    });
  }

  function doSendTo(entry, srcPath) {
    if (!Z.ensureTransferIdle()) return;
    Z.modal.folderPicker('Send To…', Z.state.path).then(function (dst) {
      if (dst === null) return;
      if (!dst) dst = '/';

      var cancelled = false;
      Z.showTransferLock({
        label: 'COPYING',
        filename: entry ? entry.name : Z.basename(srcPath),
        dest: dst,
        onCancel: function () {
          cancelled = true;
          Z.api.copyCancel().catch(function(){});
          Z.hideTransferLock();
          Z.notify('Copy cancelled', 'Copy to ' + dst + ' aborted.', 'wn');
        }
      });

      var startTime = Date.now();
      var progressInterval = setInterval(function() {
         var elapsed = Math.floor((Date.now() - startTime) / 1000);
         Z.updateTransferLock({ elapsed: elapsed + 's' });
      }, 1000);

      Z.api.copy(srcPath, dst, entry ? entry.size : 0).then(function () {
        clearInterval(progressInterval);
        if (!cancelled) {
          Z.hideTransferLock();
          Z.notify('Copied', 'Successfully copied to ' + dst, 'ok');
          explorer.nav(Z.state.path);
        }
      }).catch(function (e) {
        clearInterval(progressInterval);
        Z.hideTransferLock();
        Z.notify('Copy failed', e.message, 'er');
      });
    });
  }

  function doExtract(archivePath, dstDir) {
    if (!Z.ensureTransferIdle()) return;
    Z.modal.folderPicker('Extract to…', dstDir).then(function (dst) {
      if (dst === null) return;
      if (!dst) dst = dstDir;

      var cancelled = false;
      Z.showTransferLock({
        label: 'EXTRACTING',
        filename: Z.basename(archivePath),
        dest: dst,
        onCancel: function () {
          cancelled = true;
          Z.api.extractCancel().catch(function(){});
          Z.hideTransferLock();
          Z.notify('Extraction cancelled', 'Extract to ' + dst + ' aborted.', 'wn');
        }
      });

      var startTime = Date.now();
      var progressInterval = setInterval(function() {
         var elapsed = Math.floor((Date.now() - startTime) / 1000);
         Z.updateTransferLock({ elapsed: elapsed + 's' });
         /* If your API supports extract progress, poll it here */
         Z.api.extractProgress().then(function(d) {
           if (d && typeof d.progress === 'number') {
             Z.updateTransferLock({ pct: d.progress });
           }
         }).catch(function(){});
      }, 1000);

      Z.api.extract(archivePath, dst).then(function () {
        clearInterval(progressInterval);
        if (!cancelled) {
          Z.hideTransferLock();
          Z.notify('Extracted', 'Successfully extracted to ' + dst, 'ok');
          explorer.nav(Z.state.path);
        }
      }).catch(function (e) {
        clearInterval(progressInterval);
        Z.hideTransferLock();
        Z.notify('Extract failed', e.message, 'er');
      });
    });
  }

  /* ── Upload — with transfer lock modal ── */
  explorer.upload = function (files) {
    if (!files || !files.length) return;
    if (!Z.ensureTransferIdle()) return;

    var fileIdx = 0;
    var cancelled = false;
    var currentXhr = null;

    function uploadNext() {
      if (cancelled || fileIdx >= files.length) {
        Z.hideTransferLock();
        if (!cancelled) {
          explorer.nav(Z.state.path);
          Z.notify('Upload complete', files.length + ' file(s) uploaded', 'ok');
        }
        return;
      }
      var f = files[fileIdx++];
      var startTime = Date.now();

      Z.showTransferLock({
        label: 'UPLOADING',
        filename: f.name,
        dest: Z.state.path,
        onCancel: function () {
          cancelled = true;
          if (currentXhr) currentXhr.abort();
          Z.hideTransferLock();
          Z.notify('Upload cancelled', f.name, 'wn');
        }
      });

      var prom = Z.api.upload(Z.state.path, f, function (pct, loaded, total) {
        var elapsed = Math.floor((Date.now() - startTime) / 1000);
        var speedBps = elapsed > 0 ? loaded / elapsed : 0;
        Z.updateTransferLock({
          pct: pct,
          speed: Z.bytes(speedBps) + '/s',
          elapsed: elapsed + 's'
        });
      });
      /* Capture the XHR handle for abort */
      if (prom._xhr) currentXhr = prom._xhr;

      prom.then(function () {
        uploadNext();
      }).catch(function (e) {
        Z.hideTransferLock();
        Z.notify('Upload failed', f.name + ': ' + e.message, 'er');
      });
    }

    uploadNext();
  };

  /* ── View switching ── */
  explorer.setView = function (v) {
    if (!{ grid: 1, list: 1, details: 1 }[v]) return;
    _view = v;
    ['grid', 'list', 'details'].forEach(function (x) {
      var b = $('vb-' + x);
      if (b) b.classList.toggle('active', x === v);
    });
    render($('search') ? $('search').value : '');
    try { localStorage.setItem('zftpd_explorer_view', v); } catch (e) { }
  };

  /* ── Helper updates ── */
  function updatePath() {
    var el = $('current-path');
    if (el) el.textContent = Z.state.path;
  }

  function updateStatus(ok) {
    var pill = $('status');
    if (!pill) return;
    pill.className = 'status-pill ' + (ok ? 'status-ok' : 'status-bad');
    var dot = pill.querySelector('.sdot');
    var txt = pill.querySelector('.stxt');
    if (txt) txt.textContent = ok ? 'Connected' : 'Error';
  }

  function updateCount() {
    var el = $('fl-count');
    if (el) el.innerHTML = '<b>' + (Z.state.entries ? Z.state.entries.length : 0) + '</b> items';
  }

  /* ── Init (called when view becomes active) ── */
  explorer.init = function () {
    try {
      var sv = localStorage.getItem('zftpd_explorer_view');
      if (sv) _view = sv;
    } catch (e) { }

    /* Wire toolbar buttons */
    var bu = $('btn-up');
    if (bu) bu.onclick = function () { var p = Z.parent(Z.state.path); if (p !== null) explorer.nav(p); };
    var br = $('btn-ref');
    if (br) br.onclick = function () { explorer.nav(Z.state.path); };
    var sr = $('search');
    if (sr) sr.oninput = Z.debounce(function () { render(sr.value); }, 150);
    var fi = $('file-input');
    if (fi) fi.onchange = function (e) { explorer.upload(e.target.files); e.target.value = ''; };

    ['grid', 'list', 'details'].forEach(function (v) {
      var b = $('vb-' + v);
      if (b) b.onclick = function () { explorer.setView(v); };
    });

    explorer.setView(_view);
  };

  Z.explorer = explorer;

})(ZFTPD);
