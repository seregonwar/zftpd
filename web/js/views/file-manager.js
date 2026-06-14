/* ══ FILE MANAGER VIEW - PROFESSIONAL DUAL PANE CLIENT ═══════════════════
 * WinSCP/FileZilla inspired source/destination workspace.
 * ES5 compatible for PS5 browser.
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var D = document;
  var $ = Z.$;
  var ICO = Z.ICO;

  var fm = {};
  var _wired = false;
  var _leftPath = '/';
  var _rightPath = '/';
  var _leftEntries = [];
  var _rightEntries = [];
  var _leftSelected = [];
  var _rightSelected = [];
  var _activePanel = 'left';
  var _layoutMode = 'pro';

  fm.init = function () {
    _leftPath = Z.state.fmLeftPath || '/';
    _rightPath = Z.state.fmRightPath || '/';
    _layoutMode = readLayoutMode();
    applyLayoutMode(_layoutMode);
    wireButtons();
    setActivePanel(_activePanel || 'left');
    loadPanel('left', _leftPath);
    loadPanel('right', _rightPath);
  };

  fm.refresh = function () {
    loadPanel('left', _leftPath);
    loadPanel('right', _rightPath);
  };

  fm.open = function (side, path) {
    loadPanel(side === 'right' ? 'right' : 'left', path || '/');
  };

  function readLayoutMode() {
    try {
      return localStorage.getItem('zftpd_fm_layout') === 'classic' ? 'classic' : 'pro';
    } catch (e) {
      return 'pro';
    }
  }

  function saveLayoutMode(mode) {
    try { localStorage.setItem('zftpd_fm_layout', mode); } catch (e) {}
  }

  function applyLayoutMode(mode) {
    var view = $('view-filemanager');
    var btn = $('fm-layout-toggle');
    _layoutMode = mode === 'classic' ? 'classic' : 'pro';
    if (view) view.classList.toggle('fm-classic', _layoutMode === 'classic');
    if (btn) btn.textContent = _layoutMode === 'classic' ? 'Professional' : 'Classic';
  }

  function toggleLayoutMode() {
    var next = _layoutMode === 'classic' ? 'pro' : 'classic';
    applyLayoutMode(next);
    saveLayoutMode(next);
    Z.toast(next === 'classic' ? 'Classic layout enabled' : 'Professional layout enabled', 'ok');
  }

  function loadPanel(side, path) {
    path = Z.norm(path);
    if (side === 'left') {
      _leftPath = path;
      Z.state.fmLeftPath = path;
      _leftSelected = [];
    } else {
      _rightPath = path;
      Z.state.fmRightPath = path;
      _rightSelected = [];
    }
    if (Z.updateTopbarContext) Z.updateTopbarContext();

    setActivePanel(side);
    renderPanelPath(side, path);
    updatePanelChrome(side);
    updateActionStates();

    var body = $('fm-' + side + '-body');
    if (body) body.innerHTML = '<div class="fm-loading"><div class="loader"></div></div>';

    Z.api.list(path).then(function (d) {
      var entries = (d && Array.isArray(d.entries)) ? d.entries : [];
      entries.sort(function (a, b) {
        var da = isDir(a) ? 0 : 1;
        var db = isDir(b) ? 0 : 1;
        if (da !== db) return da - db;
        return String(a.name || '').toLowerCase().localeCompare(String(b.name || '').toLowerCase());
      });

      if (side === 'left') _leftEntries = entries;
      else _rightEntries = entries;

      renderPanel(side, entries);
      updateFooter(side, entries);
      updatePanelChrome(side);
      updateActionStates();
    }).catch(function (err) {
      if (side === 'left') _leftEntries = [];
      else _rightEntries = [];
      if (body) {
        body.innerHTML = '<div class="fm-error">Failed to load ' + esc(path) +
          (err && err.message ? '<br>' + esc(err.message) : '') + '</div>';
      }
      updateFooter(side, []);
      updateActionStates();
    });
  }

  function renderPanelPath(side, path) {
    var bc = $('fm-' + side + '-path');
    if (!bc) return;
    bc.innerHTML = '';

    var root = D.createElement('span');
    root.className = 'crumb' + (path === '/' ? ' act' : '');
    root.textContent = '/';
    root.onclick = function () { loadPanel(side, '/'); };
    bc.appendChild(root);

    var parts = Z.norm(path).split('/');
    var acc = '';
    for (var i = 0; i < parts.length; i++) {
      if (!parts[i]) continue;
      acc += '/' + parts[i];

      var sep = D.createElement('span');
      sep.className = 'cr-sep';
      sep.textContent = '/';
      bc.appendChild(sep);

      var seg = D.createElement('span');
      seg.className = 'crumb' + (acc === path ? ' act' : '');
      seg.textContent = parts[i];
      (function (cp) {
        seg.onclick = function () { loadPanel(side, cp); };
      })(acc);
      bc.appendChild(seg);
    }
  }

  function renderPanel(side, entries) {
    var body = $('fm-' + side + '-body');
    if (!body) return;
    body.innerHTML = '';

    var path = getPath(side);

    if (path !== '/') {
      var parentRow = D.createElement('div');
      parentRow.className = 'fm-row fm-parent';
      parentRow.innerHTML =
        '<div class="fm-row-main">' +
          '<div class="fm-row-ico fi-dir">' + ICO.arrowUp + '</div>' +
          '<div class="fm-row-name">..</div>' +
        '</div>' +
        '<div class="fm-row-size"></div>' +
        '<div class="fm-row-date"></div>';
      parentRow.onclick = function () { setActivePanel(side); };
      parentRow.ondblclick = function () { loadPanel(side, Z.parent(path) || '/'); };
      body.appendChild(parentRow);
    }

    if (!entries.length) {
      var empty = D.createElement('div');
      empty.className = 'fm-empty';
      empty.textContent = 'Empty directory';
      body.appendChild(empty);
      return;
    }

    for (var i = 0; i < entries.length; i++) {
      body.appendChild(makeRow(side, entries[i], i, path));
    }
  }

  function makeRow(side, entry, index, basePath) {
    var directory = isDir(entry);
    var fullPath = Z.join(basePath, entry.name);
    var cat = Z.fileCategory(entry.name, directory);
    var row = D.createElement('div');
    row.className = 'fm-row';
    row.setAttribute('data-index', index);
    row.setAttribute('data-path', fullPath);
    row.setAttribute('data-name', entry.name || '');
    row.setAttribute('data-dir', directory ? '1' : '0');
    row.innerHTML =
      '<div class="fm-row-main">' +
        '<div class="fm-row-ico fi-' + esc(cat) + '">' + (directory ? ICO.folder : ICO.file) + '</div>' +
        '<div class="fm-row-name" title="' + esc(entry.name) + '">' + esc(entry.name) + '</div>' +
      '</div>' +
      '<div class="fm-row-size">' + (directory ? '' : esc(Z.bytes(entry.size || 0))) + '</div>' +
      '<div class="fm-row-date">' + esc(formatMtime(entry.mtime)) + '</div>';

    row.onclick = function (ev) {
      selectRow(side, index, !!(ev.ctrlKey || ev.metaKey), !!ev.shiftKey);
    };
    row.ondblclick = function () {
      if (directory) loadPanel(side, fullPath);
      else Z.download(Z.api.downloadUrl(fullPath));
    };

    return row;
  }

  function selectRow(side, index, additive, range) {
    setActivePanel(side);
    var selected = getSelected(side);

    if (range && selected.length) {
      var last = selected[selected.length - 1];
      var start = Math.min(last, index);
      var end = Math.max(last, index);
      selected.length = 0;
      for (var i = start; i <= end; i++) selected.push(i);
    } else if (additive) {
      var pos = selected.indexOf(index);
      if (pos >= 0) selected.splice(pos, 1);
      else selected.push(index);
    } else {
      selected.length = 0;
      selected.push(index);
    }

    updateSelection(side);
    updateFooter(side, getEntries(side));
    updateActionStates();
  }

  function updateSelection(side) {
    var body = $('fm-' + side + '-body');
    if (!body) return;
    var selected = getSelected(side);
    var rows = body.querySelectorAll('.fm-row[data-index]');
    for (var i = 0; i < rows.length; i++) {
      var idx = parseInt(rows[i].getAttribute('data-index'), 10);
      rows[i].classList.toggle('selected', selected.indexOf(idx) >= 0);
    }
  }

  function updateFooter(side, entries) {
    var footer = $('fm-' + side + '-footer');
    if (!footer) return;
    var dirs = 0;
    var files = 0;
    var totalSize = 0;
    for (var i = 0; i < entries.length; i++) {
      if (isDir(entries[i])) dirs++;
      else {
        files++;
        totalSize += entries[i].size || 0;
      }
    }
    var selected = selectedItems(side);
    footer.innerHTML =
      '<span><b>' + dirs + '</b> folders, <b>' + files + '</b> files, <b>' + esc(Z.bytes(totalSize)) + '</b></span>' +
      '<span class="fm-selected-count">' + selected.length + ' selected</span>';
  }

  function updatePanelChrome(side) {
    var cur = $('fm-' + side + '-current');
    if (cur) {
      cur.textContent = getPath(side);
      cur.title = getPath(side);
    }
  }

  function setActivePanel(side) {
    _activePanel = side === 'right' ? 'right' : 'left';
    var left = $('fm-panel-left');
    var right = $('fm-panel-right');
    if (left) left.classList.toggle('active', _activePanel === 'left');
    if (right) right.classList.toggle('active', _activePanel === 'right');
    updateActionStates();
  }

  function updateActionStates() {
    setDisabled('fm-copy-right', !_leftSelected.length);
    setDisabled('fm-copy-left', !_rightSelected.length);
    setDisabled('fm-delete', !selectedItems(_activePanel).length);
    setDisabled('fm-left-rename', selectedItems('left').length !== 1);
    setDisabled('fm-right-rename', selectedItems('right').length !== 1);
    setDisabled('fm-left-download', !hasSelectedFile('left'));
    setDisabled('fm-right-download', !hasSelectedFile('right'));
  }

  function wireButtons() {
    if (_wired) return;
    _wired = true;

    on('fm-copy-right', function () { doCopy('left', 'right'); });
    on('fm-copy-left', function () { doCopy('right', 'left'); });
    on('fm-delete', doDeleteSelected);
    on('fm-swap', swapPanels);
    on('fm-refresh-all', fm.refresh);
    on('fm-layout-toggle', toggleLayoutMode);

    on('fm-refresh-left', function () { loadPanel('left', _leftPath); });
    on('fm-refresh-right', function () { loadPanel('right', _rightPath); });
    on('fm-left-up', function () { goUp('left'); });
    on('fm-right-up', function () { goUp('right'); });
    on('fm-left-mkdir', function () { newFolder('left'); });
    on('fm-right-mkdir', function () { newFolder('right'); });
    on('fm-left-rename', function () { renameSelected('left'); });
    on('fm-right-rename', function () { renameSelected('right'); });
    on('fm-left-download', function () { downloadSelected('left'); });
    on('fm-right-download', function () { downloadSelected('right'); });

    var panels = D.querySelectorAll('.fm-panel[data-side]');
    for (var p = 0; p < panels.length; p++) {
      (function (panel) {
        panel.onclick = function () { setActivePanel(panel.getAttribute('data-side')); };
      })(panels[p]);
    }

    var presets = D.querySelectorAll('.fm-preset[data-fm-side][data-fm-path]');
    for (var i = 0; i < presets.length; i++) {
      (function (btn) {
        btn.onclick = function () {
          loadPanel(btn.getAttribute('data-fm-side'), btn.getAttribute('data-fm-path'));
        };
      })(presets[i]);
    }
  }

  function on(id, fn) {
    var el = $(id);
    if (el) el.onclick = fn;
  }

  function goUp(side) {
    var parent = Z.parent(getPath(side));
    if (parent !== null) loadPanel(side, parent);
  }

  function newFolder(side) {
    setActivePanel(side);
    Z.modal.prompt('New folder', '').then(function (name) {
      name = name ? String(name).trim() : '';
      if (!name) return;
      Z.api.mkdir(getPath(side), name).then(function () {
        Z.toast('Folder created', 'ok');
        loadPanel(side, getPath(side));
      }).catch(function (e) {
        Z.toast('Create folder failed: ' + (e && e.message ? e.message : 'error'), 'er');
      });
    });
  }

  function renameSelected(side) {
    setActivePanel(side);
    var items = selectedItems(side);
    if (items.length !== 1) {
      Z.toast('Select one item to rename', 'wn');
      return;
    }
    var item = items[0];
    Z.modal.prompt('Rename', item.entry.name || '').then(function (name) {
      name = name ? String(name).trim() : '';
      if (!name || name === item.entry.name) return;
      Z.api.rename(item.path, name).then(function () {
        Z.toast('Renamed', 'ok');
        loadPanel(side, getPath(side));
      }).catch(function (e) {
        Z.toast('Rename failed: ' + (e && e.message ? e.message : 'error'), 'er');
      });
    });
  }

  function downloadSelected(side) {
    setActivePanel(side);
    var items = selectedItems(side);
    var files = [];
    for (var i = 0; i < items.length; i++) {
      if (!isDir(items[i].entry)) files.push(items[i]);
    }
    if (!files.length) {
      Z.toast('Select one or more files', 'wn');
      return;
    }
    for (var f = 0; f < files.length; f++) {
      (function (item, delay) {
        setTimeout(function () { Z.download(Z.api.downloadUrl(item.path)); }, delay);
      })(files[f], f * 140);
    }
    Z.notify('Download started', files.length + ' file(s)', 'ok');
  }

  function swapPanels() {
    var left = _leftPath;
    var right = _rightPath;
    _leftPath = right;
    _rightPath = left;
    Z.state.fmLeftPath = _leftPath;
    Z.state.fmRightPath = _rightPath;
    loadPanel('left', _leftPath);
    loadPanel('right', _rightPath);
    Z.toast('Panels swapped', 'ok');
  }

  function doCopy(fromSide, toSide) {
    if (!Z.ensureTransferIdle()) return;
    var items = selectedItems(fromSide);
    var fromPath = getPath(fromSide);
    var toPath = getPath(toSide);

    if (!items.length) {
      Z.toast('Select files first', 'wn');
      return;
    }

    var i = 0;
    var cancelled = false;
    var progressTimer = null;
    var totalItems = items.length;
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
      clearSelection(fromSide);
      loadPanel(toSide, toPath);
      loadPanel(fromSide, fromPath);
      if (!cancelled && ok && doneItems > 0) {
        Z.notify('Copy complete', doneItems + ' of ' + totalItems + ' items copied to ' + toPath, 'ok');
      }
    }

    function processNext() {
      if (cancelled) {
        finish(false);
        return;
      }
      if (i >= items.length) {
        finish(true);
        return;
      }

      var item = items[i];
      var entry = item.entry;
      if (!entry) {
        i++;
        processNext();
        return;
      }

      Z.showTransferLock({
        label: 'COPYING (' + (i + 1) + '/' + totalItems + ')',
        filename: entry.name,
        dest: toPath,
        onCancel: function () {
          cancelled = true;
          Z.api.copyCancel().catch(function () {});
          finish(false);
        }
      });

      Z.api.copy(item.path, toPath, entry.size || 0).then(function (resp) {
        if (!resp || !resp.async) {
          doneItems++;
          i++;
          processNext();
          return;
        }

        var started = Date.now();
        var prevBytes = 0;
        var prevTime = Date.now();
        progressTimer = setInterval(function () {
          Z.api.copyProgress().then(function (p) {
            if (cancelled) return;
            var elapsed = Math.floor((Date.now() - started) / 1000);
            if (p && p.active && !p.done) {
              var pct = p.total_bytes > 0 ? Math.min(100, Math.round(p.bytes_copied * 100 / p.total_bytes)) : 0;
              var now = Date.now();
              var dt = (now - prevTime) / 1000;
              var instantSpeed = dt > 0 ? (p.bytes_copied - prevBytes) / dt : 0;
              prevBytes = p.bytes_copied;
              prevTime = now;
              Z.updateTransferLock({
                pct: pct,
                speed: instantSpeed > 0 ? Z.bps(instantSpeed) : (p.total_bytes > 0 && elapsed > 0 ? Z.bps(p.bytes_copied / elapsed) : ''),
                elapsed: elapsed + 's'
              });
            } else if (p && p.done) {
              stopPolling();
              if (p.error) {
                Z.notify('Copy failed', entry.name + ': ' + (p.error_msg || 'error'), 'er');
              } else {
                doneItems++;
              }
              i++;
              processNext();
            } else if (!p || !p.active) {
              stopPolling();
              if (!p || !p.error) doneItems++;
              i++;
              processNext();
            }
          }).catch(function () {
            /* Keep polling if a transient status request fails. */
          });
        }, 500);
      }).catch(function (e) {
        stopPolling();
        Z.notify('Copy failed', entry.name + ': ' + (e && e.message ? e.message : 'error'), 'er');
        i++;
        processNext();
      });
    }

    processNext();
  }

  function doDeleteSelected() {
    var side = _activePanel;
    var items = selectedItems(side);
    var path = getPath(side);

    if (!items.length) {
      Z.toast('Select files first', 'wn');
      return;
    }

    var names = [];
    for (var n = 0; n < items.length; n++) names.push(items[n].entry.name || '');
    Z.modal.confirm('Delete ' + items.length + ' item(s)', names.join(', '), true).then(function (ok) {
      if (!ok) return;
      var i = 0;
      function next() {
        if (i >= items.length) {
          clearSelection(side);
          loadPanel(side, path);
          Z.toast('Deleted', 'ok');
          return;
        }
        var item = items[i++];
        Z.api.del(item.path, isDir(item.entry)).then(next).catch(function (e) {
          Z.toast('Delete failed: ' + (e && e.message ? e.message : 'error'), 'er');
        });
      }
      next();
    });
  }

  function selectedItems(side) {
    var selected = getSelected(side);
    var entries = getEntries(side);
    var base = getPath(side);
    var items = [];
    for (var i = 0; i < selected.length; i++) {
      var entry = entries[selected[i]];
      if (!entry) continue;
      items.push({
        index: selected[i],
        entry: entry,
        path: Z.join(base, entry.name)
      });
    }
    return items;
  }

  function hasSelectedFile(side) {
    var items = selectedItems(side);
    for (var i = 0; i < items.length; i++) {
      if (!isDir(items[i].entry)) return true;
    }
    return false;
  }

  function clearSelection(side) {
    if (side === 'left') _leftSelected = [];
    else _rightSelected = [];
    updateSelection(side);
    updateFooter(side, getEntries(side));
    updateActionStates();
  }

  function getPath(side) {
    return side === 'left' ? _leftPath : _rightPath;
  }

  function getEntries(side) {
    return side === 'left' ? _leftEntries : _rightEntries;
  }

  function getSelected(side) {
    return side === 'left' ? _leftSelected : _rightSelected;
  }

  function isDir(entry) {
    return entry && (entry.type === 'directory' || entry.type === 'dir' || entry.is_dir === true);
  }

  function setDisabled(id, disabled) {
    var el = $(id);
    if (el) el.disabled = !!disabled;
  }

  function formatMtime(mtime) {
    if (!mtime) return '';
    return Z.relativeTime ? Z.relativeTime(mtime) : String(mtime);
  }

  function esc(value) {
    return Z.h ? Z.h(value) : String(value == null ? '' : value);
  }

  Z.fileManager = fm;

})(ZFTPD);
