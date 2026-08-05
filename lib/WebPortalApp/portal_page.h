// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#pragma once

const char PORTAL_PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,maximum-scale=1.0,user-scalable=no">
<title>Cyber Fidget - Portal</title>
<style>
/* Phase-B design language, device side. The tokens below are the website's
   cf-kit tokens (cyberfidget_website/assets/css/cf-kit.css) with system
   font tails: the device serves this page over a single SPI stream, so a
   webfont or external stylesheet fetch is not allowed. Components below
   reference these tokens only - a re-theme is a one-block swap.
   Shape language: chamfered corners on panels and filled CTAs, square
   corners on inputs and bordered row actions, no border radius. */
:root {
  /* Native controls (select popups, checkboxes, scrollbars) render dark -
     the same declaration the website's theme.css uses. Without it the
     upload-folder select comes out light and breaks the surface. */
  color-scheme: dark;
  --bg-primary: #0e0820;
  --bg-secondary: #15102a;
  --bg-tertiary: #08050f;
  --bg-elev: #1f1840;
  --bg-sunk: rgba(0,0,0,0.35);
  --text-primary: #f5ecff;
  --text-secondary: #a89cc8;
  --text-mute: #6b5e88;
  --accent: #68e1fd;
  --accent-hover: #a5edff;
  --accent-dim: rgba(104,225,253,0.1);
  --magenta: #ff2bb8;
  --magenta-soft: #ff6ed1;
  --magenta-line: rgba(255,43,184,0.32);
  --purple: #b16cff;
  --danger: #ff5252;
  --danger-hover: #ff7a7a;
  --success: #4dffaf;
  --warn: #ffd84a;
  --border: rgba(245,236,255,0.08);
  --border-cy: rgba(104,225,253,0.35);
  --f-d: 'Chakra Petch',system-ui,sans-serif;
  --f-m: ui-monospace,Consolas,monospace;
  --notch: polygon(0 0,calc(100% - 14px) 0,100% 14px,100% 100%,0 100%);
  --notch-b: polygon(4px 0,100% 0,100% calc(100% - 4px),calc(100% - 4px) 100%,0 100%,0 4px);
  --bevel: inset 0 1px 0 rgba(255,255,255,0.22),inset 0 -3px 0 rgba(0,0,0,0.3);
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:var(--f-d);background:var(--bg-primary);color:var(--text-primary);min-height:100vh;display:flex;overflow:hidden}
a{color:var(--accent);text-decoration:none}

/* Hamburger */
.hamburger{display:none;position:fixed;top:10px;left:10px;z-index:301;background:var(--bg-secondary);border:1px solid var(--border-cy);clip-path:var(--notch-b);width:40px;height:40px;cursor:pointer;font-size:1.4em;color:var(--accent);align-items:center;justify-content:center;line-height:1}
.sidebar-overlay{display:none;position:fixed;inset:0;background:rgba(8,5,15,0.72);z-index:199}

/* Sidebar - the global nav artboard, turned vertical. Active is magenta
   (matching the site nav's magenta underline); cyan stays informational. */
.sidebar{width:220px;background:var(--bg-tertiary);border-right:1px solid var(--border);display:flex;flex-direction:column;flex-shrink:0;height:100vh;z-index:200}
.sidebar-brand{padding:20px 16px 16px;text-align:center;border-bottom:1px solid var(--magenta-line)}
.logo{position:relative;display:inline-block}
.logo-shadow{position:absolute;top:2px;left:2px;font-family:var(--f-d);font-size:22px;font-weight:700;letter-spacing:3px;color:var(--magenta);opacity:0.85;white-space:nowrap}
.logo-main{position:relative;font-family:var(--f-d);font-size:22px;font-weight:700;letter-spacing:3px;color:var(--accent);text-shadow:0 0 12px rgba(104,225,253,0.45);white-space:nowrap}
.logo-sub{font-family:var(--f-m);font-size:0.58em;color:var(--magenta-soft);letter-spacing:0.22em;text-transform:uppercase;margin-top:6px}
.sidebar-nav{padding:8px 0;flex:1;overflow-y:auto}
.nav-item{display:flex;align-items:center;padding:11px 16px;color:var(--text-secondary);cursor:pointer;border-left:3px solid transparent;transition:all 0.2s;font-size:0.82em;font-weight:600;letter-spacing:0.08em;text-transform:uppercase}
.nav-item:hover{background:var(--accent-dim);color:var(--text-primary)}
.nav-item.active{color:var(--magenta);border-left-color:var(--magenta);background:rgba(255,43,184,0.09)}
.nav-item .icon{margin-right:10px;font-size:1.1em;opacity:0.85}
.nav-item .badge{margin-left:auto;font-family:var(--f-m);font-size:0.68em;font-weight:700;letter-spacing:0.06em;background:var(--warn);padding:1px 6px;color:var(--bg-tertiary)}

/* Main. column-reverse puts the status line ABOVE the title, which is the
   artboard masthead order: cyan mono eyebrow over a display-face heading. */
.main{flex:1;display:flex;flex-direction:column;height:100vh;overflow:hidden;min-width:0}
.page-header{padding:13px 20px 12px;border-bottom:1px solid var(--magenta-line);background:var(--bg-tertiary);display:flex;flex-direction:column-reverse;align-items:flex-start}
.page-header h1{font-size:1.45em;font-weight:700;letter-spacing:0.02em;text-transform:uppercase;line-height:1.1}
.page-header .status{font-family:var(--f-m);font-size:0.66em;font-weight:700;color:var(--accent);letter-spacing:0.18em;text-transform:uppercase;margin-bottom:6px}
.content{flex:1;overflow-y:auto;padding:16px 20px;padding-bottom:120px}

/* Banner */
.banner{background:rgba(104,225,253,0.08);border-bottom:1px solid var(--border-cy);padding:8px 20px;font-family:var(--f-m);font-size:0.76em;color:var(--text-secondary);display:none;align-items:center;gap:8px;line-height:1.5}
.banner.show{display:flex}
.banner .url{color:var(--accent);font-weight:700}
.banner .close-banner{margin-left:auto;cursor:pointer;color:var(--text-mute);padding:2px 6px}

/* Drop zone. Dashed edges stay square - the artboards reserve the chamfer
   for panels and filled CTAs, and pair dashed borders with sharp corners. */
.drop-zone{border:1px dashed var(--border-cy);padding:24px 16px;text-align:center;margin-bottom:16px;cursor:pointer;transition:all 0.2s;background:var(--accent-dim)}
.drop-zone:hover,.drop-zone.drag-over{border-color:var(--accent);background:rgba(104,225,253,0.08)}
.drop-zone p{color:var(--text-secondary);margin-bottom:3px}
.drop-zone .main-text{color:var(--accent);font-size:0.92em;font-weight:700;letter-spacing:0.08em;text-transform:uppercase}
.drop-zone .hint{font-family:var(--f-m);font-size:0.76em;color:var(--text-mute)}
.drop-zone .folder-sel{margin-top:10px;display:flex;align-items:center;justify-content:center;gap:8px;font-family:var(--f-m);font-size:0.76em;color:var(--text-secondary)}
.drop-zone .folder-sel select{background:var(--bg-sunk);color:var(--text-primary);border:1px solid var(--border);padding:4px 8px;font-family:var(--f-m);font-size:0.95em}
input[type="file"]{display:none}

/* Upload bar. Cyan-to-magenta is the phase-B progress fill (same gradient
   the companion uses), so progress reads the same on every surface. */
.upload-bar{display:none;margin-bottom:16px}
.upload-bar.active{display:block}
.upload-bar .bar{height:6px;background:var(--bg-sunk);border:1px solid var(--border);overflow:hidden}
.upload-bar .fill{height:100%;width:0%;background:linear-gradient(90deg,var(--accent),var(--magenta));transition:width 0.15s}
.upload-bar .info{display:flex;justify-content:space-between;font-family:var(--f-m);font-size:0.76em;color:var(--text-secondary);margin-top:5px}

/* Section headers = the artboard eyebrow: cyan mono, uppercase, wide track */
.section-hdr{font-family:var(--f-m);font-size:0.7em;font-weight:700;color:var(--accent);text-transform:uppercase;letter-spacing:0.18em;margin:20px 0 10px;display:flex;align-items:center;gap:10px;flex-wrap:wrap}
.section-hdr .spacer{flex:1}
.section-hdr .action{font-family:var(--f-d);font-size:11px;font-weight:700;color:var(--accent);cursor:pointer;text-transform:uppercase;letter-spacing:0.06em;padding:4px 9px;border:1px solid var(--border-cy);white-space:nowrap}
.section-hdr .action:hover{background:var(--accent);color:var(--bg-tertiary)}
.view-toggle{display:inline-flex;border:1px solid var(--border)}
.view-toggle .vt{padding:5px 12px;cursor:pointer;font-family:var(--f-d);font-size:11px;font-weight:600;letter-spacing:0.06em;color:var(--text-secondary);white-space:nowrap}
.view-toggle .vt.active{background:var(--accent);color:var(--bg-tertiary)}

/* File tree. Row hover borrows the artboard item-row treatment: elevated
   background plus a left accent bar, instead of a tinted wash. */
.file-tree{list-style:none}
.file-tree li{display:flex;align-items:center;padding:8px 10px;margin-bottom:1px;transition:background 0.15s;gap:8px;border-left:2px solid transparent}
.file-tree li:hover{background:var(--bg-elev);border-left-color:var(--accent)}
.file-tree .icon{flex-shrink:0;width:18px;text-align:center;color:var(--text-mute)}
.file-tree .name{flex:1;font-size:0.88em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
.file-tree .size{font-family:var(--f-m);color:var(--text-mute);font-size:0.74em;flex-shrink:0}
.file-tree .acts{display:flex;gap:4px;flex-shrink:0;opacity:0;transition:opacity 0.15s}
.file-tree li:hover .acts{opacity:1}
.file-tree .sub{padding-left:20px}
.folder-toggle{cursor:pointer;user-select:none;color:var(--accent);font-weight:700}

/* Search. Inputs stay square with a sunk fill - matches .cf-explore-search */
.search-row{margin-bottom:10px;display:none}
.search-row input{width:100%;padding:9px 12px;background:var(--bg-sunk);border:1px solid var(--border);color:var(--text-primary);font-family:var(--f-m);font-size:0.82em;outline:none}
.search-row input::placeholder{color:var(--text-mute)}
.search-row input:focus{border-color:var(--accent)}
.search-row.show{display:block}

/* Track table (flat view). Column headers take the eyebrow treatment;
   every metadata column is mono, the title stays in the display face. */
.track-table{width:100%;border-collapse:collapse;display:none;font-size:0.85em}
.track-table.show{display:table}
.track-table th{text-align:left;font-family:var(--f-m);color:var(--accent);font-weight:700;font-size:0.72em;text-transform:uppercase;letter-spacing:0.16em;padding:9px 6px;border-bottom:1px solid var(--border-cy);cursor:pointer;user-select:none;white-space:nowrap}
.track-table th:hover{color:var(--accent-hover)}
.sort-arrow{font-size:0.7em;margin-left:3px;opacity:0.4}
.track-table th .sort-arrow.asc,.track-table th .sort-arrow.desc{opacity:1;color:var(--magenta)}
.track-table td{padding:7px 6px;border-bottom:1px solid var(--border);vertical-align:middle}
.track-table tr:hover td{background:var(--bg-elev)}
.track-table tr.playing td{background:rgba(255,43,184,0.1)}
.track-table tr.playing td:first-child{box-shadow:inset 3px 0 0 var(--magenta)}
.pl-track.playing{background:rgba(255,43,184,0.1) !important}
.pl-track.playing .num{color:var(--magenta);font-weight:700}
.track-table .col-title{min-width:120px;max-width:280px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-weight:600}
.track-table .col-artist,.track-table .col-album{font-family:var(--f-m);font-size:0.9em;color:var(--text-secondary);max-width:160px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.track-table .col-size{font-family:var(--f-m);font-size:0.9em;color:var(--text-mute);white-space:nowrap;text-align:right;width:70px;font-variant-numeric:tabular-nums}
.track-table .col-acts{width:100px;text-align:right}
.track-table .col-acts .acts{display:flex;gap:3px;justify-content:flex-end;opacity:0;transition:opacity 0.15s}
.track-table tr:hover .acts{opacity:1}

/* Buttons. Two shapes, per the artboards: bordered row actions (HIDE /
   REMOVE) are square outlines, filled CTAs (SEND TO MY FIDGET / CONNECT)
   are chamfered with an inset bevel. Cyan fill is the primary action here
   because every portal CTA is a device-management verb, which is the
   colour those verbs carry on the rail sheet. */
.btn{background:none;border:1px solid var(--border);padding:6px 11px;cursor:pointer;font-family:var(--f-d);font-size:0.76em;font-weight:700;letter-spacing:0.06em;text-transform:uppercase;color:var(--text-secondary);transition:all 0.15s;white-space:nowrap}
.btn:hover{background:var(--bg-elev);color:var(--text-primary)}
.btn-play{border-color:var(--border-cy);color:var(--accent)}
.btn-play:hover{background:var(--accent);color:var(--bg-tertiary)}
.btn-add{border-color:rgba(77,255,175,0.45);color:var(--success)}
.btn-add:hover{background:var(--success);color:var(--bg-tertiary)}
.btn-move{border-color:rgba(177,108,255,0.45);color:var(--purple)}
.btn-move:hover{background:var(--purple);color:var(--bg-tertiary)}
.btn-del{border-color:rgba(255,82,82,0.5);color:var(--danger)}
.btn-del:hover{background:var(--danger);color:#fff}
.btn-accent{background:var(--accent);border-color:var(--accent);color:var(--bg-tertiary);clip-path:var(--notch-b);box-shadow:var(--bevel)}
.btn-accent:hover{background:var(--accent-hover);color:var(--bg-tertiary)}
.btn-sm{padding:3px 8px;font-size:0.7em;letter-spacing:0.04em}

/* Playlists. A playlist is the artboard's section row (elevated bar,
   display-face uppercase name, mono count) over a list of item rows. */
.playlists{margin-top:16px}
.pl-card{background:var(--bg-secondary);border:1px solid var(--border);clip-path:var(--notch);margin-bottom:10px;overflow:hidden}
.pl-header{display:flex;align-items:center;padding:11px 12px;cursor:pointer;gap:8px;background:var(--bg-elev)}
.pl-header .name{flex:1;font-weight:700;font-size:0.86em;letter-spacing:0.06em;text-transform:uppercase}
.pl-header .count{font-family:var(--f-m);color:var(--text-secondary);font-size:0.74em}
.pl-tracks{border-top:1px solid var(--border);max-height:300px;overflow-y:auto}
.pl-track{display:flex;align-items:center;padding:7px 12px;font-size:0.85em;border-bottom:1px solid var(--border);gap:8px}
.pl-track:hover{background:var(--bg-elev)}
.pl-track .num{font-family:var(--f-m);color:var(--text-mute);font-size:0.86em;width:24px;text-align:center;flex-shrink:0}
.pl-track .name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.pl-track .rm{opacity:0;cursor:pointer;color:var(--danger);padding:2px 6px}
.pl-track:hover .rm{opacity:1}
.pl-actions{display:flex;gap:8px;padding:9px 12px;border-top:1px solid var(--border)}

/* Player bar */
.player-bar{position:fixed;bottom:0;left:220px;right:0;background:var(--bg-secondary);border-top:1px solid var(--magenta-line);display:none;flex-direction:column;padding:10px 16px 12px;z-index:100}
.player-bar.show{display:flex}
.player-top{display:flex;align-items:center;gap:12px;margin-bottom:8px}
.player-controls{display:flex;gap:6px;align-items:center;flex-shrink:0}
.player-controls button{background:none;border:none;color:var(--text-primary);font-size:1.3em;cursor:pointer;padding:4px 6px}
.player-controls button:hover{color:var(--accent);background:var(--accent-dim)}
.player-info{flex:1;min-width:0;overflow:hidden}
.player-info .title{font-size:0.9em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-weight:700}
.player-info .artist{font-family:var(--f-m);font-size:0.74em;color:var(--text-secondary);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:2px}
.player-bottom{display:flex;align-items:center;gap:10px}
.player-bottom .time{font-family:var(--f-m);font-size:0.72em;color:var(--text-mute);min-width:40px;font-variant-numeric:tabular-nums}
.player-bottom .time.right{text-align:right}
.seek-wrap{flex:1;position:relative;height:28px;display:flex;align-items:center}
.seek-track{width:100%;height:4px;background:var(--bg-sunk);overflow:hidden;position:relative}
.seek-fill{height:100%;background:linear-gradient(90deg,var(--accent),var(--magenta));width:0%;pointer-events:none}
.seek-thumb{position:absolute;width:14px;height:14px;border-radius:50%;background:var(--accent);top:50%;transform:translate(-50%,-50%);left:0%;box-shadow:0 0 8px rgba(104,225,253,0.5);pointer-events:none}

/* Playlist dropdown */
.pl-dropdown{display:none;position:fixed;background:var(--bg-secondary);border:1px solid var(--border);clip-path:var(--notch);min-width:180px;max-height:240px;overflow-y:auto;z-index:250;box-shadow:0 8px 24px rgba(0,0,0,0.5)}
.pl-dropdown.show{display:block}
.pl-dropdown .dd-title{padding:9px 12px;font-family:var(--f-m);font-size:0.7em;font-weight:700;color:var(--accent);text-transform:uppercase;letter-spacing:0.18em;border-bottom:1px solid var(--border)}
.pl-dropdown .dd-item{padding:8px 12px;cursor:pointer;font-size:0.86em;transition:background 0.1s}
.pl-dropdown .dd-item:hover{background:var(--bg-elev);color:var(--accent)}
.pl-dropdown .dd-item.new{color:var(--success);border-top:1px solid var(--border)}

/* Modal */
.modal-overlay{display:none;position:fixed;inset:0;background:rgba(8,5,15,0.86);z-index:300;justify-content:center;align-items:center}
.modal-overlay.active{display:flex}
.modal{background:var(--bg-secondary);border:1px solid var(--border);clip-path:var(--notch);padding:20px;min-width:300px;max-width:90vw}
.modal h3{margin-bottom:14px;font-size:0.95em;font-weight:700;letter-spacing:0.08em;text-transform:uppercase;color:var(--accent)}
.modal input[type="text"],.modal select{width:100%;padding:9px 11px;background:var(--bg-sunk);border:1px solid var(--border);color:var(--text-primary);font-family:var(--f-m);font-size:0.86em;margin-bottom:12px;outline:none}
.modal input[type="text"]:focus,.modal select:focus{border-color:var(--accent)}
.modal .modal-err{display:none;font-family:var(--f-m);color:var(--danger);font-size:0.76em;margin:-6px 0 10px}
.modal .modal-err.show{display:block}
.modal .modal-actions{display:flex;gap:8px;justify-content:flex-end}

/* Toast - elevated fill with a magenta hairline, matching .cf-toast */
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:var(--bg-elev);border:1px solid var(--magenta);padding:9px 16px;font-size:0.82em;z-index:400;opacity:0;transition:opacity 0.3s;pointer-events:none}
.toast.show{opacity:1}

/* Empty */
.empty{text-align:center;font-family:var(--f-m);color:var(--text-mute);padding:26px 20px;font-size:0.8em;line-height:1.6}

/* WiFi settings. Each card is the rail panel from the device-rail sheet:
   chamfered panel, a bordered cyan mono eyebrow for the title, mono
   key/value rows underneath. */
.wifi-card{background:var(--bg-secondary);border:1px solid var(--border);clip-path:var(--notch);padding:16px;margin-bottom:14px}
.wifi-card h3{display:inline-block;font-family:var(--f-m);font-size:0.68em;font-weight:700;letter-spacing:0.2em;text-transform:uppercase;color:var(--accent);border:1px solid var(--border-cy);padding:3px 8px;margin-bottom:14px}
.wifi-card .info-row{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:5px 0;font-family:var(--f-m);font-size:0.8em}
.wifi-card .info-row .label{color:var(--text-mute)}
.wifi-card .info-row .val{color:var(--text-primary);text-align:right;overflow-wrap:anywhere}
.wifi-card .info-row .val.accent{color:var(--accent)}
.network-list{list-style:none;margin:10px 0}
.network-list li{display:flex;align-items:center;gap:10px;padding:9px 12px;cursor:pointer;transition:background 0.15s;font-size:0.86em;border-left:2px solid transparent}
.network-list li:hover{background:var(--bg-elev)}
.network-list li.selected{background:var(--bg-elev);border-left-color:var(--accent)}
.network-list .ssid{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.network-list .signal{font-family:var(--f-m);color:var(--text-mute);font-size:0.78em;white-space:nowrap}
.network-list .lock{font-size:0.8em;color:var(--text-mute)}
.wifi-input{width:100%;padding:9px 11px;background:var(--bg-sunk);border:1px solid var(--border);color:var(--text-primary);font-family:var(--f-m);font-size:0.86em;margin:8px 0}
.wifi-input:focus{border-color:var(--accent);outline:none}
.wifi-actions{display:flex;gap:8px;margin-top:12px}
.wifi-spinner{display:inline-block;width:16px;height:16px;border:2px solid var(--border);border-top-color:var(--accent);border-radius:50%;animation:spin 0.8s linear infinite;vertical-align:middle;margin-right:6px}
@keyframes spin{to{transform:rotate(360deg)}}

/* Connection status bar - the rail's status sub-line: mono, glowing dot */
.conn-bar{padding:7px 20px;background:var(--bg-primary);border-bottom:1px solid var(--border);font-family:var(--f-m);font-size:0.72em;letter-spacing:0.04em;color:var(--text-secondary);display:flex;gap:16px;flex-wrap:wrap}
.conn-bar .tag{display:inline-flex;align-items:center;gap:5px}
.conn-bar .dot{width:7px;height:7px;border-radius:50%;flex-shrink:0}
.conn-bar .dot.on{background:var(--success);box-shadow:0 0 6px var(--success)}
.conn-bar .dot.off{background:var(--text-mute)}

/* Voice notes. Each note is the artboard item card: chamfered panel with a
   category-coloured left bar, display-face name, mono meta line. */
.vn-list{list-style:none}
.vn-item{background:var(--bg-secondary);border:1px solid var(--border);border-left:2px solid var(--accent);clip-path:var(--notch);padding:11px 12px;margin-bottom:8px}
.vn-top{display:flex;align-items:center;gap:8px}
.vn-chk{flex-shrink:0;width:16px;height:16px;accent-color:var(--accent);cursor:pointer}
.vn-name{flex:1;font-weight:700;font-size:0.9em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
.vn-acts{display:flex;gap:5px;flex-shrink:0}
.vn-acts .btn{text-decoration:none}
.vn-meta{font-family:var(--f-m);color:var(--text-mute);font-size:0.74em;margin:5px 0 2px;font-variant-numeric:tabular-nums}
.vn-item audio{width:100%;margin-top:6px;display:block;height:36px}
.vn-bulk{display:none;align-items:center;gap:10px;padding:9px 11px;margin-bottom:10px;background:var(--bg-elev);border:1px solid var(--border);font-family:var(--f-m);font-size:0.78em;flex-wrap:wrap}
.vn-bulk.show{display:flex}
.vn-selall{display:flex;align-items:center;gap:6px;cursor:pointer;color:var(--text-secondary)}
.vn-selall input{width:16px;height:16px;accent-color:var(--accent);cursor:pointer}
.vn-bulk .count{color:var(--text-mute)}
.vn-bulk .spacer{flex:1}
.btn:disabled{opacity:0.4;cursor:default}

/* Files browser tab */
.fb-chk{flex-shrink:0;width:16px;height:16px;accent-color:var(--accent);cursor:pointer}
.fb-path{display:flex;flex-wrap:wrap;align-items:center;gap:2px;font-family:var(--f-m);font-size:0.78em;background:var(--bg-sunk);border:1px solid var(--border);padding:8px 10px;margin-bottom:10px;color:var(--text-mute)}
.fb-crumb{color:var(--accent);cursor:pointer;padding:1px 4px}
.fb-crumb:hover{background:var(--accent-dim)}
.fb-crumb.cur{color:var(--text-primary);cursor:default}
.fb-crumb.cur:hover{background:none}
.fb-sep{opacity:0.5;padding:0 1px}
.fb-name{flex:1;font-weight:600;font-size:0.9em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
.fb-folder{flex:1;font-weight:700;font-size:0.9em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0;cursor:pointer;color:var(--accent);letter-spacing:0.04em}
.fb-folder:hover{text-decoration:underline}
.fb-ico{flex-shrink:0;width:18px;text-align:center;color:var(--text-mute)}

/* Download overlay (bulk zip / individual + progress) */
.dl-overlay{display:none;position:fixed;inset:0;background:rgba(8,5,15,0.86);z-index:320;justify-content:center;align-items:center;padding:16px}
.dl-overlay.show{display:flex}
.dl-box{background:var(--bg-secondary);border:1px solid var(--border);clip-path:var(--notch);padding:20px;width:340px;max-width:92vw}
.dl-box h3{font-size:0.95em;font-weight:700;letter-spacing:0.08em;text-transform:uppercase;color:var(--accent);margin-bottom:7px}
.dl-size{font-family:var(--f-m);font-size:0.78em;color:var(--text-secondary);margin-bottom:14px}
.dl-choice-btns{display:flex;flex-direction:column;gap:8px;margin-bottom:10px}
.dl-choice-btns .btn{padding:11px}
.dl-cancel{width:100%;margin-top:4px}
.dl-bar{height:8px;background:var(--bg-sunk);border:1px solid var(--border);overflow:hidden;margin:12px 0 8px}
.dl-fill{height:100%;width:0%;background:linear-gradient(90deg,var(--accent),var(--magenta));transition:width 0.15s}
.dl-stat{font-family:var(--f-m);font-size:0.82em;font-variant-numeric:tabular-nums}
.dl-rate{font-family:var(--f-m);font-size:0.76em;color:var(--accent);margin-top:3px;font-variant-numeric:tabular-nums}
.dl-file{font-family:var(--f-m);font-size:0.76em;color:var(--text-mute);margin-top:3px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
/* Caution notice, styled like the rail sheet's yellow-bordered panel */
.dl-warn{font-family:var(--f-m);font-size:0.74em;line-height:1.5;color:var(--warn);border:1px solid rgba(255,216,74,0.4);background:rgba(255,216,74,0.06);padding:7px 9px;margin:12px 0 10px}

/* Mobile */
@media(max-width:700px){
  .hamburger{display:flex}
  /* The menu button is fixed at the viewport's top-left and draws above the
     drawer, so the wordmark starts below it instead of under it. */
  .sidebar-brand{padding-top:58px}
  .sidebar{position:fixed;left:-240px;transition:left 0.25s;height:100vh}
  .sidebar.open{left:0}
  .sidebar-overlay.open{display:block}
  .main{width:100vw}
  .page-header{padding-left:56px}
  .player-bar{left:0}
  .content{padding:12px 14px;padding-bottom:130px}
  .track-table .col-album{display:none}
  /* Phone masthead: the status eyebrow is a long live string, so it drops
     its tracking here to stay on one or two tight lines beside the menu. */
  .page-header{padding-top:9px}
  .page-header h1{font-size:1.25em}
  .page-header .status{font-size:0.6em;letter-spacing:0.09em}
  .section-hdr{letter-spacing:0.1em}
}
@media(max-width:420px){
  .track-table .col-artist{display:none}
  .player-top{gap:8px}
}
</style>
</head>
<body>

<div class="hamburger" id="hamburger" onclick="toggleSidebar()">&#9776;</div>
<div class="sidebar-overlay" id="sidebarOverlay" onclick="toggleSidebar()"></div>

<div class="sidebar" id="sidebar">
  <div class="sidebar-brand">
    <div class="logo">
      <div class="logo-shadow">CYBER FIDGET</div>
      <div class="logo-main">CYBER FIDGET</div>
    </div>
    <div class="logo-sub">WEB PORTAL</div>
  </div>
  <div class="sidebar-nav">
    <div class="nav-item active" data-page="media" onclick="showPage('media')">
      <span class="icon">&#9835;</span> Media
    </div>
    <div class="nav-item" data-page="voice" onclick="showPage('voice')">
      <span class="icon">&#9679;</span> Voice notes
    </div>
    <div class="nav-item" data-page="files" onclick="showPage('files')">
      <span class="icon">&#9636;</span> Files
    </div>
    <div class="nav-item" onclick="window.location.href='/web/'">
      <span class="icon">&#9678;</span> Live listening
    </div>
    <div class="nav-item" data-page="settings" onclick="showPage('settings')">
      <span class="icon">&#9881;</span> Settings
    </div>
    <div class="nav-item" data-page="live" onclick="showPage('live')">
      <span class="icon">&#9733;</span> Live Playlist
      <span class="badge">Soon</span>
    </div>
  </div>
</div>

<div class="main">
  <div class="page-header">
    <h1 id="pageTitle">Media Manager</h1>
    <div class="status" id="statusBar">Loading...</div>
  </div>

  <div class="conn-bar" id="connBar">
    <span class="tag"><span class="dot on"></span> AP: <span id="connAP">192.168.4.1</span></span>
    <span class="tag" id="connSTATag" style="display:none"><span class="dot" id="connSTADot"></span> WiFi: <span id="connSTA">-</span></span>
  </div>

  <div class="banner" id="captiveBanner">
    <span>For uploads, open in your browser:</span>
    <span class="url">192.168.4.1</span>
    <span class="close-banner" onclick="this.parentElement.classList.remove('show')">&times;</span>
  </div>

  <div class="content" id="mediaPage">
    <div class="drop-zone" id="dropZone">
      <p class="main-text">Drop MP3 files here or tap to browse</p>
      <p class="hint">Supports multiple files</p>
      <div class="folder-sel" onclick="event.stopPropagation()">
        Upload to: <select id="uploadDir" onclick="event.stopPropagation()"><option value="/media">/media (root)</option></select>
      </div>
    </div>
    <input type="file" id="fileInput" accept=".mp3,audio/mpeg" multiple>

    <div class="upload-bar" id="uploadBar">
      <div class="bar"><div class="fill" id="uploadFill"></div></div>
      <div class="info">
        <span id="uploadName">...</span>
        <span id="uploadPct">0%</span>
      </div>
    </div>

    <div class="section-hdr">
      Files on device
      <div class="view-toggle">
        <span class="vt" data-view="tree" onclick="setView('tree')">Folders</span>
        <span class="vt active" data-view="table" onclick="setView('table')">All Tracks</span>
      </div>
      <span class="spacer"></span>
      <span class="action" onclick="createFolder()">+ Folder</span>
    </div>
    <div class="search-row" id="searchRow">
      <input type="text" id="searchInput" placeholder="Search tracks..." oninput="filterTracks()">
    </div>
    <ul class="file-tree" id="fileTree" style="display:none"><li class="empty">Loading...</li></ul>
    <table class="track-table show" id="trackTable">
      <thead><tr>
        <th class="sortable" onclick="sortTable('title')">Title <span id="sort_title" class="sort-arrow">&#9650;</span></th>
        <th class="col-artist sortable" onclick="sortTable('artist')">Artist <span id="sort_artist" class="sort-arrow"></span></th>
        <th class="col-album sortable" onclick="sortTable('album')">Album <span id="sort_album" class="sort-arrow"></span></th>
        <th class="col-size sortable" onclick="sortTable('size')">Size <span id="sort_size" class="sort-arrow"></span></th>
        <th class="col-acts"></th>
      </tr></thead>
      <tbody id="trackBody"></tbody>
    </table>

    <div class="section-hdr" style="margin-top:24px">
      Playlists
      <span class="spacer"></span>
      <span class="action" onclick="createPlaylist()">+ Playlist</span>
    </div>
    <div class="playlists" id="playlistList"></div>
  </div>

  <div class="content" id="voicePage" style="display:none">
    <div class="section-hdr">
      Voice notes
      <span class="spacer"></span>
      <span class="action" onclick="loadVoiceNotes()">Refresh</span>
    </div>
    <div class="vn-bulk" id="vnBulk">
      <label class="vn-selall"><input type="checkbox" id="vnSelAll" onclick="vnToggleAll(this)"> Select all</label>
      <span class="count" id="vnBulkCount">0 selected</span>
      <span class="spacer"></span>
      <button class="btn btn-play btn-sm" id="vnDlBtn" onclick="vnDownloadSelected()" disabled>Download</button>
      <button class="btn btn-del btn-sm" id="vnDelBtn" onclick="vnDeleteSelected()" disabled>Delete</button>
    </div>
    <ul class="vn-list" id="vnList"><li class="empty">Loading...</li></ul>
  </div>

  <div class="content" id="filesPage" style="display:none">
    <div class="section-hdr">
      Files
      <span class="spacer"></span>
      <span class="action" onclick="fbNewFolder()">+ Folder</span>
      <span class="action" onclick="loadBrowse(fbPath)">Refresh</span>
    </div>
    <div class="fb-path" id="fbPath"></div>
    <div class="drop-zone" id="fbDrop">
      <p class="main-text">Drop files here to add them to this folder</p>
      <p class="hint">Or tap to choose files</p>
    </div>
    <input type="file" id="fbInput" multiple>
    <div class="upload-bar" id="fbUploadBar">
      <div class="bar"><div class="fill" id="fbUploadFill"></div></div>
      <div class="info"><span id="fbUploadName">...</span><span id="fbUploadPct">0%</span></div>
    </div>
    <div class="vn-bulk" id="fbBulk">
      <label class="vn-selall"><input type="checkbox" id="fbSelAll" onclick="fbToggleAll(this)"> Select all</label>
      <span class="count" id="fbBulkCount">0 selected</span>
      <span class="spacer"></span>
      <button class="btn btn-play btn-sm" id="fbDlBtn" onclick="fbDownloadSelected()" disabled>Download</button>
      <button class="btn btn-del btn-sm" id="fbDelBtn" onclick="fbDeleteSelected()" disabled>Delete</button>
    </div>
    <ul class="vn-list" id="fbList"><li class="empty">Loading...</li></ul>
  </div>

  <div class="content" id="settingsPage" style="display:none">
    <div class="wifi-card" id="wifiStatusCard">
      <h3>WiFi Connection</h3>
      <div id="wifiStatusContent">
        <div class="info-row"><span class="label">Status</span><span class="val" id="wifiStatusText">Not connected</span></div>
      </div>
    </div>

    <div class="wifi-card">
      <h3>Available Networks</h3>
      <div style="display:flex;gap:8px;margin-bottom:8px">
        <button class="btn btn-accent" onclick="scanWifi()" id="scanBtn">Scan for Networks</button>
      </div>
      <ul class="network-list" id="networkList"></ul>
      <div id="wifiConnectForm" style="display:none">
        <div style="font-size:0.85em;margin-bottom:4px;color:var(--text-secondary)">Connecting to: <strong id="selectedSSID" style="color:var(--text-primary)"></strong></div>
        <input type="password" class="wifi-input" id="wifiPass" placeholder="Password (leave empty for open network)">
        <div class="wifi-actions">
          <button class="btn" onclick="cancelWifiConnect()">Cancel</button>
          <button class="btn btn-accent" onclick="doWifiConnect()" id="connectBtn">Connect</button>
        </div>
      </div>
    </div>

    <div class="wifi-card">
      <h3>Access Point</h3>
      <div class="info-row"><span class="label">SSID</span><span class="val">CyberFidget</span></div>
      <div class="info-row"><span class="label">IP</span><span class="val accent" id="apIPDisplay">192.168.4.1</span></div>
      <div class="info-row"><span class="label">Status</span><span class="val" style="color:var(--success)">Always available</span></div>
    </div>
  </div>
  <div class="content" id="livePage" style="display:none">
    <div class="empty">Live collaborative playlist coming soon.</div>
  </div>
</div>

<div class="player-bar" id="playerBar">
  <div class="player-top">
    <div class="player-controls">
      <button onclick="playerPrev()" title="Restart">&#9664;&#9664;</button>
      <button id="btnPlay" onclick="playerToggle()" title="Play/Pause">&#9654;</button>
      <button onclick="playerNext()" title="Next">&#9654;&#9654;</button>
    </div>
    <div class="player-info">
      <div class="title" id="pTitle">-</div>
      <div class="artist" id="pArtist">-</div>
    </div>
  </div>
  <div class="player-bottom">
    <span class="time" id="pCur">0:00</span>
    <div class="seek-wrap" id="seekWrap">
      <div class="seek-track">
        <div class="seek-fill" id="seekFill"></div>
      </div>
      <div class="seek-thumb" id="seekThumb"></div>
    </div>
    <span class="time right" id="pDur">0:00</span>
  </div>
</div>

<div class="pl-dropdown" id="plDropdown"></div>

<div class="modal-overlay" id="modalOverlay">
  <div class="modal" id="modalBox">
    <h3 id="modalTitle">New Folder</h3>
    <input type="text" id="modalInput" placeholder="Name...">
    <div class="modal-err" id="modalErr"></div>
    <div id="modalExtra"></div>
    <div class="modal-actions">
      <button class="btn" onclick="closeModal()">Cancel</button>
      <button class="btn btn-accent" id="modalOk" onclick="modalConfirm()">Create</button>
    </div>
  </div>
</div>

<div class="dl-overlay" id="dlOverlay">
  <div class="dl-box">
    <div id="dlChoice">
      <h3 id="dlTitle">Download files</h3>
      <div class="dl-size" id="dlSize">Total: 0 B</div>
      <div class="dl-choice-btns">
        <button class="btn btn-accent" id="dlZipBtn" onclick="dlStart('zip')">Bundle as one .zip</button>
        <button class="btn" id="dlIndivBtn" onclick="dlStart('individual')">Individual files</button>
      </div>
      <button class="btn dl-cancel" onclick="dlClose()">Cancel</button>
    </div>
    <div id="dlProgress" style="display:none">
      <h3 id="dlProgTitle">Downloading...</h3>
      <div class="dl-bar"><div class="dl-fill" id="dlFill"></div></div>
      <div class="dl-stat" id="dlStat">0 B / 0 B</div>
      <div class="dl-rate" id="dlRate"></div>
      <div class="dl-file" id="dlFile"></div>
      <div class="dl-warn">Keep this page open until it finishes.</div>
      <button class="btn dl-cancel" onclick="dlCancel()">Cancel</button>
    </div>
  </div>
</div>

<div class="toast" id="toast"></div>

<audio id="audio" preload="none"></audio>

<script>
// ─── State ───
let files=[], playlists=[], queue=[], queueIdx=-1, currentView='table';
let flatTracks=[], sortCol='title', sortAsc=true, searchQuery='';
let allFolders=[]; // cached folder paths for move/upload
let nowPlayingUrl='', playingSource=''; // 'tracks' or 'playlist'
const audio=document.getElementById('audio');
const $=id=>document.getElementById(id);

// ─── Utilities ───
function fmt(b){if(b<1024)return b+' B';if(b<1048576)return(b/1024).toFixed(1)+' KB';return(b/1048576).toFixed(1)+' MB'}
function fmtTime(s){if(isNaN(s))return'0:00';s=Math.floor(s);return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')}
function basename(p){const i=p.lastIndexOf('/');return i>=0?p.substring(i+1):p}
function stripExt(n){const i=n.lastIndexOf('.');return i>0?n.substring(0,i):n}
function toast(msg){const t=$('toast');t.textContent=msg;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2000)}
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;')}

// ─── Sidebar / Hamburger ───
function toggleSidebar(){
  $('sidebar').classList.toggle('open');
  $('sidebarOverlay').classList.toggle('open');
}
function showPage(p){
  document.querySelectorAll('.nav-item').forEach(n=>n.classList.toggle('active',n.dataset.page===p));
  $('mediaPage').style.display=p==='media'?'':'none';
  $('voicePage').style.display=p==='voice'?'':'none';
  $('filesPage').style.display=p==='files'?'':'none';
  $('settingsPage').style.display=p==='settings'?'':'none';
  $('livePage').style.display=p==='live'?'':'none';
  $('pageTitle').textContent=p==='media'?'Media Manager':p==='voice'?'Voice Notes':p==='files'?'Files':p==='settings'?'Settings':'Live Playlist';
  if(p==='voice')loadVoiceNotes();
  if(p==='files')loadBrowse(fbPath);
  if(window.innerWidth<=700)toggleSidebar();
}

// ─── View Toggle ───
function setView(v){
  currentView=v;
  document.querySelectorAll('.view-toggle .vt').forEach(e=>e.classList.toggle('active',e.dataset.view===v));
  $('fileTree').style.display=v==='tree'?'':'none';
  $('trackTable').classList.toggle('show',v==='table');
  $('searchRow').classList.toggle('show',v==='table');
}

// ─── File Loading ───
async function loadFiles(){
  try{
    const r=await fetch('/api/files');
    files=await r.json();
    allFolders=['/media'];
    collectFolders(files,'');
    renderTree(files,$('fileTree'),'');
    updateUploadDirSelect();
  }catch(e){$('fileTree').innerHTML='<li class="empty">Error loading files</li>'}
  loadStatus();
  loadTracks();
}
function collectFolders(items,prefix){
  for(const f of items){
    if(f.type==='dir'){
      const p=prefix+'/'+f.name;
      allFolders.push('/media'+p);
      if(f.children)collectFolders(f.children,p);
    }
  }
}
function updateUploadDirSelect(){
  const sel=$('uploadDir');
  const cur=sel.value;
  sel.innerHTML=allFolders.map(f=>'<option value="'+esc(f)+'">'+esc(f.replace('/media','/ (root)').replace(/^\/ \(root\)\//,'/'))+'</option>').join('');
  sel.value=cur||'/media';
}

// ─── Tree View ───
function renderTree(items,el,prefix){
  if(!items.length){el.innerHTML='<li class="empty">No files yet — drop some MP3s above!</li>';return}
  let h='';
  items.sort((a,b)=>{if(a.type!==b.type)return a.type==='dir'?-1:1;return a.name.localeCompare(b.name)});
  for(const f of items){
    const path=prefix+'/'+f.name;
    if(f.type==='dir'){
      const id='d_'+btoa(unescape(encodeURIComponent(path))).replace(/[^a-zA-Z0-9]/g,'');
      h+='<li class="folder-toggle" onclick="togDir(\''+id+'\')">';
      h+='<span class="icon" id="'+id+'_i">&#9654;</span>';
      h+='<span class="name">'+esc(f.name)+'/</span>';
      h+='<span class="size">'+(f.children?f.children.length:0)+' items</span>';
      h+='<span class="acts">';
      h+='<button class="btn btn-del btn-sm" onclick="event.stopPropagation();delPath(\''+esc(path)+'\',true)">Del</button>';
      h+='</span></li>';
      h+='<ul class="file-tree sub" id="'+id+'" style="display:none">';
      if(f.children&&f.children.length){const t=document.createElement('ul');renderTree(f.children,t,path);h+=t.innerHTML}
      else h+='<li class="empty">Empty folder</li>';
      h+='</ul>';
    }else{
      const mediaPath='/media'+path;
      const safeName=esc(f.name);
      const safePath=esc(path);
      h+='<li>';
      h+='<span class="icon">&#9835;</span>';
      h+='<span class="name" title="'+safePath+'">'+safeName+'</span>';
      h+='<span class="size">'+fmt(f.size||0)+'</span>';
      h+='<span class="acts">';
      h+='<button class="btn btn-play btn-sm" onclick="event.stopPropagation();playTrack(\''+esc(mediaPath)+'\',\''+safeName+'\',\'\',\'tracks\')">&#9654;</button>';
      h+='<button class="btn btn-add btn-sm" onclick="event.stopPropagation();showAddToPl(event,\''+esc(mediaPath)+'\')">+PL</button>';
      h+='<button class="btn btn-move btn-sm" onclick="event.stopPropagation();moveFile(\''+safePath+'\')">Move</button>';
      h+='<button class="btn btn-del btn-sm" onclick="event.stopPropagation();delPath(\''+safePath+'\')">Del</button>';
      h+='</span></li>';
    }
  }
  el.innerHTML=h;
}
function togDir(id){
  const el=$(id),ic=$(id+'_i');
  if(el.style.display==='none'){el.style.display='';ic.innerHTML='&#9660;'}
  else{el.style.display='none';ic.innerHTML='&#9654;'}
}

// ─── Table View (flat, with ID3 metadata from /api/tracks) ───
async function loadTracks(){
  try{
    const r=await fetch('/api/tracks');
    flatTracks=(await r.json()).map(t=>({
      name:basename(t.path),path:t.path,
      folder:t.path.substring(0,t.path.lastIndexOf('/')),
      title:t.title||stripExt(basename(t.path)),
      artist:t.artist||'-',album:t.album||'-',size:t.size||0
    }));
    applyTableSort();
  }catch(e){$('trackBody').innerHTML='<tr><td colspan="5" class="empty">Error loading tracks</td></tr>'}
}
function applyTableSort(){
  let list=flatTracks.slice();
  if(searchQuery){
    const q=searchQuery.toLowerCase();
    list=list.filter(t=>t.title.toLowerCase().includes(q)||t.artist.toLowerCase().includes(q)||t.album.toLowerCase().includes(q)||t.name.toLowerCase().includes(q));
  }
  list.sort((a,b)=>{
    let va,vb;
    if(sortCol==='size'){va=a.size;vb=b.size}
    else{va=(a[sortCol]||'').toLowerCase();vb=(b[sortCol]||'').toLowerCase()}
    let r=va<vb?-1:va>vb?1:0;
    return sortAsc?r:-r;
  });
  ['title','artist','album','size'].forEach(c=>{
    const el=$('sort_'+c);
    if(!el)return;
    el.className='sort-arrow'+(c===sortCol?(sortAsc?' asc':' desc'):'');
    el.innerHTML=c===sortCol?(sortAsc?'&#9650;':'&#9660;'):'';
  });
  const tb=$('trackBody');
  if(!list.length){tb.innerHTML='<tr><td colspan="5" class="empty">'+(searchQuery?'No matching tracks':'No tracks')+'</td></tr>';return}
  tb.innerHTML=list.map(t=>{
    return '<tr data-path="'+esc(t.path)+'">'+
      '<td class="col-title" title="'+esc(t.path)+'">'+esc(t.title)+'</td>'+
      '<td class="col-artist">'+esc(t.artist)+'</td>'+
      '<td class="col-album">'+esc(t.album)+'</td>'+
      '<td class="col-size">'+fmt(t.size)+'</td>'+
      '<td class="col-acts"><span class="acts">'+
        '<button class="btn btn-play btn-sm" onclick="playTrack(\''+esc(t.path)+'\',\''+esc(t.title)+'\',\''+esc(t.artist)+'\',\'tracks\')">&#9654;</button>'+
        '<button class="btn btn-add btn-sm" onclick="showAddToPl(event,\''+esc(t.path)+'\')">+PL</button>'+
        '<button class="btn btn-del btn-sm" onclick="delPath(\''+esc(t.path)+'\')">Del</button>'+
      '</span></td></tr>';
  }).join('');
  highlightPlaying();
}
function sortTable(col){
  if(sortCol===col)sortAsc=!sortAsc;
  else{sortCol=col;sortAsc=true}
  applyTableSort();
}
function filterTracks(){
  searchQuery=$('searchInput').value.trim();
  applyTableSort();
}

// ─── Status ───
async function loadStatus(){
  try{
    const r=await fetch('/api/status');const s=await r.json();
    // Middot separators, matching the artboards' meta lines ("App · 28 KB").
    $('statusBar').textContent=s.files+' files · '+fmt(s.usedBytes)+' of '+fmt(s.totalBytes)+' used · '+s.clients+' client(s)';
  }catch(e){}
}

// ─── Device clock ───
// The Cyber Fidget has no battery clock, so until something sets it, voice
// notes are stamped "No date". Send the browser's wall-clock on load, adjusted
// by the timezone offset so the device records your *local* time (the device
// keeps time as UTC). A note made after this carries a real timestamp.
async function setDeviceClock(){
  try{
    const localMs=Date.now()-new Date().getTimezoneOffset()*60000;
    await fetch('/api/time?ms='+localMs,{method:'POST'});
  }catch(e){}
}

// ─── Voice Notes ───
let voiceNotes=[];
function fmtNoteDate(ts){
  // "2026-06-09T14:23:11" -> "2026-06-09 14:23"; blank -> "No date"
  if(!ts)return'No date';
  return ts.replace('T',' ').slice(0,16);
}
function vnResetBulk(){
  $('vnBulk').classList.remove('show');
  $('vnSelAll').checked=false;$('vnSelAll').indeterminate=false;
}
async function loadVoiceNotes(){
  const el=$('vnList');
  vnResetBulk();
  el.innerHTML='<li class="empty">Loading...</li>';
  try{
    const r=await fetch('/api/recordings');
    const d=await r.json();
    if(!d.sd){el.innerHTML='<li class="empty">Insert a memory card to see your voice notes.</li>';return}
    voiceNotes=d.items||[];
    renderVoiceNotes();
  }catch(e){el.innerHTML='<li class="empty">Could not load voice notes.</li>'}
}
function renderVoiceNotes(){
  const el=$('vnList');
  // Newest first: REC_NNNN names are monotonic, so a reverse name sort works
  // even when the clock was unset and timestamps are blank.
  const list=voiceNotes.slice().sort((a,b)=>b.name.localeCompare(a.name));
  if(!list.length){vnResetBulk();el.innerHTML='<li class="empty">No voice notes yet. Record one with the Voice Notes app on your Cyber Fidget, then tap Refresh.</li>';return}
  el.innerHTML=list.map(n=>{
    const url='/recordings/'+encodeURIComponent(n.name);
    const meta=[fmtNoteDate(n.timestamp),fmtTime(n.duration),fmt(n.bytes)].join(' · ');
    return '<li class="vn-item">'+
      '<div class="vn-top">'+
        '<input type="checkbox" class="vn-chk" data-name="'+esc(n.name)+'" onchange="vnSelChange()">'+
        '<span class="vn-name" title="'+esc(n.name)+'">'+esc(stripExt(n.name))+'</span>'+
        '<span class="vn-acts">'+
          '<a class="btn btn-play btn-sm" href="'+url+'" download="'+esc(n.name)+'">Download</a>'+
          '<button class="btn btn-move btn-sm" onclick="renameNote(\''+esc(n.name)+'\')">Rename</button>'+
          '<button class="btn btn-del btn-sm" onclick="deleteNote(\''+esc(n.name)+'\')">Delete</button>'+
        '</span>'+
      '</div>'+
      '<div class="vn-meta">'+esc(meta)+'</div>'+
      '<audio controls preload="none" src="'+url+'"></audio>'+
    '</li>';
  }).join('');
  $('vnBulk').classList.add('show');
  vnSelChange();
}

// ─── Voice Notes: multi-select ───
function vnSelectedNames(){return [...document.querySelectorAll('.vn-chk:checked')].map(c=>c.dataset.name)}
function vnSelChange(){
  const chks=[...document.querySelectorAll('.vn-chk')];
  const sel=chks.filter(c=>c.checked).length;
  $('vnBulkCount').textContent=sel+' selected';
  $('vnDlBtn').disabled=sel===0;
  $('vnDelBtn').disabled=sel===0;
  const all=$('vnSelAll');
  all.checked=chks.length>0&&sel===chks.length;
  all.indeterminate=sel>0&&sel<chks.length;
}
function vnToggleAll(cb){
  document.querySelectorAll('.vn-chk').forEach(c=>c.checked=cb.checked);
  vnSelChange();
}
// ─── Bulk download (client-side zip OR individual WAVs, with progress) ───
// The device stays a dumb one-file-at-a-time server (serveStatic); all bundling
// happens here in the browser. iOS Safari can't do multi-file downloads (one per
// user gesture + mis-named blobs), so on iOS we offer ONLY the single .zip; other
// platforms can pick zip or individual WAVs. Both fetch sequentially (the server
// wedges on concurrent streams — see the device notes) with a live byte progress
// bar, and a beforeunload guard keeps the user on the page until it finishes.
function dlDeviceClass(){
  const ua=navigator.userAgent||'';
  if(/iP(hone|ad|od)/.test(ua)||(navigator.platform==='MacIntel'&&navigator.maxTouchPoints>1))return 'ios';
  if(/Android/.test(ua))return 'android';
  return 'desktop';
}
// Conservative per-device caps for the in-browser zip (it holds the selected
// bytes + the assembled blob in memory). iOS is the tightest; tune after testing.
const DL_CAP={ios:100*1048576,android:200*1048576,desktop:500*1048576};

// A download job is a list of items: {url, name, bytes, type}. Voice notes and
// the Files browser both feed this same serialized fetch->blob->zip path, so the
// device only ever sees one transfer at a time (the async server wedges on
// concurrency — see the device notes).
let _dlItems=[],_dlZipName='files.zip',_dlTotal=0,_dlAbort=null,_dlActive=false,_dlStartMs=0;
function dlFmtEta(s){return s>=60?(Math.floor(s/60)+'m '+String(s%60).padStart(2,'0')+'s'):(s+'s')}

let _crcTab=null;
function crc32Update(crc,buf){
  if(!_crcTab){_crcTab=new Uint32Array(256);for(let n=0;n<256;n++){let c=n;for(let k=0;k<8;k++)c=(c&1)?(0xEDB88320^(c>>>1)):(c>>>1);_crcTab[n]=c>>>0;}}
  for(let i=0;i<buf.length;i++)crc=(_crcTab[(crc^buf[i])&0xFF]^(crc>>>8))>>>0;
  return crc>>>0;
}
function zipLocal(nameBytes,crc,size){
  const h=new DataView(new ArrayBuffer(30));
  h.setUint32(0,0x04034b50,true);h.setUint16(4,20,true);h.setUint16(6,0x0800,true);h.setUint16(8,0,true);
  h.setUint16(10,0,true);h.setUint16(12,0x21,true);h.setUint32(14,crc,true);
  h.setUint32(18,size,true);h.setUint32(22,size,true);h.setUint16(26,nameBytes.length,true);h.setUint16(28,0,true);
  return new Uint8Array(h.buffer);
}
function zipCentral(nameBytes,crc,size,offset){
  const h=new DataView(new ArrayBuffer(46));
  h.setUint32(0,0x02014b50,true);h.setUint16(4,20,true);h.setUint16(6,20,true);h.setUint16(8,0x0800,true);
  h.setUint16(10,0,true);h.setUint16(12,0,true);h.setUint16(14,0x21,true);h.setUint32(16,crc,true);
  h.setUint32(20,size,true);h.setUint32(24,size,true);h.setUint16(28,nameBytes.length,true);
  h.setUint16(30,0,true);h.setUint16(32,0,true);h.setUint16(34,0,true);h.setUint16(36,0,true);
  h.setUint32(38,0,true);h.setUint32(42,offset,true);
  return new Uint8Array(h.buffer);
}
function zipEOCD(count,cdSize,cdOffset){
  const h=new DataView(new ArrayBuffer(22));
  h.setUint32(0,0x06054b50,true);h.setUint16(4,0,true);h.setUint16(6,0,true);
  h.setUint16(8,count,true);h.setUint16(10,count,true);h.setUint32(12,cdSize,true);
  h.setUint32(16,cdOffset,true);h.setUint16(20,0,true);
  return new Uint8Array(h.buffer);
}
// Read one item as a Uint8Array, streaming so we can show byte progress + CRC it.
async function dlFetchBytes(item,baseReceived){
  const resp=await fetch(item.url,{signal:_dlAbort.signal});
  if(!resp.ok)throw new Error('HTTP '+resp.status);
  const reader=resp.body.getReader();
  const chunks=[];let size=0;let crc=0xFFFFFFFF;
  for(;;){
    const r=await reader.read();
    if(r.done)break;
    chunks.push(r.value);size+=r.value.length;crc=crc32Update(crc,r.value);
    dlSetProgress(baseReceived+size);
  }
  const data=new Uint8Array(size);let p=0;for(const c of chunks){data.set(c,p);p+=c.length;}
  return {data,crc:(crc^0xFFFFFFFF)>>>0};
}
async function dlZip(){
  const enc=new TextEncoder();
  const parts=[],central=[];let offset=0,received=0;
  for(let i=0;i<_dlItems.length;i++){
    dlSetFile(i,_dlItems[i].name);
    const nameBytes=enc.encode(_dlItems[i].name);
    const {data,crc}=await dlFetchBytes(_dlItems[i],received);
    received+=data.length;
    const lh=zipLocal(nameBytes,crc,data.length);
    parts.push(lh,nameBytes,data);
    central.push({nameBytes,crc,size:data.length,offset});
    offset+=lh.length+nameBytes.length+data.length;
  }
  let cdSize=0;for(const e of central){const ch=zipCentral(e.nameBytes,e.crc,e.size,e.offset);parts.push(ch,e.nameBytes);cdSize+=ch.length+e.nameBytes.length;}
  parts.push(zipEOCD(central.length,cdSize,offset));
  dlSave(new Blob(parts,{type:'application/zip'}),_dlZipName);
}
async function dlIndividual(){
  let received=0;
  for(let i=0;i<_dlItems.length;i++){
    const it=_dlItems[i];
    dlSetFile(i,it.name);
    const {data}=await dlFetchBytes(it,received);
    received+=data.length;
    dlSave(new Blob([data],{type:it.type||'application/octet-stream'}),it.name);  // typed so iOS/Safari names it correctly, not .txt
    await new Promise(r=>setTimeout(r,250));                                       // let the server release the socket
  }
}
function dlSave(blob,filename){
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');a.href=url;a.download=filename;
  document.body.appendChild(a);a.click();a.remove();
  setTimeout(()=>URL.revokeObjectURL(url),4000);
}
function dlSetProgress(received){
  const pct=_dlTotal>0?Math.min(100,Math.round(received/_dlTotal*100)):0;
  $('dlFill').style.width=pct+'%';
  $('dlStat').textContent=fmt(received)+' / '+fmt(_dlTotal)+' ('+pct+'%)';
  const el=(Date.now()-_dlStartMs)/1000;
  if(el>0.4&&received>0){
    const rate=received/el;                          // bytes/sec
    const eta=rate>0?Math.round(Math.max(0,_dlTotal-received)/rate):0;
    $('dlRate').textContent=fmt(rate)+'/s'+(received<_dlTotal?'  ·  ~'+dlFmtEta(eta)+' left':'');
  }
}
function dlSetFile(i,name){$('dlFile').textContent='File '+(i+1)+' of '+_dlItems.length+': '+stripExt(name)}

// Open the download chooser for a set of items. label is the plural noun shown in
// the title ("voice notes" / "files"); zipName is the bundle filename. Both Voice
// notes and the Files browser call this.
function dlBegin(items,zipName,label){
  if(!items.length)return;
  _dlItems=items;_dlZipName=zipName;
  _dlTotal=items.reduce((s,it)=>s+(it.bytes||0),0);
  const dev=dlDeviceClass(),cap=DL_CAP[dev]||DL_CAP.desktop;
  const over=_dlTotal>cap;
  $('dlTitle').textContent='Download '+items.length+' '+label;
  $('dlSize').textContent='Total: '+fmt(_dlTotal)+(over?'  (too large to bundle on this device, ~'+fmt(cap)+' max)':'');
  $('dlZipBtn').disabled=over;
  $('dlIndivBtn').style.display=dev==='ios'?'none':'';   // iOS: zip only
  if(dev==='ios'&&over){
    $('dlSize').textContent='Total: '+fmt(_dlTotal)+' is too large to bundle here. Download files one at a time using the Download button on each one.';
  }
  $('dlChoice').style.display='';$('dlProgress').style.display='none';
  $('dlOverlay').classList.add('show');
}
function vnDownloadSelected(){
  const names=vnSelectedNames();
  if(!names.length)return;
  const items=names.map(n=>{
    const it=voiceNotes.find(v=>v.name===n);
    return {url:'/recordings/'+encodeURIComponent(n),name:n,bytes:(it?it.bytes:0)+44,type:'audio/wav'};
  });
  dlBegin(items,'voice-notes.zip','voice notes');
}
async function dlStart(mode){
  $('dlChoice').style.display='none';$('dlProgress').style.display='';
  $('dlProgTitle').textContent=mode==='zip'?'Building your .zip...':'Downloading...';
  $('dlFill').style.width='0%';$('dlRate').textContent='';
  _dlStartMs=Date.now();dlSetProgress(0);
  _dlAbort=new AbortController();_dlActive=true;
  try{
    if(mode==='zip')await dlZip();else await dlIndividual();
    toast(mode==='zip'?'Bundle ready':'Downloads done');
  }catch(e){
    if(e&&e.name==='AbortError')toast('Download cancelled');
    else toast('Download failed'+(e&&e.message?': '+e.message:''));
  }finally{
    _dlActive=false;dlClose();
  }
}
function dlCancel(){if(_dlAbort)_dlAbort.abort()}
function dlClose(){
  $('dlOverlay').classList.remove('show');
  $('dlChoice').style.display='';$('dlProgress').style.display='none';
  $('dlFill').style.width='0%';$('dlRate').textContent='';
}
// Keep the user on the page while a transfer is in flight (it lives in JS here).
window.addEventListener('beforeunload',e=>{if(_dlActive){e.preventDefault();e.returnValue=''}});
async function vnDeleteSelected(){
  const names=vnSelectedNames();
  if(!names.length)return;
  if(!confirm('Delete '+names.length+' voice note'+(names.length>1?'s':'')+'?'))return;
  let ok=0;
  for(const n of names){
    try{
      const r=await fetch('/api/delete?path='+encodeURIComponent('/recordings/'+n),{method:'POST'});
      if(r.ok)ok++;
    }catch(e){}
  }
  toast('Deleted '+ok+' of '+names.length);
  loadVoiceNotes();
}
async function deleteNote(name){
  if(!confirm('Delete voice note "'+stripExt(name)+'"?'))return;
  try{
    const r=await fetch('/api/delete?path='+encodeURIComponent('/recordings/'+name),{method:'POST'});
    if(!r.ok){alert('Delete failed: '+await r.text());return}
    toast('Deleted '+stripExt(name));
    loadVoiceNotes();
  }catch(e){alert('Error: '+e)}
}
function renameNote(name){
  $('modalTitle').textContent='Rename Voice Note';
  $('modalInput').style.display='';
  $('modalInput').value=stripExt(name);
  $('modalInput').placeholder='Name...';
  $('modalInput').maxLength=48;   // block over-typing rather than rejecting on confirm
  $('modalExtra').innerHTML='';
  $('modalOk').textContent='Rename';
  // Live-validate characters as the user types (length is capped by maxLength).
  modalValidator=(v)=>(/[\\\/:*?"<>|]/.test(stripExt((v||'').trim())))
    ? 'Name cannot contain \\ / : * ? " < > |' : '';
  modalShowErr('');
  $('modalOverlay').classList.add('active');
  $('modalInput').focus();
  $('modalInput').select();
  modalCallback=async(v)=>{
    const base=stripExt((v||'').trim());
    if(!base)return;
    const to='/recordings/'+base+'.wav';
    if(to==='/recordings/'+name)return;
    try{
      const r=await fetch('/api/move?from='+encodeURIComponent('/recordings/'+name)+'&to='+encodeURIComponent(to),{method:'POST'});
      if(r.ok){toast('Renamed');loadVoiceNotes()}
      else alert('Rename failed: '+await r.text());
    }catch(e){alert('Error: '+e)}
  };
}

// ─── Files browser (raw card, nested folder navigation) ───
// A power-user view of the whole SD card: navigate into one folder at a time
// (NOT a flat dump of every file), see size + modified date, multi-select,
// upload into the current folder, rename, and delete (files and whole folders).
// Reuses the same serialized download/zip path as Voice notes (dlBegin).
let fbPath='/', fbEntries=[];
function fbJoin(name){return (fbPath==='/'?'':fbPath)+'/'+name}
function fbDate(mt){
  if(!mt||mt<1600000000)return'No date';
  // The device stores time as local-naive seconds (see the clock-set note), so
  // read the epoch back with UTC getters to show that wall-clock time unshifted.
  const d=new Date(mt*1000),p=n=>String(n).padStart(2,'0');
  return d.getUTCFullYear()+'-'+p(d.getUTCMonth()+1)+'-'+p(d.getUTCDate())+' '+p(d.getUTCHours())+':'+p(d.getUTCMinutes());
}
function fbResetBulk(){
  $('fbBulk').classList.remove('show');
  $('fbSelAll').checked=false;$('fbSelAll').indeterminate=false;
}
function renderCrumbs(){
  const parts=fbPath.split('/').filter(Boolean);
  let acc='',h='<span class="fb-crumb'+(fbPath==='/'?' cur':'')+'" onclick="loadBrowse(\'/\')">card</span>';
  parts.forEach((p,i)=>{
    acc+='/'+p;
    const cur=i===parts.length-1;
    h+='<span class="fb-sep">/</span><span class="fb-crumb'+(cur?' cur':'')+'" data-p="'+esc(acc)+'" onclick="fbCrumb(this)">'+esc(p)+'</span>';
  });
  $('fbPath').innerHTML=h;
}
function fbCrumb(el){loadBrowse(el.dataset.p)}
async function loadBrowse(path){
  fbPath=path||'/';
  fbResetBulk();
  renderCrumbs();
  $('fbList').innerHTML='<li class="empty">Loading...</li>';
  try{
    const r=await fetch('/api/browse?path='+encodeURIComponent(fbPath));
    const d=await r.json();
    if(!d.sd){$('fbList').innerHTML='<li class="empty">Insert a memory card to browse files.</li>';return}
    fbEntries=d.entries||[];
    renderBrowse();
  }catch(e){$('fbList').innerHTML='<li class="empty">Could not read this folder.</li>'}
}
function renderBrowse(){
  const el=$('fbList');
  // Folders first, then alphabetical — the order a user expects in a file list.
  const list=fbEntries.slice().sort((a,b)=>{if(a.type!==b.type)return a.type==='dir'?-1:1;return a.name.localeCompare(b.name)});
  if(!list.length){fbResetBulk();el.innerHTML='<li class="empty">This folder is empty. Drop files above to add some.</li>';return}
  el.innerHTML=list.map(e=>{
    const isDir=e.type==='dir',full=fbJoin(e.name);
    const meta=isDir?'Folder':(fmt(e.size)+' · '+fbDate(e.mtime));
    const ico=isDir?'&#9656;':'&#9642;';   // monochrome: small triangle = folder, small square = file
    const nameCell=isDir
      ?'<span class="fb-folder" onclick="fbOpen(this)">'+ico+' '+esc(e.name)+'</span>'
      :'<span class="fb-name"><span class="fb-ico">'+ico+'</span> '+esc(e.name)+'</span>';
    return '<li class="vn-item" data-path="'+esc(full)+'" data-name="'+esc(e.name)+'" data-dir="'+(isDir?1:0)+'">'+
      '<div class="vn-top">'+
        '<input type="checkbox" class="fb-chk" data-path="'+esc(full)+'" data-dir="'+(isDir?1:0)+'" onchange="fbSelChange()">'+
        nameCell+
        '<span class="vn-acts">'+
          (isDir?'':'<a class="btn btn-play btn-sm" href="/api/download?path='+encodeURIComponent(full)+'" download="'+esc(e.name)+'">Download</a>')+
          '<button class="btn btn-move btn-sm" onclick="fbRename(this)">Rename</button>'+
          '<button class="btn btn-del btn-sm" onclick="fbDelete(this)">Delete</button>'+
        '</span>'+
      '</div>'+
      '<div class="vn-meta">'+esc(meta)+'</div>'+
    '</li>';
  }).join('');
  $('fbBulk').classList.add('show');
  fbSelChange();
}
function fbOpen(el){loadBrowse(el.closest('li').dataset.path)}
function fbSelChange(){
  const chks=[...document.querySelectorAll('.fb-chk')];
  const sel=chks.filter(c=>c.checked).length;
  $('fbBulkCount').textContent=sel+' selected';
  $('fbDlBtn').disabled=sel===0;
  $('fbDelBtn').disabled=sel===0;
  const all=$('fbSelAll');
  all.checked=chks.length>0&&sel===chks.length;
  all.indeterminate=sel>0&&sel<chks.length;
}
function fbToggleAll(cb){document.querySelectorAll('.fb-chk').forEach(c=>c.checked=cb.checked);fbSelChange()}
function fbSelected(){return [...document.querySelectorAll('.fb-chk:checked')].map(c=>c.dataset)}
function fbDownloadSelected(){
  const files=fbSelected().filter(d=>d.dir!=='1');   // folders can't be bundled
  if(!files.length){toast('Pick files to download (folders cannot be bundled)');return}
  const items=files.map(d=>{
    const e=fbEntries.find(x=>fbJoin(x.name)===d.path);
    return {url:'/api/download?path='+encodeURIComponent(d.path),name:basename(d.path),bytes:e?e.size:0,type:'application/octet-stream'};
  });
  dlBegin(items,'files.zip','files');
}
async function fbDeleteSelected(){
  const sel=fbSelected();
  if(!sel.length)return;
  const nDir=sel.filter(d=>d.dir==='1').length;
  const extra=nDir?' (including '+nDir+' folder'+(nDir>1?'s':'')+' and everything inside)':'';
  if(!confirm('Delete '+sel.length+' item'+(sel.length>1?'s':'')+extra+'?'))return;
  let ok=0;
  for(const d of sel){try{const r=await fetch('/api/delete?path='+encodeURIComponent(d.path),{method:'POST'});if(r.ok)ok++;}catch(e){}}
  toast('Deleted '+ok+' of '+sel.length);
  loadBrowse(fbPath);
}
async function fbDelete(el){
  const li=el.closest('li'),path=li.dataset.path,name=li.dataset.name,isDir=li.dataset.dir==='1';
  if(!confirm('Delete '+(isDir?'folder "'+name+'" and everything inside':'"'+name+'"')+'?'))return;
  try{
    const r=await fetch('/api/delete?path='+encodeURIComponent(path),{method:'POST'});
    if(!r.ok){alert('Delete failed: '+await r.text());return}
    toast('Deleted '+name);loadBrowse(fbPath);
  }catch(e){alert('Error: '+e)}
}
function fbRename(el){
  const li=el.closest('li'),path=li.dataset.path,name=li.dataset.name;
  $('modalTitle').textContent='Rename';
  $('modalInput').style.display='';$('modalInput').value=name;$('modalInput').placeholder='Name...';
  $('modalExtra').innerHTML='';$('modalOk').textContent='Rename';
  modalValidator=(v)=>(/[\\\/:*?"<>|]/.test((v||'').trim()))?'Name cannot contain \\ / : * ? " < > |':'';
  modalShowErr('');
  $('modalOverlay').classList.add('active');$('modalInput').focus();$('modalInput').select();
  modalCallback=async(v)=>{
    const nn=(v||'').trim();
    if(!nn||nn===name)return;
    const parent=path.substring(0,path.lastIndexOf('/'));
    const to=parent+'/'+nn;
    try{
      const r=await fetch('/api/move?from='+encodeURIComponent(path)+'&to='+encodeURIComponent(to),{method:'POST'});
      if(r.ok){toast('Renamed');loadBrowse(fbPath)}else alert('Rename failed: '+await r.text());
    }catch(e){alert('Error: '+e)}
  };
}
function fbNewFolder(){
  $('modalTitle').textContent='New Folder';
  $('modalInput').style.display='';$('modalInput').value='';$('modalInput').placeholder='Folder name...';
  $('modalExtra').innerHTML='';$('modalOk').textContent='Create';
  modalValidator=(v)=>(/[\\\/:*?"<>|]/.test((v||'').trim()))?'Name cannot contain \\ / : * ? " < > |':'';
  modalShowErr('');
  $('modalOverlay').classList.add('active');$('modalInput').focus();
  modalCallback=async(name)=>{
    if(!name)return;
    const base=fbPath==='/'?'':fbPath;
    try{
      const r=await fetch('/api/mkdir?path='+encodeURIComponent(base+'/'+name),{method:'POST'});
      if(r.ok){toast('Created folder: '+name);loadBrowse(fbPath)}else alert('Could not create folder: '+await r.text());
    }catch(e){alert('Error: '+e)}
  };
}
// Files-tab upload (into the current folder)
const fbDrop=$('fbDrop'),fbInput=$('fbInput');
fbDrop.addEventListener('click',()=>fbInput.click());
fbDrop.addEventListener('dragover',e=>{e.preventDefault();fbDrop.classList.add('drag-over')});
fbDrop.addEventListener('dragleave',()=>fbDrop.classList.remove('drag-over'));
fbDrop.addEventListener('drop',e=>{e.preventDefault();fbDrop.classList.remove('drag-over');fbUploadFiles(e.dataTransfer.files)});
fbInput.addEventListener('change',()=>{fbUploadFiles(fbInput.files);fbInput.value=''});
async function fbUploadFiles(fl){
  const arr=Array.from(fl);
  if(!arr.length)return;
  const dir=fbPath;   // "/" at root -> server writes /<name>; "/media" -> /media/<name>
  const bars={bar:'fbUploadBar',name:'fbUploadName',fill:'fbUploadFill',pct:'fbUploadPct'};
  for(let i=0;i<arr.length;i++)await uploadOne(arr[i],i+1,arr.length,dir,bars);
  $('fbUploadBar').classList.remove('active');
  loadBrowse(fbPath);
}

// ─── Upload ───
const dropZone=$('dropZone'),fileInput=$('fileInput');
dropZone.addEventListener('click',()=>fileInput.click());
dropZone.addEventListener('dragover',e=>{e.preventDefault();dropZone.classList.add('drag-over')});
dropZone.addEventListener('dragleave',()=>dropZone.classList.remove('drag-over'));
dropZone.addEventListener('drop',e=>{e.preventDefault();dropZone.classList.remove('drag-over');uploadFiles(e.dataTransfer.files)});
fileInput.addEventListener('change',()=>{uploadFiles(fileInput.files);fileInput.value=''});

async function uploadFiles(fl){
  const arr=Array.from(fl);
  const dir=$('uploadDir').value;
  for(let i=0;i<arr.length;i++)await uploadOne(arr[i],i+1,arr.length,dir);
  $('uploadBar').classList.remove('active');
  loadFiles();
}
function uploadOne(file,num,total,dir,bars){
  bars=bars||{bar:'uploadBar',name:'uploadName',fill:'uploadFill',pct:'uploadPct'};
  return new Promise((resolve,reject)=>{
    $(bars.bar).classList.add('active');
    $(bars.name).textContent='('+num+'/'+total+') '+file.name;
    $(bars.fill).style.width='0%';
    $(bars.pct).textContent='0%';
    const xhr=new XMLHttpRequest();
    const fd=new FormData();
    fd.append('file',file,file.name);
    xhr.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);$(bars.fill).style.width=p+'%';$(bars.pct).textContent=p+'%'}};
    xhr.onload=()=>xhr.status===200?resolve():reject(xhr.responseText);
    xhr.onerror=()=>reject('Network error');
    xhr.open('POST','/api/upload?dir='+encodeURIComponent(dir));
    xhr.send(fd);
  });
}

// ─── Delete ───
async function delPath(path,isDir){
  const name=basename(path);
  if(!confirm('Delete '+(isDir?'folder':'file')+' "'+name+'"?'))return;
  try{
    const fp=path.startsWith('/media')?path:'/media'+path;
    const r=await fetch('/api/delete?path='+encodeURIComponent(fp),{method:'POST'});
    if(!r.ok)alert('Delete failed: '+await r.text());
    else toast('Deleted '+name);
    loadFiles();
  }catch(e){alert('Error: '+e)}
}

// ─── Move ───
function moveFile(path){
  $('modalTitle').textContent='Move File';
  $('modalInput').style.display='none';
  // No inline styling here: .modal select already carries the token styling.
  $('modalExtra').innerHTML='<p style="margin-bottom:8px;font-family:var(--f-m);font-size:0.8em;color:var(--text-secondary)">Moving: '+esc(basename(path))+'</p>'+
    '<select id="moveDest">'+
    allFolders.map(f=>'<option value="'+esc(f)+'">'+esc(f)+'</option>').join('')+'</select>';
  $('modalOk').textContent='Move';
  $('modalOverlay').classList.add('active');
  modalCallback=async()=>{
    const dest=document.getElementById('moveDest').value;
    const fullSrc='/media'+path;
    const fullDest=dest+'/'+basename(path);
    if(fullSrc===fullDest){toast('Already in that folder');return}
    try{
      const r=await fetch('/api/move?from='+encodeURIComponent(fullSrc)+'&to='+encodeURIComponent(fullDest),{method:'POST'});
      if(r.ok){toast('Moved!');loadFiles()}
      else alert('Move failed: '+await r.text());
    }catch(e){alert('Error: '+e)}
  };
}

// ─── Create Folder ───
let modalCallback=null;
// Optional live validator: (value)=>errorString|''. When set (e.g. for rename),
// the modal shows the error inline as you type and blocks confirm — no waiting
// for Enter and losing what you typed.
let modalValidator=null;
function modalShowErr(msg){
  $('modalErr').textContent=msg||'';
  $('modalErr').classList.toggle('show',!!msg);
  $('modalOk').disabled=!!msg;
}
function createFolder(){
  $('modalTitle').textContent='New Folder';
  $('modalInput').value='';$('modalInput').placeholder='Folder name...';$('modalInput').style.display='';
  $('modalExtra').innerHTML='';
  $('modalOk').textContent='Create';
  $('modalOverlay').classList.add('active');
  $('modalInput').focus();
  modalCallback=async(name)=>{
    if(!name)return;
    const dir=$('uploadDir').value;
    await fetch('/api/mkdir?path='+encodeURIComponent(dir+'/'+name),{method:'POST'});
    toast('Created folder: '+name);
    loadFiles();
  };
}
function closeModal(){
  $('modalOverlay').classList.remove('active');
  $('modalInput').style.display='';
  $('modalInput').removeAttribute('maxlength');
  modalValidator=null;
  modalShowErr('');
}
function modalConfirm(){
  const visible=$('modalInput').style.display!=='none';
  if(visible&&modalValidator){
    const err=modalValidator($('modalInput').value);
    if(err){modalShowErr(err);return;}   // blocked: keep the modal + their text
  }
  const v=visible?$('modalInput').value.trim():null;
  closeModal();
  if(modalCallback)modalCallback(v);
}
$('modalInput').addEventListener('input',()=>{if(modalValidator)modalShowErr(modalValidator($('modalInput').value))});
$('modalInput').addEventListener('keydown',e=>{if(e.key==='Enter')modalConfirm()});

// ─── Audio Player ───
let _fromQueue=false;
function playTrack(url,fallbackTitle,fallbackArtist,source){
  audio.src=url;audio.play();
  $('playerBar').classList.add('show');
  nowPlayingUrl=url;
  if(source)playingSource=source;
  let title=fallbackTitle||stripExt(basename(url));
  let artist=fallbackArtist||'';
  // Look up real ID3 metadata from flatTracks
  const found=flatTracks.find(t=>t.path===url);
  if(found){title=found.title||title;artist=found.artist||artist}
  $('pTitle').textContent=title;
  $('pArtist').textContent=artist!=='-'?artist:'';
  $('btnPlay').innerHTML='&#10074;&#10074;';
  // Build queue from all tracks (unless navigating within existing queue)
  if(!_fromQueue&&flatTracks.length){
    queue=flatTracks.map(t=>({url:t.path,title:t.title,artist:t.artist}));
    queueIdx=queue.findIndex(q=>q.url===url);
    if(queueIdx<0)queueIdx=0;
  }
  _fromQueue=false;
  highlightPlaying();
}
function playerToggle(){
  if(audio.paused){audio.play();$('btnPlay').innerHTML='&#10074;&#10074;'}
  else{audio.pause();$('btnPlay').innerHTML='&#9654;'}
}
function playerPrev(){
  if(queue.length&&queueIdx>0){queueIdx--;_fromQueue=true;const t=queue[queueIdx];playTrack(t.url,t.title,t.artist)}
  else audio.currentTime=0;
}
function playerNext(){
  if(queue.length&&queueIdx<queue.length-1){queueIdx++;_fromQueue=true;const t=queue[queueIdx];playTrack(t.url,t.title,t.artist)}
}

// ─── Now-playing highlight ───
function highlightPlaying(){
  document.querySelectorAll('.playing').forEach(e=>e.classList.remove('playing'));
  if(!nowPlayingUrl)return;
  if(playingSource==='tracks'){
    $('trackBody').querySelectorAll('tr[data-path]').forEach(r=>{
      if(r.dataset.path===nowPlayingUrl)r.classList.add('playing');
    });
  }else if(playingSource==='playlist'){
    document.querySelectorAll('.pl-track[data-path]').forEach(el=>{
      if(el.dataset.path===nowPlayingUrl)el.classList.add('playing');
    });
  }
}

// Seekbar (visual progress only — interactive scrubbing backlogged)
const seekFill=$('seekFill'),seekThumb=$('seekThumb');
audio.addEventListener('timeupdate',()=>{
  if(!audio.duration)return;
  $('pCur').textContent=fmtTime(audio.currentTime);
  $('pDur').textContent='-'+fmtTime(audio.duration-audio.currentTime);
  const p=audio.currentTime/audio.duration*100;
  seekFill.style.width=p+'%';seekThumb.style.left=p+'%';
});
audio.addEventListener('ended',()=>{$('btnPlay').innerHTML='&#9654;';playerNext()});

// ─── Playlists ───
async function loadPlaylists(){
  try{const r=await fetch('/api/playlists');playlists=await r.json();renderPlaylists()}
  catch(e){$('playlistList').innerHTML='<div class="empty">Error loading playlists</div>'}
}
function renderPlaylists(){
  const el=$('playlistList');
  if(!playlists.length){el.innerHTML='<div class="empty">No playlists yet — create one above!</div>';return}
  el.innerHTML=playlists.map(p=>{
    const id='pl_'+btoa(unescape(encodeURIComponent(p.name))).replace(/[^a-zA-Z0-9]/g,'');
    return '<div class="pl-card" id="'+id+'">'+
      '<div class="pl-header" onclick="togglePl(\''+esc(p.name)+'\')">'+
      '<span class="name">'+esc(p.name)+'</span>'+
      '<span class="count">'+(p.tracks||0)+' tracks</span>'+
      '<button class="btn btn-play btn-sm" onclick="event.stopPropagation();playPlaylist(\''+esc(p.name)+'\')">&#9654; Play</button>'+
      '</div></div>';
  }).join('');
}
async function togglePl(name){
  const id='pl_'+btoa(unescape(encodeURIComponent(name))).replace(/[^a-zA-Z0-9]/g,'');
  const card=$(id);
  let tracks=card.querySelector('.pl-tracks');
  if(tracks){tracks.remove();card.querySelector('.pl-actions')?.remove();return}
  const r=await fetch('/api/playlist?name='+encodeURIComponent(name));
  const data=await r.json();
  let h='<div class="pl-tracks">';
  if(data.tracks.length){
    data.tracks.forEach((t,i)=>{
      const found=flatTracks.find(ft=>ft.path===t);
      const title=found?found.title:stripExt(basename(t));
      const artist=found?(found.artist!=='-'?found.artist:''):'';
      const missing=!found;
      h+='<div class="pl-track'+(missing?' missing':'')+'" data-path="'+esc(t)+'" style="'+(missing?'opacity:0.5;':'')+'">'+
        '<span class="num">'+(i+1)+'</span>'+
        '<button class="btn btn-play btn-sm" style="flex-shrink:0;padding:2px 6px" onclick="event.stopPropagation();playTrack(\''+esc(t)+'\',\''+esc(title)+'\',\''+esc(artist)+'\',\'playlist\')">&#9654;</button>'+
        '<span class="name" title="'+esc(t)+'">'+esc(title)+(artist?' <span style="color:var(--text-secondary);font-size:0.88em">- '+esc(artist)+'</span>':'')+(missing?' <span style="color:var(--danger);font-size:0.8em">(missing)</span>':'')+'</span>'+
        '<span class="rm" onclick="rmFromPl(\''+esc(name)+'\','+i+')">&#10005;</span>'+
        '</div>';
    });
  }else h+='<div class="empty" style="padding:8px">Empty playlist</div>';
  h+='</div><div class="pl-actions">'+
    '<button class="btn btn-del btn-sm" onclick="deletePl(\''+esc(name)+'\')">Delete Playlist</button>'+
    '</div>';
  card.insertAdjacentHTML('beforeend',h);
  highlightPlaying();
}
function createPlaylist(){
  $('modalTitle').textContent='New Playlist';
  $('modalInput').value='';$('modalInput').placeholder='Playlist name...';$('modalInput').style.display='';
  $('modalExtra').innerHTML='';
  $('modalOk').textContent='Create';
  $('modalOverlay').classList.add('active');
  $('modalInput').focus();
  modalCallback=async(name)=>{
    if(!name)return;
    await fetch('/api/playlist?name='+encodeURIComponent(name),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tracks:[]})});
    toast('Created playlist: '+name);
    loadPlaylists();
  };
}
async function deletePl(name){
  if(!confirm('Delete playlist "'+name+'"?'))return;
  await fetch('/api/playlist/delete?name='+encodeURIComponent(name),{method:'POST'});
  toast('Deleted playlist');loadPlaylists();
}
async function rmFromPl(name,idx){
  const r=await fetch('/api/playlist?name='+encodeURIComponent(name));
  const data=await r.json();
  data.tracks.splice(idx,1);
  await fetch('/api/playlist?name='+encodeURIComponent(name),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});
  const id='pl_'+btoa(unescape(encodeURIComponent(name))).replace(/[^a-zA-Z0-9]/g,'');
  const card=$(id);
  card.querySelector('.pl-tracks')?.remove();
  card.querySelector('.pl-actions')?.remove();
  togglePl(name);loadPlaylists();
}
async function playPlaylist(name){
  const r=await fetch('/api/playlist?name='+encodeURIComponent(name));
  const data=await r.json();
  if(!data.tracks.length)return;
  queue=data.tracks.map(t=>{
    const found=flatTracks.find(ft=>ft.path===t);
    return{url:t,title:found?found.title:stripExt(basename(t)),artist:found?found.artist:''};
  });
  queueIdx=0;_fromQueue=true;playTrack(queue[0].url,queue[0].title,queue[0].artist,'playlist');
}

// ─── Add to Playlist Dropdown ───
let addTrackPath='';
function showAddToPl(ev,trackPath){
  ev.stopPropagation();
  addTrackPath=trackPath;
  const dd=$('plDropdown');
  let h='<div class="dd-title">Add to playlist</div>';
  if(playlists.length){
    playlists.forEach(p=>{h+='<div class="dd-item" onclick="doAddToPl(\''+esc(p.name)+'\')">'+esc(p.name)+'</div>'});
  }
  h+='<div class="dd-item new" onclick="addToNewPl()">+ New playlist...</div>';
  dd.innerHTML=h;
  // Position near click
  const x=Math.min(ev.clientX,window.innerWidth-200);
  const y=Math.min(ev.clientY,window.innerHeight-200);
  dd.style.left=x+'px';dd.style.top=y+'px';
  dd.classList.add('show');
  setTimeout(()=>document.addEventListener('click',closePLDD,{once:true}),0);
}
function closePLDD(){$('plDropdown').classList.remove('show')}
async function doAddToPl(name){
  closePLDD();
  const r=await fetch('/api/playlist?name='+encodeURIComponent(name));
  const data=await r.json();
  data.tracks.push(addTrackPath);
  await fetch('/api/playlist?name='+encodeURIComponent(name),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});
  toast('Added to '+name);loadPlaylists();
}
function addToNewPl(){
  closePLDD();
  $('modalTitle').textContent='New Playlist';
  $('modalInput').value='';$('modalInput').placeholder='Playlist name...';$('modalInput').style.display='';
  $('modalExtra').innerHTML='';
  $('modalOk').textContent='Create & Add';
  $('modalOverlay').classList.add('active');
  $('modalInput').focus();
  modalCallback=async(name)=>{
    if(!name)return;
    await fetch('/api/playlist?name='+encodeURIComponent(name),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tracks:[addTrackPath]})});
    toast('Created playlist & added track');loadPlaylists();
  };
}

// ─── WiFi Settings ───
let selectedSSID='';
async function loadWifiStatus(){
  try{
    const r=await fetch('/api/wifi/status');
    const s=await r.json();
    const tag=$('connSTATag'),dot=$('connSTADot'),sta=$('connSTA');
    const sc=$('wifiStatusContent');
    $('connAP').textContent=s.ap_ip||'192.168.4.1';
    $('apIPDisplay').textContent=s.ap_ip||'192.168.4.1';
    if(s.connected){
      const mdns=s.mdns||'';
      tag.style.display='';dot.className='dot on';
      sta.textContent=mdns?mdns+' ('+s.ip+')':s.ip;
      sc.innerHTML='<div class="info-row"><span class="label">Status</span><span class="val" style="color:var(--success)">Connected</span></div>'+
        '<div class="info-row"><span class="label">Network</span><span class="val">'+esc(s.ssid)+'</span></div>'+
        '<div class="info-row"><span class="label">IP Address</span><span class="val accent">'+s.ip+'</span></div>'+
        (mdns?'<div class="info-row"><span class="label">mDNS</span><span class="val accent">'+mdns+'</span></div>':'')+
        '<div style="margin-top:12px"><button class="btn btn-del" onclick="forgetWifi()">Forget Network</button></div>';
    }else if(s.ssid&&s.status==='connecting'){
      tag.style.display='';dot.className='dot off';
      sta.textContent='Connecting to '+s.ssid+'...';
      sc.innerHTML='<div class="info-row"><span class="label">Status</span><span class="val"><span class="wifi-spinner"></span>Connecting to '+esc(s.ssid)+'...</span></div>';
      setTimeout(loadWifiStatus,2000);
    }else{
      tag.style.display='none';
      sc.innerHTML='<div class="info-row"><span class="label">Status</span><span class="val">Not connected</span></div>';
    }
  }catch(e){}
}

function signalBars(rssi){
  const n=rssi>-50?4:rssi>-60?3:rssi>-70?2:1;
  let b='';
  for(let i=1;i<=4;i++)b+='<span style="display:inline-block;width:3px;height:'+(4+i*3)+'px;background:'+(i<=n?'var(--accent)':'var(--border)')+';margin-right:1px;border-radius:1px;vertical-align:bottom"></span>';
  return b;
}

async function scanWifi(){
  const btn=$('scanBtn');
  btn.disabled=true;btn.innerHTML='<span class="wifi-spinner"></span>Scanning...';
  $('networkList').innerHTML='<li class="empty" style="padding:12px">Scanning nearby networks...</li>';
  try{
    // Start async scan, then poll until results are ready
    const r=await fetch('/api/wifi/scan');
    let data=await r.json();
    if(data.status==='scanning'){
      // Poll until scan completes
      let attempts=0;
      while(attempts<15){
        await new Promise(ok=>setTimeout(ok,500));
        const p=await fetch('/api/wifi/scan');
        data=await p.json();
        if(!data.status)break; // got results (array)
        attempts++;
      }
      if(data.status){$('networkList').innerHTML='<li class="empty" style="padding:12px">Scan timed out</li>';return}
    }
    const nets=Array.isArray(data)?data:[];
    if(!nets.length){$('networkList').innerHTML='<li class="empty" style="padding:12px">No networks found</li>';return}
    $('networkList').innerHTML=nets.map(n=>
      '<li onclick="selectNetwork(\''+esc(n.ssid)+'\')">'+
      '<span class="lock">'+(n.secure?'&#128274;':'&#128275;')+'</span>'+
      '<span class="ssid">'+esc(n.ssid)+'</span>'+
      '<span class="signal">'+signalBars(n.rssi)+' '+n.rssi+'</span>'+
      '</li>'
    ).join('');
  }catch(e){$('networkList').innerHTML='<li class="empty" style="padding:12px">Scan failed</li>'}
  finally{btn.disabled=false;btn.textContent='Scan for Networks'}
}

function selectNetwork(ssid){
  selectedSSID=ssid;
  $('selectedSSID').textContent=ssid;
  $('wifiPass').value='';
  $('wifiConnectForm').style.display='';
  $('wifiPass').focus();
  document.querySelectorAll('.network-list li').forEach(li=>{
    li.classList.toggle('selected',li.querySelector('.ssid')?.textContent===ssid);
  });
}

function cancelWifiConnect(){
  $('wifiConnectForm').style.display='none';
  selectedSSID='';
  document.querySelectorAll('.network-list li').forEach(li=>li.classList.remove('selected'));
}

async function doWifiConnect(){
  if(!selectedSSID)return;
  const btn=$('connectBtn');
  btn.disabled=true;btn.innerHTML='<span class="wifi-spinner"></span>Connecting...';
  try{
    await fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid:selectedSSID,pass:$('wifiPass').value})});
    $('wifiConnectForm').style.display='none';
    toast('Connecting to '+selectedSSID+'...');
    // Poll for connection status
    let attempts=0;
    const poll=async()=>{
      const r=await fetch('/api/wifi/status');
      const s=await r.json();
      if(s.connected){toast('Connected to '+s.ssid+'!');loadWifiStatus();return}
      if(++attempts<10)setTimeout(poll,2000);
      else{toast('Connection timed out');loadWifiStatus()}
    };
    setTimeout(poll,2000);
  }catch(e){toast('Connection failed')}
  finally{btn.disabled=false;btn.textContent='Connect'}
}

async function forgetWifi(){
  if(!confirm('Forget saved WiFi network?'))return;
  await fetch('/api/wifi/forget',{method:'POST'});
  toast('WiFi network forgotten');
  loadWifiStatus();
}

$('wifiPass').addEventListener('keydown',e=>{if(e.key==='Enter')doWifiConnect()});

// ─── Captive Portal Detection ───
function detectCaptive(){
  // Android captive portal WebView and iOS CaptiveNetwork have limited file input support
  const ua=navigator.userAgent||'';
  if(/CaptivePortal|MiniProgram|wv\)/i.test(ua)){
    $('captiveBanner').classList.add('show');
  }
  // Also show banner if file input click doesn't open picker (test on first interaction)
  const fi=$('fileInput');
  let origClick=false;
  fi.addEventListener('click',()=>{origClick=true},{once:true});
  // If after click the dialog doesn't appear, user is probably in restricted browser
  // We'll show the banner subtly anyway since the URL is useful info
}

// ─── Init ───
detectCaptive();
$('searchRow').classList.add('show'); // table is default view
setDeviceClock();   // stamp the device clock before the user records anything
loadFiles();
loadPlaylists();
loadWifiStatus();
</script>
</body>
</html>
)rawliteral";
