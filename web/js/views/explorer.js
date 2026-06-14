/* ══ FILE EXPLORER VIEW ═══════════════════════════════════════════════════
 * Professional file browser with selection, media preview, inspector,
 * filtering, sorting, upload, copy, extraction, and batch operations.
 * ES5 compatible for PS5 browser.
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var D = document;
  var $ = Z.$;
  var ICO = Z.ICO;

  var explorer = {};
  var _view = 'grid'; /* grid | list | details */
  var _sortKey = 'name';
  var _sortAsc = true;
  var _filter = 'all';
  var _selected = {};
  var _selectedOrder = [];
  var _visibleEntries = [];
  var _lastClickedIndex = -1;
  var _viewInitialized = false;
  var _previewOpen = true;
  var _textPreviewReq = 0;
  var _keysWired = false;

  function isMobileViewport() {
    return !!(window.matchMedia && window.matchMedia('(max-width: 768px)').matches);
  }

  function syncPreviewButton() {
    var btn = $('btn-inspector');
    if (btn) btn.classList.toggle('active', _previewOpen);
  }

  /* ── Navigation ── */
  explorer.nav = function (path) {
    if (!_viewInitialized) _viewInitialized = true;
    if (Z.state.transferActive) {
      Z.toast('Transfer in progress\u2026', 'wn');
      return;
    }

    Z.state.path = Z.norm(path);
    try { localStorage.setItem('zftpd_path', Z.state.path); } catch (e) { }
    if (Z.updateTopbarContext) Z.updateTopbarContext();

    clearSelection(true);
    updatePath();
    renderBreadcrumb();
    renderPreview();

    var fl = $('file-list');
    if (fl) {
      fl.innerHTML = '<div class="s-card"><div class="loader"></div><div>Loading\u2026</div></div>';
    }

    Z.api.list(Z.state.path).then(function (d) {
      Z.state.entries = (d && Array.isArray(d.entries)) ? d.entries : [];
      sortEntries();
      render($('search') ? $('search').value : '');
      updateStatus(true);
    }).catch(function () {
      if (fl) {
        fl.innerHTML = '<div class="s-card s-err"><div class="s-ico">' + ICO.alert + '</div><div>Failed to load directory</div></div>';
      }
      updateStatus(false);
    });
  };

  /* ── Rendering ── */
  function render(query) {
    var fl = $('file-list');
    if (!fl) return;
    fl.innerHTML = '';
    fl.className = 'fl vg-' + _view;

    query = (query || '').trim().toLowerCase();
    var entries = getFilteredEntries(query);
    _visibleEntries = entries;
    syncSelection(entries);
    updateCount(entries);
    updateExplorerMetrics();
    updateActionButtons();

    if (!entries.length) {
      var empty = query || _filter !== 'all' ? 'No matching files' : 'Empty directory';
      fl.innerHTML = '<div class="s-card"><div class="s-ico">' + ICO.folder + '</div><div>' + empty + '</div></div>';
      renderPreview();
      return;
    }

    if (_view === 'details') {
      renderDetails(fl, entries);
    } else {
      for (var i = 0; i < entries.length; i++) {
        var entry = entries[i];
        var isDir = entry.type === 'directory';
        var path = entryPath(entry);
        var cat = Z.fileCategory(entry.name, isDir);
        var card = createCard(entry, path, isDir, cat, i);
        fl.appendChild(card);
      }
    }
    applySelectionState();
  }

  function getFilteredEntries(query) {
    var entries = Z.state.entries || [];
    var out = [];
    for (var i = 0; i < entries.length; i++) {
      var x = entries[i];
      var isDir = x.type === 'directory';
      var settings = Z.settings || {};
      if (isDir && !settings.showSystemFolders && Z.isDangerousFolder && Z.isDangerousFolder(x.name)) continue;
      if (!settings.showHiddenFiles && Z.isHiddenFile && Z.isHiddenFile(x.name)) continue;
      if (query && String(x.name || '').toLowerCase().indexOf(query) < 0) continue;
      if (!matchesFilter(x, isDir)) continue;
      out.push(x);
    }
    return out;
  }

  function matchesFilter(entry, isDir) {
    if (_filter === 'all') return true;
    var cat = Z.fileCategory(entry.name, isDir);
    if (_filter === 'media') return cat === 'img' || cat === 'video' || cat === 'audio';
    if (_filter === 'docs') return cat === 'doc' || cat === 'sheet' || cat === 'slide' || cat === 'data' || cat === 'code' || cat === 'web' || cat === 'style' || cat === 'script' || cat === 'log';
    if (_filter === 'archives') return cat === 'arch';
    if (_filter === 'games') return cat === 'game';
    return true;
  }

  function createCard(entry, path, isDir, cat, idx) {
    var card = D.createElement('div');
    card.className = 'card cat-' + cat;
    card.setAttribute('data-path', path);
    card.setAttribute('data-dir', isDir ? '1' : '0');
    card.setAttribute('data-name', entry.name);
    card.tabIndex = 0;

    var iconHtml = iconFor(entry.name, isDir);
    var iconClass = 'fi-' + cat;
    var ext = Z.extname(entry.name);
    var sizeStr = isDir ? '' : Z.bytes(entry.size || 0);

    if (_view === 'grid') {
      card.innerHTML =
        '<div class="select-mark">' + ICO.check + '</div>' +
        '<div class="c-ico"><div class="fi-wrap ' + iconClass + '">' + iconHtml + '</div></div>' +
        '<div class="c-name"></div>' +
        '<div class="c-meta">' +
          (ext && !isDir ? '<span class="xb">' + Z.h(ext) + '</span>' : '') +
          (sizeStr ? '<span class="sb">' + Z.h(sizeStr) + '</span>' : '') +
        '</div>';
    } else {
      card.innerHTML =
        '<div class="select-mark">' + ICO.check + '</div>' +
        '<div class="c-ico"><div class="fi-wrap ' + iconClass + '">' + iconHtml + '</div></div>' +
        '<div class="c-name"></div>' +
        '<div class="c-right">' +
          (ext && !isDir ? '<span class="xb">' + Z.h(ext) + '</span>' : '') +
          (sizeStr ? '<span class="sb">' + Z.h(sizeStr) + '</span>' : '') +
        '</div>';
    }
    var nm = card.querySelector('.c-name');
    if (nm) {
      nm.textContent = entry.name;
      nm.title = entry.name;
    }

    card.onclick = function (ev) { handleEntryClick(ev, entry, path, idx); };
    card.ondblclick = function (ev) {
      ev.preventDefault();
      openEntry(entry, path, isDir);
    };
    card.onkeydown = function (ev) {
      if (ev.key === 'Enter') openEntry(entry, path, isDir);
      if (ev.key === ' ') {
        ev.preventDefault();
        handleEntryClick(ev, entry, path, idx);
      }
    };
    card.addEventListener('contextmenu', function (ev) {
      ev.preventDefault();
      ev.stopPropagation();
      if (!_selected[path]) selectOnly(path);
      showCtx(ev, entry, path, isDir);
    });

    return card;
  }

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

    var ths = thead.querySelectorAll('th[data-sort]');
    for (var h = 0; h < ths.length; h++) {
      (function (th) {
        var key = th.getAttribute('data-sort');
        if (key === _sortKey) {
          th.innerHTML = th.innerHTML + '<span class="sort-a">' + (_sortAsc ? '\u2193' : '\u2191') + '</span>';
        }
        th.onclick = function () {
          setSort(key, _sortKey === key ? !_sortAsc : true);
        };
      })(ths[h]);
    }

    var tbody = D.createElement('tbody');
    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      var isDir = e.type === 'directory';
      var path = entryPath(e);
      var cat = Z.fileCategory(e.name, isDir);
      var ext = Z.extname(e.name);

      var tr = D.createElement('tr');
      tr.setAttribute('data-path', path);
      tr.setAttribute('data-is-dir', isDir ? '1' : '0');
      tr.innerHTML =
        '<td class="t-ic"><span class="fi-' + cat + '">' + iconFor(e.name, isDir) + '</span></td>' +
        '<td class="t-nm"></td>' +
        '<td class="t-ex">' + (isDir ? 'Folder' : (ext ? '<span class="xb">' + Z.h(ext) + '</span>' : '\u2014')) + '</td>' +
        '<td class="t-sz">' + (isDir ? '\u2014' : Z.h(Z.bytes(e.size || 0))) + '</td>' +
        '<td class="t-dt">' + (e.mtime ? Z.h(Z.relativeTime(e.mtime)) : '\u2014') + '</td>';

      var nameCell = tr.querySelector('.t-nm');
      if (nameCell) {
        nameCell.textContent = e.name;
        nameCell.title = e.name;
      }

      (function (entry, rowPath, dir, idx, row) {
        row.onclick = function (ev) { handleEntryClick(ev, entry, rowPath, idx); };
        row.ondblclick = function (ev) {
          ev.preventDefault();
          openEntry(entry, rowPath, dir);
        };
        row.addEventListener('contextmenu', function (ev) {
          ev.preventDefault();
          ev.stopPropagation();
          if (!_selected[rowPath]) selectOnly(rowPath);
          showCtx(ev, entry, rowPath, dir);
        });
      })(e, path, isDir, i, tr);

      tbody.appendChild(tr);
    }
    tbl.appendChild(tbody);
    wrap.appendChild(tbl);
    fl.appendChild(wrap);
  }

  function iconFor(name, isDir) {
    var kind = Z.mediaKind(name, isDir);
    if (kind === 'folder') return ICO.folder;
    if (kind === 'image') return ICO.image || ICO.file;
    if (kind === 'video') return ICO.film || ICO.file;
    if (kind === 'audio') return ICO.music || ICO.file;
    if (kind === 'text' || kind === 'pdf') return ICO.fileText || ICO.file;
    if (kind === 'archive') return ICO.archive || ICO.file;
    if (kind === 'game') return ICO.gamepad || ICO.file;
    return ICO.file;
  }

  /* ── Selection ── */
  function handleEntryClick(ev, entry, path, idx) {
    if (isMobileViewport() && !ev.ctrlKey && !ev.metaKey && !ev.shiftKey) {
      if (entry.type === 'directory') {
        openEntry(entry, path, true);
        return;
      }
      selectOnly(path);
      _lastClickedIndex = idx;
      _previewOpen = true;
      syncPreviewButton();
      applySelectionState();
      return;
    }

    var multi = ev.ctrlKey || ev.metaKey;
    var range = ev.shiftKey && _lastClickedIndex >= 0;

    if (range) {
      if (!multi) clearSelection(true);
      selectRange(_lastClickedIndex, idx);
    } else if (multi) {
      toggleSelect(path);
      _lastClickedIndex = idx;
    } else {
      selectOnly(path);
      _lastClickedIndex = idx;
    }
    applySelectionState();
  }

  function selectOnly(path) {
    _selected = {};
    _selectedOrder = [];
    addSelection(path);
    applySelectionState();
  }

  function toggleSelect(path) {
    if (_selected[path]) removeSelection(path);
    else addSelection(path);
  }

  function addSelection(path) {
    if (_selected[path]) return;
    _selected[path] = true;
    _selectedOrder.push(path);
  }

  function removeSelection(path) {
    if (!_selected[path]) return;
    delete _selected[path];
    var next = [];
    for (var i = 0; i < _selectedOrder.length; i++) {
      if (_selectedOrder[i] !== path) next.push(_selectedOrder[i]);
    }
    _selectedOrder = next;
  }

  function selectRange(a, b) {
    var start = Math.min(a, b);
    var end = Math.max(a, b);
    for (var i = start; i <= end && i < _visibleEntries.length; i++) {
      addSelection(entryPath(_visibleEntries[i]));
    }
  }

  function selectAllVisible() {
    clearSelection(true);
    for (var i = 0; i < _visibleEntries.length; i++) addSelection(entryPath(_visibleEntries[i]));
    applySelectionState();
  }

  function clearSelection(skipApply) {
    _selected = {};
    _selectedOrder = [];
    _lastClickedIndex = -1;
    if (!skipApply) applySelectionState();
  }

  function syncSelection(entries) {
    var valid = {};
    for (var i = 0; i < entries.length; i++) valid[entryPath(entries[i])] = true;
    var next = [];
    for (var j = 0; j < _selectedOrder.length; j++) {
      if (valid[_selectedOrder[j]]) next.push(_selectedOrder[j]);
      else delete _selected[_selectedOrder[j]];
    }
    _selectedOrder = next;
  }

  function selectedItems() {
    var items = [];
    var map = {};
    var entries = Z.state.entries || [];
    for (var i = 0; i < entries.length; i++) map[entryPath(entries[i])] = entries[i];
    for (var j = 0; j < _selectedOrder.length; j++) {
      var path = _selectedOrder[j];
      if (map[path]) items.push({ entry: map[path], path: path, isDir: map[path].type === 'directory' });
    }
    return items;
  }

  function applySelectionState() {
    var nodes = D.querySelectorAll('#file-list [data-path]');
    for (var i = 0; i < nodes.length; i++) {
      var p = nodes[i].getAttribute('data-path');
      nodes[i].classList.toggle('selected', !!_selected[p]);
    }
    updateActionButtons();
    renderPreview();
  }

  /* ── Actions ── */
  function openSelected() {
    var items = selectedItems();
    if (!items.length) return;
    if (items.length > 1) {
      downloadSelected();
      return;
    }
    openEntry(items[0].entry, items[0].path, items[0].isDir);
  }

  function openEntry(entry, path, isDir) {
    if (isDir) {
      explorer.nav(path);
      return;
    }
    if (Z.isPreviewable(entry.name, false, entry.size || 0)) {
      showPreviewModal(entry, path);
      return;
    }
    Z.download(Z.api.downloadUrl(path));
  }

  function installPackage(path, reinstall) {
    if (!path || !Z.api) return;
    if (!Z.featureEnabled || !Z.featureEnabled('pkgInstall')) {
      Z.toast('PKG installation is disabled for now. Remote app launch remains available in Games.', 'wn');
      return;
    }
    var action = reinstall ? 'Reinstall' : 'Install';
    var fn = reinstall ? Z.api.gameReinstall : Z.api.gameInstall;
    Z.modal.confirm(action + ' package', Z.basename(path), false).then(function (ok) {
      if (!ok) return;
      fn(path).then(function (r) {
        var good = !!(r && r.ok !== false);
        Z.notify(action + ' queued', path, good ? 'ok' : 'wn');
        if (Z.gamesView && Z.gamesView.refreshStatus) Z.gamesView.refreshStatus();
      }).catch(function (e) {
        Z.notify(action + ' failed', (e && e.message) ? e.message : path, 'er');
      });
    });
  }

  function downloadSelected() {
    var items = selectedItems();
    var files = [];
    for (var i = 0; i < items.length; i++) {
      if (!items[i].isDir) files.push(items[i]);
    }
    if (!files.length) {
      Z.toast('Select one or more files', 'wn');
      return;
    }
    for (var j = 0; j < files.length; j++) {
      (function (it, delay) {
        setTimeout(function () { Z.download(Z.api.downloadUrl(it.path)); }, delay);
      })(files[j], j * 160);
    }
    Z.notify('Download started', files.length + ' file(s)', 'ok');
  }

  function sendSelected() {
    if (!Z.ensureTransferIdle()) return;
    var items = selectedItems();
    if (!items.length) {
      Z.toast('Select items first', 'wn');
      return;
    }
    Z.modal.folderPicker('Send To\u2026', '/').then(function (dst) {
      if (dst === null) return;
      if (!dst) dst = '/';
      copyItems(items, dst);
    });
  }

  function copyItems(items, dst) {
    var idx = 0;
    var cancelled = false;
    var progressTimer = null;
    var doneItems = 0;

    function stopPolling() {
      if (progressTimer) {
        clearInterval(progressTimer);
        progressTimer = null;
      }
    }

    function finish(ok) {
      stopPolling();
      Z.hideTransferLock();
      if (ok && !cancelled) Z.notify('Copy complete', doneItems + ' item(s) copied to ' + dst, 'ok');
      explorer.nav(Z.state.path);
    }

    function next() {
      if (cancelled) {
        finish(false);
        return;
      }
      if (idx >= items.length) {
        finish(true);
        return;
      }

      var item = items[idx++];
      Z.showTransferLock({
        label: 'COPYING (' + idx + '/' + items.length + ')',
        filename: item.entry.name,
        dest: dst,
        onCancel: function () {
          cancelled = true;
          Z.api.copyCancel().catch(function () {});
          finish(false);
          Z.notify('Copy cancelled', item.entry.name, 'wn');
        }
      });

      var started = Date.now();
      var prevBytes = 0;
      var prevTime = Date.now();
      stopPolling();
      Z.api.copy(item.path, dst, item.entry.size || 0).then(function (resp) {
        if (!resp || !resp.async) {
          doneItems++;
          Z.updateTransferLock({ pct: 100 });
          next();
          return;
        }
        progressTimer = setInterval(function () {
          Z.api.copyProgress().then(function (p) {
            if (cancelled) return;
            var elapsed = Math.max(1, Math.floor((Date.now() - started) / 1000));
            if (p && p.active && !p.done) {
              var pct = p.total_bytes > 0 ? Math.min(100, Math.round(p.bytes_copied * 100 / p.total_bytes)) : 0;
              var now = Date.now();
              var dt = (now - prevTime) / 1000;
              var instantSpeed = dt > 0 ? (p.bytes_copied - prevBytes) / dt : 0;
              prevBytes = p.bytes_copied;
              prevTime = now;
              Z.updateTransferLock({
                pct: pct,
                speed: instantSpeed > 0 ? Z.bps(instantSpeed) : '',
                elapsed: elapsed + 's'
              });
            } else if (p && p.done) {
              stopPolling();
              if (p.error) {
                Z.hideTransferLock();
                Z.notify('Copy failed', p.error_msg || item.entry.name, 'er');
                explorer.nav(Z.state.path);
              } else {
                doneItems++;
                next();
              }
            }
          }).catch(function () {});
        }, 500);
      }).catch(function (e) {
        Z.hideTransferLock();
        Z.notify('Copy failed', e.message, 'er');
        explorer.nav(Z.state.path);
      });
    }

    next();
  }

  function deleteSelected() {
    var items = selectedItems();
    if (!items.length) {
      Z.toast('Select items first', 'wn');
      return;
    }
    var label = items.length === 1 ? items[0].path : (items.length + ' selected items');
    Z.modal.confirm('Delete', label, true).then(function (ok) {
      if (!ok) return;
      var i = 0;
      function next() {
        if (i >= items.length) {
          Z.notify('Deleted', items.length + ' item(s)', 'ok');
          clearSelection(true);
          explorer.nav(Z.state.path);
          return;
        }
        var item = items[i++];
        Z.api.del(item.path, item.isDir).then(next).catch(function (e) {
          Z.notify('Delete failed', item.entry.name + ': ' + e.message, 'er');
          next();
        });
      }
      next();
    });
  }

  function renameSelected() {
    var items = selectedItems();
    if (items.length !== 1) {
      Z.toast('Select one item to rename', 'wn');
      return;
    }
    doRename(items[0].path);
  }

  function copySelectedPath() {
    var items = selectedItems();
    if (!items.length) return;
    var text = items.map(function (it) { return it.path; }).join('\n');
    Z.copyText(text).then(function () {
      Z.notify('Path copied', items.length + ' item(s)', 'ok');
    }).catch(function () {
      Z.modal.prompt('Copy path', text);
    });
  }

  function doRename(path) {
    Z.modal.prompt('Rename', Z.basename(path)).then(function (name) {
      if (!name) return;
      Z.api.rename(path, name).then(function () {
        Z.notify('Renamed', name, 'ok');
        explorer.nav(Z.state.path);
      }).catch(function (e) { Z.toast('Rename failed: ' + e.message, 'er'); });
    });
  }

  function doDelete(path, recursive) {
    var msg = path + (recursive ? ' (recursive)' : '');
    Z.modal.confirm('Delete', msg, true).then(function (ok) {
      if (!ok) return;
      Z.api.del(path, recursive).then(function () {
        Z.notify('Deleted', path, 'ok');
        explorer.nav(Z.state.path);
      }).catch(function (e) { Z.toast('Delete failed: ' + e.message, 'er'); });
    });
  }

  function doCreateFile() {
    Z.modal.prompt('New File', '').then(function (name) {
      if (!name) return;
      Z.api.createFile(Z.state.path, name).then(function () {
        Z.notify('Created', name, 'ok');
        explorer.nav(Z.state.path);
      }).catch(function (e) { Z.toast('Failed: ' + e.message, 'er'); });
    });
  }

  function doCreateDir() {
    Z.modal.prompt('New Folder', '').then(function (name) {
      if (!name) return;
      Z.api.mkdir(Z.state.path, name).then(function () {
        Z.notify('Created', name, 'ok');
        explorer.nav(Z.state.path);
      }).catch(function (e) { Z.toast('Failed: ' + e.message, 'er'); });
    });
  }

  function doExtract(archivePath, dstDir) {
    if (!Z.ensureTransferIdle()) return;
    Z.modal.folderPicker('Extract to\u2026', dstDir).then(function (dst) {
      if (dst === null) return;
      if (!dst) dst = dstDir;

      var cancelled = false;
      var progressTimer = null;

      Z.showTransferLock({
        label: 'EXTRACTING',
        filename: Z.basename(archivePath),
        dest: dst,
        onCancel: function () {
          cancelled = true;
          Z.api.extractCancel().catch(function () {});
          Z.hideTransferLock();
          if (progressTimer) clearInterval(progressTimer);
          Z.notify('Extraction cancelled', 'Extract to ' + dst + ' aborted.', 'wn');
        }
      });

      var started = Date.now();
      Z.api.extract(archivePath, dst).then(function (resp) {
        if (!resp || !resp.async) {
          Z.hideTransferLock();
          Z.notify('Extracted', 'Successfully extracted to ' + dst, 'ok');
          explorer.nav(Z.state.path);
          return;
        }

        progressTimer = setInterval(function () {
          Z.api.extractProgress().then(function (d) {
            if (cancelled) return;
            var elapsed = Math.floor((Date.now() - started) / 1000);
            if (d && d.active && !d.done) {
              Z.updateTransferLock({
                pct: typeof d.progress === 'number' ? d.progress : 0,
                elapsed: elapsed + 's'
              });
            } else if (d && d.done) {
              clearInterval(progressTimer);
              Z.hideTransferLock();
              if (d.error) Z.notify('Extract failed', d.error_msg || 'error', 'er');
              else Z.notify('Extracted', 'Successfully extracted to ' + dst, 'ok');
              explorer.nav(Z.state.path);
            }
          }).catch(function () {});
        }, 1000);
      }).catch(function (e) {
        if (progressTimer) clearInterval(progressTimer);
        Z.hideTransferLock();
        Z.notify('Extract failed', e.message, 'er');
      });
    });
  }

  /* ── Preview / Inspector ── */
  function renderPreview() {
    var pane = $('preview-pane');
    if (!pane) return;
    pane.classList.toggle('collapsed', !_previewOpen);
    var shell = pane.parentNode;
    if (shell) shell.classList.toggle('inspector-collapsed', !_previewOpen);
    if (!_previewOpen) return;

    var items = selectedItems();
    if (!items.length) {
      renderDirectorySummary(pane);
      return;
    }
    if (items.length > 1) {
      renderBulkPreview(pane, items);
      return;
    }
    renderEntryPreview(pane, items[0]);
  }

  function previewHeader(title, subtitle) {
    return '<div class="preview-head">' +
      '<div><div class="preview-kicker">Inspector</div><div class="preview-title">' + Z.h(title) + '</div>' +
      (subtitle ? '<div class="preview-sub">' + Z.h(subtitle) + '</div>' : '') + '</div>' +
      '<button id="preview-close" class="preview-close" title="Hide inspector">' + ICO.x + '</button>' +
      '</div>';
  }

  function bindPreviewClose() {
    var close = $('preview-close');
    if (close) close.onclick = function () { togglePreview(false); };
  }

  function renderDirectorySummary(pane) {
    var counts = summarizeEntries(Z.state.entries || []);
    pane.innerHTML = previewHeader('Current folder', Z.state.path) +
      '<div class="preview-body">' +
        '<div class="preview-hero folder">' + ICO.folderOpen + '</div>' +
        '<div class="metric-grid">' +
          metric('Folders', counts.dirs) +
          metric('Files', counts.files) +
          metric('Media', counts.media) +
          metric('Size', Z.bytes(counts.size)) +
        '</div>' +
        '<div class="preview-actions">' +
          '<button id="pv-select-all" class="btn">' + ICO.check + ' Select All</button>' +
          '<button id="pv-refresh" class="btn">' + ICO.refresh + ' Refresh</button>' +
        '</div>' +
      '</div>';
    bindPreviewClose();
    var sa = $('pv-select-all');
    if (sa) sa.onclick = selectAllVisible;
    var rf = $('pv-refresh');
    if (rf) rf.onclick = function () { explorer.nav(Z.state.path); };
  }

  function renderBulkPreview(pane, items) {
    var size = 0;
    var dirs = 0;
    var media = 0;
    for (var i = 0; i < items.length; i++) {
      if (items[i].isDir) dirs++;
      else size += items[i].entry.size || 0;
      var cat = Z.fileCategory(items[i].entry.name, items[i].isDir);
      if (cat === 'img' || cat === 'video' || cat === 'audio') media++;
    }
    pane.innerHTML = previewHeader(items.length + ' selected', Z.state.path) +
      '<div class="preview-body">' +
        '<div class="preview-hero multi">' + ICO.check + '</div>' +
        '<div class="metric-grid">' +
          metric('Items', items.length) +
          metric('Folders', dirs) +
          metric('Media', media) +
          metric('Size', Z.bytes(size)) +
        '</div>' +
        '<div class="preview-actions vertical">' +
          '<button id="pv-download" class="btn primary">' + ICO.download + ' Download Files</button>' +
          '<button id="pv-send" class="btn">' + ICO.sendTo + ' Send To</button>' +
          '<button id="pv-copy-path" class="btn">' + ICO.clipboard + ' Copy Paths</button>' +
          '<button id="pv-delete" class="btn danger">' + ICO.trash + ' Delete</button>' +
        '</div>' +
      '</div>';
    bindPreviewClose();
    bindAction('pv-download', downloadSelected);
    bindAction('pv-send', sendSelected);
    bindAction('pv-copy-path', copySelectedPath);
    bindAction('pv-delete', deleteSelected);
  }

  function renderEntryPreview(pane, item) {
    var entry = item.entry;
    var path = item.path;
    var isDir = item.isDir;
    var kind = Z.mediaKind(entry.name, isDir);
    var url = Z.api.downloadUrl(path);
    var body = previewHeader(entry.name, Z.fileKind(entry.name, isDir)) +
      '<div class="preview-body">' +
        previewMedia(entry, path, kind, url) +
        (kind === 'game' && Z.isGamePackage(entry.name) && Z.featureEnabled && Z.featureEnabled('pkgInstall') ?
          '<div class="preview-actions">' +
            '<button id="pv-install-pkg" class="btn primary">' + (ICO.archive || ICO.file) + ' Install PKG</button>' +
            '<button id="pv-reinstall-pkg" class="btn">' + ICO.refresh + ' Reinstall</button>' +
            '<button id="pv-download-one" class="btn">' + ICO.download + ' Download</button>' +
          '</div>' :
          '<div class="preview-actions">' +
            '<button id="pv-open" class="btn primary">' + ICO.external + ' Open</button>' +
            (!isDir ? '<button id="pv-download-one" class="btn">' + ICO.download + ' Download</button>' : '') +
          '</div>') +
        '<div class="meta-list">' +
          meta('Name', entry.name) +
          meta('Type', Z.fileKind(entry.name, isDir)) +
          meta('Size', isDir ? 'Folder' : Z.bytes(entry.size || 0)) +
          meta('Modified', entry.mtime ? Z.relativeTime(entry.mtime) : 'Unavailable') +
          meta('Path', path) +
        '</div>' +
        '<div class="preview-actions vertical">' +
          '<button id="pv-rename" class="btn">' + ICO.edit + ' Rename</button>' +
          '<button id="pv-send-one" class="btn">' + ICO.sendTo + ' Send To</button>' +
          '<button id="pv-copy-path" class="btn">' + ICO.clipboard + ' Copy Path</button>' +
          '<button id="pv-delete-one" class="btn danger">' + ICO.trash + ' Delete</button>' +
        '</div>' +
      '</div>';

    pane.innerHTML = body;
    bindPreviewClose();
    bindAction('pv-open', function () { openEntry(entry, path, isDir); });
    bindAction('pv-install-pkg', function () { installPackage(path, false); });
    bindAction('pv-reinstall-pkg', function () { installPackage(path, true); });
    bindAction('pv-download-one', function () { Z.download(url); });
    bindAction('pv-rename', renameSelected);
    bindAction('pv-send-one', sendSelected);
    bindAction('pv-copy-path', copySelectedPath);
    bindAction('pv-delete-one', deleteSelected);

    if (isDir) bindDirSize(path);
    if (kind === 'text') loadTextPreview(path);
  }

  function previewMedia(entry, path, kind, url) {
    if (kind === 'folder') {
      return '<div class="preview-hero folder">' + ICO.folderOpen + '</div>' +
        '<button id="pv-dir-size" class="dir-size-btn">Calculate folder size</button>';
    }
    if (kind === 'image') {
      return '<div class="preview-media"><img class="preview-img" src="' + Z.h(url) + '" alt=""></div>';
    }
    if (kind === 'video') {
      return '<div class="preview-media"><video class="preview-video" controls preload="metadata" src="' + Z.h(url) + '"></video></div>';
    }
    if (kind === 'audio') {
      return '<div class="preview-audio">' + (ICO.music || ICO.file) + '<audio controls src="' + Z.h(url) + '"></audio></div>';
    }
    if (kind === 'pdf') {
      return '<iframe class="preview-frame" src="' + Z.h(url) + '" title=""></iframe>';
    }
    if (kind === 'text') {
      if ((entry.size || 0) > 2 * 1024 * 1024) {
        return '<div class="preview-hero doc">' + (ICO.fileText || ICO.file) + '</div>';
      }
      return '<pre id="text-preview" class="text-preview">Loading preview...</pre>';
    }
    if (kind === 'game') {
      return '<div class="preview-hero game">' + (ICO.gamepad || ICO.file) + '</div>';
    }
    if (kind === 'archive') {
      return '<div class="preview-hero archive">' + (ICO.archive || ICO.file) + '</div>';
    }
    return '<div class="preview-hero file">' + ICO.file + '</div>';
  }

  function showPreviewModal(entry, path) {
    var isDir = entry.type === 'directory';
    var kind = Z.mediaKind(entry.name, isDir);
    if (isDir) {
      explorer.nav(path);
      return;
    }
    var url = Z.api.downloadUrl(path);
    var html = '<div class="media-modal">' + previewMedia(entry, path, kind, url) + '</div>';
    var body = Z.modal.showHTML(entry.name, html);
    body.parentNode.style.width = 'min(980px,94vw)';
    body.style.maxHeight = '76vh';
    if (kind === 'text') {
      var pre = body.querySelector('#text-preview');
      fetch(url).then(function (r) { return r.text(); }).then(function (txt) {
        if (pre) pre.textContent = txt.slice(0, 80000);
      }).catch(function () {
        if (pre) pre.textContent = 'Preview unavailable';
      });
    }
  }

  function loadTextPreview(path) {
    var req = ++_textPreviewReq;
    var pre = $('text-preview');
    if (!pre) return;
    fetch(Z.api.downloadUrl(path)).then(function (r) { return r.text(); }).then(function (txt) {
      if (req !== _textPreviewReq) return;
      pre.textContent = txt.slice(0, 24000);
    }).catch(function () {
      if (req !== _textPreviewReq) return;
      pre.textContent = 'Preview unavailable';
    });
  }

  function bindDirSize(path) {
    var btn = $('pv-dir-size');
    if (!btn) return;
    btn.onclick = function () {
      btn.textContent = 'Calculating...';
      Z.api.dirsize(path).then(function (d) {
        var size = d && typeof d.size === 'number' ? d.size : (d && typeof d.bytes === 'number' ? d.bytes : 0);
        btn.textContent = Z.bytes(size);
      }).catch(function () {
        btn.textContent = 'Size unavailable';
      });
    };
  }

  function metric(label, value) {
    return '<div class="metric"><span>' + Z.h(label) + '</span><b>' + Z.h(value) + '</b></div>';
  }

  function meta(label, value) {
    return '<div class="meta-row"><span>' + Z.h(label) + '</span><b title="' + Z.h(value) + '">' + Z.h(value) + '</b></div>';
  }

  function bindAction(id, fn) {
    var el = $(id);
    if (el) el.onclick = fn;
  }

  function summarizeEntries(entries) {
    var s = { dirs: 0, files: 0, media: 0, archives: 0, games: 0, size: 0 };
    for (var i = 0; i < entries.length; i++) {
      var isDir = entries[i].type === 'directory';
      var cat = Z.fileCategory(entries[i].name, isDir);
      if (isDir) s.dirs++;
      else {
        s.files++;
        s.size += entries[i].size || 0;
      }
      if (cat === 'img' || cat === 'video' || cat === 'audio') s.media++;
      if (cat === 'arch') s.archives++;
      if (cat === 'game') s.games++;
    }
    return s;
  }

  function togglePreview(force) {
    _previewOpen = typeof force === 'boolean' ? force : !_previewOpen;
    try { localStorage.setItem('zftpd_preview_open', _previewOpen ? '1' : '0'); } catch (e) { }
    syncPreviewButton();
    renderPreview();
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
      el.innerHTML = '<span class="ci-i">' + ico + '</span><span class="ci-l">' + Z.h(label) + '</span>';
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

      item(ICO.external, isDir ? 'Open Folder' : 'Preview / Open', false, function () { openEntry(entry, path, isDir); });
      if (!isDir && Z.isGamePackage && Z.isGamePackage(entry.name) && Z.featureEnabled && Z.featureEnabled('pkgInstall')) {
        item(ICO.archive || ICO.file, 'Install PKG', false, function () { installPackage(path, false); });
        item(ICO.refresh, 'Reinstall PKG', false, function () { installPackage(path, true); });
      }
      if (!isDir) item(ICO.download, 'Download', false, function () { Z.download(Z.api.downloadUrl(path)); });
      item(ICO.edit, 'Rename', false, function () { doRename(path); });
      item(ICO.sendTo, 'Send To\u2026', false, sendSelected);
      item(ICO.clipboard, 'Copy Path', false, copySelectedPath);

      if (entry && !isDir) {
        var ext = Z.extname(entry.name);
        if (['zip', 'tar', 'gz', 'bz2', 'xz', '7z', 'rar'].indexOf(ext) >= 0) {
          item(ICO.extractBox, 'Extract Here', false, function () { doExtract(path, Z.state.path); });
        }
      }

      item(ICO.trash, isDir ? 'Delete (recursive)' : 'Delete', true, function () { doDelete(path, isDir); });
    } else {
      item(ICO.newFile, 'New File', false, doCreateFile);
      item(ICO.newFolder, 'New Folder', false, doCreateDir);
      item(ICO.check, 'Select All', false, selectAllVisible);
      item(ICO.refresh, 'Refresh', false, function () { explorer.nav(Z.state.path); });
    }

    setTimeout(function () {
      function dismiss() {
        ctx.classList.remove('on');
        D.removeEventListener('click', dismiss);
      }
      D.addEventListener('click', dismiss);
    }, 0);
  }

  /* ── Upload ── */
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
        label: 'UPLOADING (' + fileIdx + '/' + files.length + ')',
        filename: f.name,
        dest: Z.state.path,
        onCancel: function () {
          cancelled = true;
          if (currentXhr) currentXhr.abort();
          Z.hideTransferLock();
          Z.notify('Upload cancelled', f.name, 'wn');
        }
      });

      var prom = Z.api.upload(Z.state.path, f, function (pct, loaded) {
        var elapsed = Math.max(1, Math.floor((Date.now() - startTime) / 1000));
        var speedBps = loaded / elapsed;
        Z.updateTransferLock({
          pct: pct,
          speed: Z.bytes(speedBps) + '/s',
          elapsed: elapsed + 's'
        });
      });
      if (prom._xhr) currentXhr = prom._xhr;

      prom.then(uploadNext).catch(function (e) {
        Z.hideTransferLock();
        Z.notify('Upload failed', f.name + ': ' + e.message, 'er');
      });
    }

    uploadNext();
  };

  /* ── Sorting / Filters ── */
  function sortEntries() {
    var entries = Z.state.entries;
    if (!entries) return;
    entries.sort(function (a, b) {
      var da = a.type === 'directory' ? 0 : 1;
      var db = b.type === 'directory' ? 0 : 1;
      if (da !== db) return da - db;

      var va, vb;
      if (_sortKey === 'name') {
        va = (a.name || '').toLowerCase();
        vb = (b.name || '').toLowerCase();
        return _sortAsc ? compareText(va, vb) : compareText(vb, va);
      }
      if (_sortKey === 'size') {
        va = a.size || 0;
        vb = b.size || 0;
        return _sortAsc ? va - vb : vb - va;
      }
      if (_sortKey === 'ext') {
        va = Z.extname(a.name || '');
        vb = Z.extname(b.name || '');
        return _sortAsc ? compareText(va, vb) : compareText(vb, va);
      }
      if (_sortKey === 'mtime') {
        va = a.mtime || 0;
        vb = b.mtime || 0;
        return _sortAsc ? va - vb : vb - va;
      }
      return 0;
    });
  }

  function compareText(a, b) {
    return a < b ? -1 : (a > b ? 1 : 0);
  }

  function setSort(key, asc) {
    _sortKey = key;
    _sortAsc = asc;
    try {
      localStorage.setItem('zftpd_sort_key', _sortKey);
      localStorage.setItem('zftpd_sort_asc', _sortAsc ? '1' : '0');
    } catch (e) { }
    syncSortControls();
    sortEntries();
    render($('search') ? $('search').value : '');
  }

  function syncSortControls() {
    var sel = $('sort-select');
    if (sel) sel.value = _sortKey;
    var btn = $('sort-dir');
    if (btn) {
      btn.innerHTML = _sortAsc ? (ICO.sortAsc || '') : (ICO.sortDesc || '');
      btn.title = _sortAsc ? 'Ascending' : 'Descending';
    }
  }

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

  function setFilter(v) {
    _filter = v || 'all';
    var chips = D.querySelectorAll('#fl-filters .filter-chip');
    for (var i = 0; i < chips.length; i++) {
      chips[i].classList.toggle('active', chips[i].getAttribute('data-filter') === _filter);
    }
    clearSelection(true);
    render($('search') ? $('search').value : '');
    try { localStorage.setItem('zftpd_filter', _filter); } catch (e) { }
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

  /* ── Helper updates ── */
  function entryPath(entry) {
    return Z.join(Z.state.path, entry.name);
  }

  function updatePath() {
    var el = $('current-path');
    if (el) el.textContent = Z.state.path;
  }

  function updateStatus(ok) {
    var pill = $('status');
    if (!pill) return;
    pill.className = 'status-pill ' + (ok ? 'status-ok' : 'status-bad');
    var txt = pill.querySelector('.stxt');
    if (txt) txt.textContent = ok ? 'Connected' : 'Error';
  }

  function updateCount(entries) {
    var el = $('fl-count');
    if (!el) return;
    var all = Z.state.entries ? Z.state.entries.length : 0;
    var visible = entries ? entries.length : all;
    el.innerHTML = '<b>' + visible + '</b> shown <span class="muted">/ ' + all + ' total</span>';
    var sel = $('fl-selection');
    if (sel) {
      sel.textContent = _selectedOrder.length ? (_selectedOrder.length + ' selected') : '';
    }
  }

  function updateExplorerMetrics() {
    var stats = summarizeEntries(Z.state.entries || []);
    setMetric('mx-folders', stats.dirs);
    setMetric('mx-files', stats.files);
    setMetric('mx-media', stats.media);
    setMetric('mx-archives', stats.archives + stats.games);
    setMetric('mx-size', Z.bytes(stats.size));
  }

  function setMetric(id, value) {
    var el = $(id);
    if (el) el.textContent = value;
  }

  function updateActionButtons() {
    var count = _selectedOrder.length;
    var ids = ['btn-open-selected', 'btn-download-selected', 'btn-send-selected', 'btn-delete-selected'];
    for (var i = 0; i < ids.length; i++) {
      var b = $(ids[i]);
      if (b) b.disabled = !count;
    }
    var sel = $('fl-selection');
    if (sel) sel.textContent = count ? (count + ' selected') : '';
  }

  function wireKeyboard() {
    if (_keysWired) return;
    _keysWired = true;
    D.addEventListener('keydown', function (e) {
      if (Z.state.view !== 'explorer') return;
      var tag = e.target && e.target.tagName ? e.target.tagName.toLowerCase() : '';
      if (tag === 'input' || tag === 'textarea' || tag === 'select') return;
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a') {
        e.preventDefault();
        selectAllVisible();
      } else if (e.key === 'Escape') {
        clearSelection();
      } else if (e.key === 'Enter') {
        openSelected();
      } else if (e.key === 'Delete' || e.key === 'Backspace') {
        if (_selectedOrder.length) {
          e.preventDefault();
          deleteSelected();
        } else if (e.key === 'Backspace') {
          var p = Z.parent(Z.state.path);
          if (p !== null) explorer.nav(p);
        }
      }
    });
  }

  /* ── Init ── */
  explorer.init = function () {
    try {
      var sv = localStorage.getItem('zftpd_explorer_view');
      if (sv) _view = sv;
      else if (Z.settings && Z.settings.defaultView) _view = Z.settings.defaultView;

      var sk = localStorage.getItem('zftpd_sort_key');
      if (sk) _sortKey = sk;
      var sa = localStorage.getItem('zftpd_sort_asc');
      if (sa) _sortAsc = sa === '1';
      var sf = localStorage.getItem('zftpd_filter');
      if (sf) _filter = sf;
      var po = localStorage.getItem('zftpd_preview_open');
      if (po) _previewOpen = po === '1';
    } catch (e) { }
    if (isMobileViewport()) _previewOpen = false;
    _viewInitialized = true;

    var bu = $('btn-up');
    if (bu) bu.onclick = function () { var p = Z.parent(Z.state.path); if (p !== null) explorer.nav(p); };
    var br = $('btn-ref');
    if (br) br.onclick = function () { explorer.nav(Z.state.path); };
    var sr = $('search');
    if (sr) sr.oninput = Z.debounce(function () { render(sr.value); }, 150);
    var fi = $('file-input');
    if (fi) fi.onchange = function (e) { explorer.upload(e.target.files); e.target.value = ''; };

    bindAction('btn-open-selected', openSelected);
    bindAction('btn-download-selected', downloadSelected);
    bindAction('btn-send-selected', sendSelected);
    bindAction('btn-delete-selected', deleteSelected);
    bindAction('btn-inspector', function () { togglePreview(); });

    ['grid', 'list', 'details'].forEach(function (v) {
      var b = $('vb-' + v);
      if (b) b.onclick = function () { explorer.setView(v); };
    });

    var sortSel = $('sort-select');
    if (sortSel) sortSel.onchange = function () { setSort(this.value, _sortAsc); };
    var sortDir = $('sort-dir');
    if (sortDir) sortDir.onclick = function () { setSort(_sortKey, !_sortAsc); };
    syncSortControls();

    var chips = D.querySelectorAll('#fl-filters .filter-chip');
    for (var i = 0; i < chips.length; i++) {
      (function (chip) {
        chip.onclick = function () { setFilter(chip.getAttribute('data-filter')); };
      })(chips[i]);
    }
    setFilter(_filter);

    syncPreviewButton();
    wireKeyboard();
    explorer.setView(_view);
  };

  Z.explorer = explorer;

})(ZFTPD);
