/* ══ SETTINGS VIEW ════════════════════════════════════════════════════════
 * User preferences panel. All settings persist in localStorage.
 * ES5 compatible for PS5 browser.
 *
 *  ┌─────────────────────────────────────────────────┐
 *  │  ⚙ Settings                                     │
 *  │                                                  │
 *  │  ┌─ Display ──────────────────────────────────┐  │
 *  │  │  Theme            [PS5 ▾]                  │  │
 *  │  │  Default View     [Grid ▾]                 │  │
 *  │  │  Animations       [●━━━]                   │  │
 *  │  └────────────────────────────────────────────┘  │
 *  │                                                  │
 *  │  ┌─ File Explorer ────────────────────────────┐  │
 *  │  │  Show System Folders    [━━━○]             │  │
 *  │  │  Show Hidden Files      [━━━○]             │  │
 *  │  └────────────────────────────────────────────┘  │
 *  │                                                  │
 *  │  ┌─ About ────────────────────────────────────┐  │
 *  │  │  zftpd v1.5.0                              │  │
 *  │  │  Console File Manager                      │  │
 *  │  │  github.com/seregonwar/zftpd               │  │
 *  │  └────────────────────────────────────────────┘  │
 *  └─────────────────────────────────────────────────┘
 * ═════════════════════════════════════════════════════════════════════════ */

var ZFTPD = ZFTPD || {};

(function (Z) {
  'use strict';

  var D = document;
  var ICO = Z.ICO;

  /* ══════════════════════════════════════════════════════════════════════
   * DEFAULT SETTINGS
   *
   * Dangerous PS4/PS5 system folders that can brick the console
   * if modified by an inexperienced user:
   *
   *   /system        — Core OS binaries
   *   /system_ex     — Extended OS (updates, recovery)
   *   /preinst       — Pre-installed system data
   *   /preinst2      — Secondary pre-install partition
   *   /mini-syscore  — Minimal system core (safe mode)
   *   /sandcastle    — Sandbox environment
   *   /update        — System update staging area
   *   /dev           — Device nodes
   *   /proc          — Process filesystem
   * ══════════════════════════════════════════════════════════════════════ */

  var DEFAULTS = {
    showSystemFolders: false,
    showHiddenFiles: false,
    defaultView: 'grid',
    animationsEnabled: true
  };

  var DANGEROUS_FOLDERS = [
    'system', 'system_ex', 'preinst', 'preinst2',
    'mini-syscore', 'sandcastle', 'update',
    'dev', 'proc'
  ];

  /* ── Load / Save ── */

  Z.settings = {};

  Z.loadSettings = function () {
    var saved = {};
    try {
      var raw = localStorage.getItem('zftpd_settings');
      if (raw) saved = JSON.parse(raw);
    } catch (e) { /* ignore */ }

    var key;
    for (key in DEFAULTS) {
      if (DEFAULTS.hasOwnProperty(key)) {
        Z.settings[key] = saved.hasOwnProperty(key) ? saved[key] : DEFAULTS[key];
      }
    }
  };

  Z.saveSettings = function () {
    try {
      localStorage.setItem('zftpd_settings', JSON.stringify(Z.settings));
    } catch (e) { /* ignore */ }
  };

  /* ── Dangerous folder check ── */

  Z.isDangerousFolder = function (name) {
    var lower = (name || '').toLowerCase();
    for (var i = 0; i < DANGEROUS_FOLDERS.length; i++) {
      if (lower === DANGEROUS_FOLDERS[i]) return true;
    }
    return false;
  };

  Z.isHiddenFile = function (name) {
    return (name || '').charAt(0) === '.';
  };

  /* ── Init on load ── */
  Z.loadSettings();

  /* ══════════════════════════════════════════════════════════════════════
   * SETTINGS VIEW RENDERER
   * ══════════════════════════════════════════════════════════════════════ */

  var settings = {};

  settings.refresh = function () {
    var container = D.getElementById('settings-content');
    if (!container) return;

    Z.loadSettings();

    container.innerHTML =
      /* ── Header ── */
      '<div class="sett-header">' +
        '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>' +
        '<span>Settings</span>' +
      '</div>' +

      /* ══ Display Section ══ */
      '<div class="sett-section">' +
        '<div class="sett-section-title">' +
          '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect width="20" height="14" x="2" y="3" rx="2"/><line x1="8" x2="16" y1="21" y2="21"/><line x1="12" x2="12" y1="17" y2="21"/></svg>' +
          ' Display' +
        '</div>' +

        /* Theme selector */
        '<div class="sett-row">' +
          '<div class="sett-label">' +
            '<div class="sett-name">Theme</div>' +
            '<div class="sett-desc">Choose your color scheme</div>' +
          '</div>' +
          '<select id="sett-theme" class="sett-select">' +
            _buildThemeOptions() +
          '</select>' +
        '</div>' +

        /* Default View */
        '<div class="sett-row">' +
          '<div class="sett-label">' +
            '<div class="sett-name">Default View</div>' +
            '<div class="sett-desc">File explorer layout mode</div>' +
          '</div>' +
          '<select id="sett-default-view" class="sett-select">' +
            '<option value="grid"' + (Z.settings.defaultView === 'grid' ? ' selected' : '') + '>Grid</option>' +
            '<option value="list"' + (Z.settings.defaultView === 'list' ? ' selected' : '') + '>List</option>' +
            '<option value="details"' + (Z.settings.defaultView === 'details' ? ' selected' : '') + '>Details</option>' +
          '</select>' +
        '</div>' +

        /* Animations */
        _buildToggleRow('sett-animations', 'Animations', 'Smooth transitions and micro-animations', Z.settings.animationsEnabled) +

      '</div>' +

      /* ══ File Explorer Section ══ */
      '<div class="sett-section">' +
        '<div class="sett-section-title">' +
          '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>' +
          ' File Explorer' +
        '</div>' +

        _buildToggleRow('sett-sys-folders', 'Show System Folders',
          'Reveal system, system_ex, preinst and other protected directories. Modifying these can brick your console.',
          Z.settings.showSystemFolders, true) +

        _buildToggleRow('sett-hidden', 'Show Hidden Files',
          'Show files and folders starting with a dot (.)',
          Z.settings.showHiddenFiles) +

      '</div>' +

      /* ══ Hardware & System ══ */
      '<div class="sett-section">' +
        '<div class="sett-section-title">' +
          '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="4" y="4" width="16" height="16" rx="2" ry="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="1" x2="9" y2="4"/><line x1="15" y1="1" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="23"/><line x1="15" y1="20" x2="15" y2="23"/><line x1="20" y1="9" x2="23" y2="9"/><line x1="20" y1="14" x2="23" y2="14"/><line x1="1" y1="9" x2="4" y2="9"/><line x1="1" y1="14" x2="4" y2="14"/></svg>' +
          ' Console Hardware' +
        '</div>' +
        _buildSliderRow('sett-fan-speed', 'Target Temperature',
          'Threshold for SMC fan control algorithm. A lower temp generally means higher RPM. (PS4/PS5 payload exclusive)',
          Z.settings.fanThreshold || 60, 40, 90, '°C') +
      '</div>' +

      /* ══ Danger Zone ══ */
      '<div class="sett-section sett-danger-info">' +
        '<div class="sett-section-title">' +
          '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3Z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>' +
          ' Protected Directories' +
        '</div>' +
        '<div class="sett-danger-list">' +
          _buildDangerList() +
        '</div>' +
      '</div>' +

      /* ══ About Section ══ */
      '<div class="sett-section">' +
        '<div class="sett-section-title">' +
          '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>' +
          ' About' +
        '</div>' +
        '<div class="sett-about">' +
          '<div class="sett-about-logo">' +
            '<img src="assets/zftpd-logo.png" alt="zftpd" style="width:48px;height:48px;object-fit:contain;">' +
          '</div>' +
          '<div class="sett-about-info">' +
            '<div class="sett-about-name">zftpd</div>' +
            '<div class="sett-about-ver">v1.5.0</div>' +
            '<div class="sett-about-desc">The ultimate PS4/PS5 file manager.<br>FTP + HTTP + PKG installer.</div>' +
            '<div class="sett-about-link">' +
              '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M15 22v-4a4.8 4.8 0 0 0-1-3.5c3 0 6-2 6-5.5.08-1.25-.27-2.48-1-3.5.28-1.15.28-2.35 0-3.5 0 0-1 0-3 1.5-2.64-.5-5.36-.5-8 0C6 2 5 2 5 2c-.3 1.15-.3 2.35 0 3.5A5.4 5.4 0 0 0 4 9c0 3.5 3 5.5 6 5.5-.39.49-.68 1.05-.85 1.65S8.93 17.38 9 18v4"/><path d="M9 18c-4.51 2-5-2-7-2"/></svg>' +
              ' github.com/seregonwar/zftpd' +
            '</div>' +
          '</div>' +
        '</div>' +
      '</div>';

    /* ── Wire up event handlers ── */
    _bindEvents();
  };

  /* ── HTML Helpers ── */

  function _buildThemeOptions() {
    var cur = D.documentElement.getAttribute('data-theme') || 'ps5';
    var html = '';
    for (var i = 0; i < Z.THEMES.length; i++) {
      var t = Z.THEMES[i];
      html += '<option value="' + t.id + '"' + (t.id === cur ? ' selected' : '') + '>' + t.name + ' — ' + t.desc + '</option>';
    }
    return html;
  }

  function _buildToggleRow(id, name, desc, checked, isDanger) {
    return '<div class="sett-row">' +
      '<div class="sett-label">' +
        '<div class="sett-name">' + (isDanger ? '<span class="sett-warn-dot"></span>' : '') + name + '</div>' +
        '<div class="sett-desc">' + desc + '</div>' +
      '</div>' +
      '<label class="sett-toggle">' +
        '<input type="checkbox" id="' + id + '"' + (checked ? ' checked' : '') + '>' +
        '<span class="sett-toggle-track' + (isDanger ? ' danger' : '') + '">' +
          '<span class="sett-toggle-thumb"></span>' +
        '</span>' +
      '</label>' +
    '</div>';
  }

  function _buildSliderRow(id, name, desc, value, min, max, unit, isDanger) {
    return '<div class="sett-row" style="flex-wrap:wrap;">' +
      '<div class="sett-label" style="flex:1 1 200px;">' +
        '<div class="sett-name">' + (isDanger ? '<span class="sett-warn-dot"></span>' : '') + name + '</div>' +
        '<div class="sett-desc">' + desc + '</div>' +
      '</div>' +
      '<div class="sett-slider-wrap" style="display:flex;align-items:center;gap:12px;flex:1 1 200px;justify-content:flex-end;">' +
        '<input type="range" id="' + id + '" min="' + min + '" max="' + max + '" value="' + value + '" style="flex:auto;max-width:200px;">' +
        '<span id="' + id + '-val" style="min-width:40px;text-align:right;font-weight:600;font-variant-numeric:tabular-nums;">' + value + unit + '</span>' +
      '</div>' +
    '</div>';
  }

  function _buildDangerList() {
    var html = '';
    for (var i = 0; i < DANGEROUS_FOLDERS.length; i++) {
      html += '<span class="sett-danger-tag">/' + DANGEROUS_FOLDERS[i] + '</span>';
    }
    return html;
  }

  /* ── Event Binding ── */

  function _bindEvents() {
    /* Theme */
    var themeSel = D.getElementById('sett-theme');
    if (themeSel) {
      themeSel.onchange = function () {
        Z.setTheme(this.value);
        /* Re-render to update the theme selector */
      };
    }

    /* Default view */
    var viewSel = D.getElementById('sett-default-view');
    if (viewSel) {
      viewSel.onchange = function () {
        Z.settings.defaultView = this.value;
        Z.saveSettings();
      };
    }

    /* Animations */
    var animCb = D.getElementById('sett-animations');
    if (animCb) {
      animCb.onchange = function () {
        Z.settings.animationsEnabled = this.checked;
        Z.saveSettings();
        /* Toggle CSS animations globally */
        if (!this.checked) {
          D.documentElement.style.setProperty('--transition-fast', '0s');
          D.documentElement.style.setProperty('--transition-med', '0s');
          D.documentElement.style.setProperty('--transition-slow', '0s');
        } else {
          D.documentElement.style.removeProperty('--transition-fast');
          D.documentElement.style.removeProperty('--transition-med');
          D.documentElement.style.removeProperty('--transition-slow');
        }
      };
    }

    /* Show system folders */
    var sysCb = D.getElementById('sett-sys-folders');
    if (sysCb) {
      sysCb.onchange = function () {
        Z.settings.showSystemFolders = this.checked;
        Z.saveSettings();
        Z.toast(this.checked ? 'System folders visible' : 'System folders hidden', this.checked ? 'wn' : 'ok');
      };
    }

    /* Show hidden files */
    var hidCb = D.getElementById('sett-hidden');
    if (hidCb) {
      hidCb.onchange = function () {
        Z.settings.showHiddenFiles = this.checked;
        Z.saveSettings();
        Z.toast(this.checked ? 'Hidden files visible' : 'Hidden files hidden', 'ok');
      };
    }
    
    /* Fan Speed */
    var fanSlider = D.getElementById('sett-fan-speed');
    if (fanSlider) {
      fanSlider.onchange = function () {
        var val = parseInt(this.value, 10);
        Z.settings.fanThreshold = val;
        Z.saveSettings();
        
        fetch('/api/admin/fan?threshold=' + val)
          .then(function(res) { return res.json(); })
          .then(function(data) {
            if (data.status === 'ok') {
              Z.toast('Fan threshold updated to ' + val + '°C', 'ok');
            } else {
              Z.toast('Fan control failed: ' + (data.message || 'Error'), 'er');
            }
          })
          .catch(function(e) {
            Z.toast('Fan API is unreachable or blocked', 'er');
          });
      };
      
      fanSlider.oninput = function () {
        var valEl = D.getElementById('sett-fan-speed-val');
        if (valEl) valEl.textContent = this.value + '°C';
      };
    }
  }

  Z.settingsView = settings;

})(ZFTPD);
