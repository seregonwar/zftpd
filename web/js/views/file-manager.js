/* ══ FILE MANAGER VIEW — FileZilla-style Dual Pane ════════════════════════
 * Split view with source and destination panels.
 * ES5 compatible for PS5 browser.
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var D = document;
  var $ = Z.$;
  var ICO = Z.ICO;

  var fm = {};
  var _leftPath = '/';
  var _rightPath = '/';
  var _leftEntries = [];
  var _rightEntries = [];
  var _leftSelected = [];
  var _rightSelected = [];
  var _activePanel = 'left';

  fm.init = function () {
    _leftPath = Z.state.fmLeftPath || '/';
    _rightPath = Z.state.fmRightPath || '/';
    loadPanel('left', _leftPath);
    loadPanel('right', _rightPath);
    wireButtons();
  };

  /* ── Load a panel ── */
  function loadPanel(side, path) {
    path = Z.norm(path);
    if (side === 'left') { _leftPath = path; Z.state.fmLeftPath = path; }
    else { _rightPath = path; Z.state.fmRightPath = path; }

    var body = $('fm-' + side + '-body');
    if (body) body.innerHTML = '<div style="padding:20px;text-align:center;"><div class="loader" style="margin:0 auto;"></div></div>';

    renderPanelPath(side, path);

    Z.api.list(path).then(function (d) {
      var entries = (d && Array.isArray(d.entries)) ? d.entries : [];
      /* Sort: dirs first, then alpha */
      entries.sort(function (a, b) {
        var da = a.type === 'directory' ? 0 : 1;
        var db = b.type === 'directory' ? 0 : 1;
        if (da !== db) return da - db;
        return (a.name || '').toLowerCase().localeCompare((b.name || '').toLowerCase());
      });

      if (side === 'left') { _leftEntries = entries; _leftSelected = []; }
      else { _rightEntries = entries; _rightSelected = []; }

      renderPanel(side, entries);
      updateFooter(side, entries);
    }).catch(function () {
      if (body) body.innerHTML = '<div style="padding:20px;text-align:center;color:var(--er);font-size:12px;">Failed to load</div>';
    });
  }

  /* ── Render panel breadcrumb ── */
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
      (function (cp) { seg.onclick = function () { loadPanel(side, cp); }; })(acc);
      bc.appendChild(seg);
    }
  }

  /* ── Render panel file rows ── */
  function renderPanel(side, entries) {
    var body = $('fm-' + side + '-body');
    if (!body) return;
    body.innerHTML = '';

    var path = side === 'left' ? _leftPath : _rightPath;

    /* Parent directory row */
    if (path !== '/') {
      var parentRow = D.createElement('div');
      parentRow.className = 'fm-row';
      parentRow.innerHTML =
        '<div class="fm-row-ico fi-dir">' + ICO.arrowUp + '</div>' +
        '<div class="fm-row-name">..</div>' +
        '<div class="fm-row-size"></div>';
      parentRow.ondblclick = function () {
        loadPanel(side, Z.parent(path) || '/');
      };
      body.appendChild(parentRow);
    }

    if (!entries.length) {
      body.innerHTML += '<div style="padding:16px;text-align:center;color:var(--tx3);font-size:11px;">Empty</div>';
      return;
    }

    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      var isDir = e.type === 'directory';
      var fullPath = Z.join(path, e.name);

      var row = D.createElement('div');
      row.className = 'fm-row';
      row.setAttribute('data-index', i);
      row.setAttribute('data-path', fullPath);
      row.setAttribute('data-name', e.name);
      row.setAttribute('data-dir', isDir ? '1' : '0');

      var cat = Z.fileCategory(e.name, isDir);
      row.innerHTML =
        '<div class="fm-row-ico fi-' + cat + '">' + (isDir ? ICO.folder : ICO.file) + '</div>' +
        '<div class="fm-row-name" title="' + e.name + '">' + e.name + '</div>' +
        '<div class="fm-row-size">' + (isDir ? '' : Z.bytes(e.size || 0)) + '</div>';

      /* Click to select */
      (function (r, idx, s) {
        r.onclick = function (ev) {
          _activePanel = s;
          var sel = s === 'left' ? _leftSelected : _rightSelected;
          if (ev.ctrlKey || ev.metaKey) {
            var pos = sel.indexOf(idx);
            if (pos >= 0) sel.splice(pos, 1);
            else sel.push(idx);
          } else {
            if (s === 'left') _leftSelected = [idx];
            else _rightSelected = [idx];
            sel = s === 'left' ? _leftSelected : _rightSelected;
          }
          updateSelection(s);
        };
        r.ondblclick = function () {
          var isD = r.getAttribute('data-dir') === '1';
          if (isD) loadPanel(s, r.getAttribute('data-path'));
          else Z.download(Z.api.downloadUrl(r.getAttribute('data-path')));
        };
      })(row, i, side);

      body.appendChild(row);
    }
  }

  /* ── Update selection highlighting ── */
  function updateSelection(side) {
    var body = $('fm-' + side + '-body');
    if (!body) return;
    var sel = side === 'left' ? _leftSelected : _rightSelected;
    var rows = body.querySelectorAll('.fm-row[data-index]');
    for (var i = 0; i < rows.length; i++) {
      var idx = parseInt(rows[i].getAttribute('data-index'), 10);
      rows[i].classList.toggle('selected', sel.indexOf(idx) >= 0);
    }
  }

  /* ── Update footer stats ── */
  function updateFooter(side, entries) {
    var footer = $('fm-' + side + '-footer');
    if (!footer) return;
    var dirs = 0, files = 0, totalSize = 0;
    for (var i = 0; i < entries.length; i++) {
      if (entries[i].type === 'directory') dirs++;
      else { files++; totalSize += entries[i].size || 0; }
    }
    footer.innerHTML = '<b>' + dirs + '</b> folders, <b>' + files + '</b> files \u2014 ' + Z.bytes(totalSize);
  }

  /* ── Wire center action buttons ── */
  function wireButtons() {
    var copyRight = $('fm-copy-right');
    if (copyRight) copyRight.onclick = function () { doCopy('left', 'right'); };
    var copyLeft = $('fm-copy-left');
    if (copyLeft) copyLeft.onclick = function () { doCopy('right', 'left'); };
    var delBtn = $('fm-delete');
    if (delBtn) delBtn.onclick = function () { doDeleteSelected(); };
    var refreshLeft = $('fm-refresh-left');
    if (refreshLeft) refreshLeft.onclick = function () { loadPanel('left', _leftPath); };
    var refreshRight = $('fm-refresh-right');
    if (refreshRight) refreshRight.onclick = function () { loadPanel('right', _rightPath); };
  }

  /* ── Copy selected files from one panel to the other ── */
  function doCopy(fromSide, toSide) {
    if (!Z.ensureTransferIdle()) return;
    var sel = fromSide === 'left' ? _leftSelected : _rightSelected;
    var entries = fromSide === 'left' ? _leftEntries : _rightEntries;
    var fromPath = fromSide === 'left' ? _leftPath : _rightPath;
    var toPath = toSide === 'left' ? _leftPath : _rightPath;

    if (!sel.length) {
      Z.toast('Select files first', 'wn');
      return;
    }

    var i = 0;
    var cancelled = false;
    var progressTimer = null;
    var totalItems = sel.length;
    var doneItems = 0;

    function stopPolling() {
      if (progressTimer) { clearInterval(progressTimer); progressTimer = null; }
    }

    function finish(ok) {
      stopPolling();
      Z.hideTransferLock();
      loadPanel(toSide, toPath);
      if (!cancelled && ok && doneItems > 0) {
        Z.notify('Copy complete', doneItems + ' of ' + totalItems + ' items copied to ' + toPath, 'ok');
      }
      if (fromSide === 'left') _leftSelected = []; else _rightSelected = [];
      loadPanel(fromSide, fromPath);
    }

    function processNext() {
      if (cancelled) {
        finish(false);
        return;
      }
      if (i >= sel.length) {
        finish(true);
        return;
      }
      var entry = entries[sel[i]];
      if (!entry) { i++; processNext(); return; }

      var srcPath = Z.join(fromPath, entry.name);

      Z.showTransferLock({
        label: 'COPYING (' + (i + 1) + '/' + totalItems + ')',
        filename: entry.name,
        dest: toPath,
        onCancel: function () {
          cancelled = true;
          Z.api.copyCancel().catch(function(){});
          finish(false);
        }
      });

      Z.api.copy(srcPath, toPath, entry.size || 0).then(function (resp) {
        if (!resp || !resp.async) {
          doneItems++;
          i++;
          processNext();
          return;
        }

        /* Poll /api/copy_progress until done */
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
            /* Poll failed, keep trying */
          });
        }, 500);
      }).catch(function (e) {
        stopPolling();
        Z.notify('Copy failed', entry.name + ': ' + (e.message || 'error'), 'er');
        i++;
        processNext();
      });
    }
    processNext();
  }

  /* ── Delete selected files in active panel ── */
  function doDeleteSelected() {
    var sel = _activePanel === 'left' ? _leftSelected : _rightSelected;
    var entries = _activePanel === 'left' ? _leftEntries : _rightEntries;
    var path = _activePanel === 'left' ? _leftPath : _rightPath;

    if (!sel.length) {
      Z.toast('Select files first', 'wn');
      return;
    }

    var names = sel.map(function (idx) { return entries[idx] ? entries[idx].name : ''; }).join(', ');
    Z.modal.confirm('Delete ' + sel.length + ' item(s)', names, true).then(function (ok) {
      if (!ok) return;
      var i = 0;
      function next() {
        if (i >= sel.length) {
          loadPanel(_activePanel, path);
          Z.toast('Deleted', 'ok');
          return;
        }
        var entry = entries[sel[i++]];
        if (!entry) { next(); return; }
        var fullPath = Z.join(path, entry.name);
        Z.api.del(fullPath, entry.type === 'directory').then(function () {
          next();
        }).catch(function (e) {
          Z.toast('Delete failed: ' + e.message, 'er');
        });
      }
      next();
    });
  }

  Z.fileManager = fm;

})(ZFTPD);
