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
body{font-family:var(--f-d);background:var(--bg-primary);color:var(--text-primary)}
a{color:var(--accent);text-decoration:none}

/* CF-KIT-CSS:BEGIN - GENERATED from portal-companion/src/shared/. Do not edit
   between these markers: the same block is generated into the companion, and
   `npm run verify` fails if the two copies differ. To change the navigation,
   the player, the gate panel or the download overlay, edit
   portal-companion/src/shared/ and run:  cd portal-companion && npm run chrome:sync */
/* SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
   Copyright (c) 2023-2026 Dismo Industries LLC

   THE SHARED KIT - stylesheet half.

   The device serves two documents (the portal at / and the companion at /web/)
   and the user is meant to experience one app spanning both. Everything that
   must look and behave identically on each side lives here, in ONE source, and
   is generated into both by sync_chrome.mjs. verify_chrome.mjs fails the build
   if either copy drifts. See DEVICE-SURFACES-HANDOFF.md decision 4.

   Colours are literal phase-B values under --k-* rather than either document's
   token names, because the two documents name their tokens differently and this
   block has to be byte-identical in both. verify_tokens.mjs checks all three
   spellings agree.

   Shape language, carried from the artboards: chamfers not radii, panels take a
   14px top-right notch, filled CTAs take a 5px top-left/bottom-right notch,
   inputs and bordered row actions stay square. Every glyph is CSS geometry -
   nothing here depends on a font, so system-face substitution cannot break it. */

:root{
--k-deep:#08050f;--k-page:#0e0820;--k-card:#15102a;--k-elev:#1f1840;
--k-ink:#f5ecff;--k-dim:#a89cc8;--k-mute:#6b5e88;--k-faint:#3a3055;
--k-cy:#68e1fd;--k-mg:#ff2bb8;--k-gn:#4dffaf;--k-yl:#ffd84a;--k-rd:#ff5252;
--k-line:rgba(245,236,255,0.08);--k-sunk:rgba(0,0,0,0.35);
--k-cy-line:rgba(104,225,253,0.4);--k-mg-line:rgba(255,43,184,0.25);
--k-oled:#0a0a0a;--k-oled-fg:#b8f1ff;
--k-fd:'Chakra Petch',system-ui,sans-serif;--k-fm:ui-monospace,Consolas,monospace;
--k-notch:polygon(0 0,calc(100% - 14px) 0,100% 14px,100% 100%,0 100%);
--k-notch-b:polygon(5px 0,100% 0,100% calc(100% - 5px),calc(100% - 5px) 100%,0 100%,0 5px);
--k-bevel:inset 0 1px 0 rgba(255,255,255,0.22),inset 0 -3px 0 rgba(0,0,0,0.3);
}

/* ── Shell ──────────────────────────────────────────────────────────────
   Desktop: sidebar + main column, each its own scroll context. Phone: fixed
   masthead and tab bar with the main column inset between them. Neither
   document scrolls its body, so the tab bar cannot drift off-screen. */
body{display:flex;min-height:100vh;overflow:hidden}
.k-main{flex:1;display:flex;flex-direction:column;height:100vh;overflow:hidden;min-width:0}
.k-mast,.k-tabs{display:none}

/* ── Sidebar (desktop) ── */
.k-sb{width:220px;flex:none;background:var(--k-deep);border-right:1px solid var(--k-line);display:flex;flex-direction:column;height:100vh}
.k-sb-brand{padding:20px 14px 16px;border-bottom:1px solid rgba(255,43,184,0.22);text-align:center}
.k-logo{font-family:var(--k-fd);font-size:17px;font-weight:700;letter-spacing:0.13em;color:var(--k-ink);text-shadow:2px 2px 0 rgba(255,43,184,0.5);white-space:nowrap}
.k-logo-sub{font-family:var(--k-fm);font-size:9px;letter-spacing:0.3em;color:var(--k-cy);margin-top:5px;text-transform:uppercase}
.k-sb-nav{padding:10px 0;flex:1;overflow-y:auto}
.k-nv{display:flex;align-items:center;gap:11px;height:42px;padding:0 15px;font-family:var(--k-fd);font-size:11.5px;font-weight:600;letter-spacing:0.1em;text-transform:uppercase;color:var(--k-dim);border-left:3px solid transparent;cursor:pointer;text-decoration:none}
.k-nv:hover{color:var(--k-ink);background:rgba(104,225,253,0.05)}
.k-nv.on{color:var(--k-mg);border-left-color:var(--k-mg);background:rgba(255,43,184,0.08)}
.k-sb-foot{padding:13px 15px;border-top:1px solid var(--k-line);display:flex;flex-direction:column;gap:7px;font-family:var(--k-fm);font-size:9.5px;color:var(--k-dim)}
.k-sb-foot .k-kv{display:flex;align-items:center;gap:7px}
.k-sb-foot .k-ver{color:var(--k-mute)}

/* ── Phone masthead + tab bar ── */
.k-mast{align-items:center;gap:8px;padding:11px 14px;border-bottom:1px solid rgba(255,43,184,0.2);background:var(--k-deep);position:fixed;top:0;left:0;right:0;height:46px;z-index:120}
.k-mast .k-logo{font-size:12px}
.k-mast .k-sp{flex:1}
.k-tabs{grid-template-columns:repeat(5,1fr);background:rgba(8,5,15,0.97);border-top:1px solid var(--k-mg-line);padding:7px 0 max(10px,env(safe-area-inset-bottom));position:fixed;bottom:0;left:0;right:0;z-index:120}
.k-tab{display:flex;flex-direction:column;align-items:center;gap:4px;padding-top:5px;min-height:44px;color:var(--k-mute);cursor:pointer;text-decoration:none}
.k-tab.on{color:var(--k-mg)}
.k-tab .k-lb{font-family:var(--k-fd);font-size:9px;font-weight:600;letter-spacing:0.08em;text-transform:uppercase}

/* ── Page masthead: display-face title on the left, mono status on the right,
      sharing one baseline - the artboards' .ph1 (phone) and .mh (desktop).
      The title is the growing cell and takes the ellipsis; the status is short
      and fixed, so neither depends on how wide the substituted face renders. ── */
.page-header{padding:13px 20px 12px;border-bottom:1px solid var(--k-mg-line);background:var(--k-deep);display:flex;align-items:baseline;justify-content:space-between;gap:12px;flex-shrink:0}
.page-header h1{font-family:var(--k-fd);font-size:1.45em;font-weight:700;letter-spacing:0.02em;text-transform:uppercase;line-height:1.1;color:var(--k-ink);min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.page-header .status{font-family:var(--k-fm);font-size:0.7em;color:var(--k-dim);letter-spacing:0.04em;flex:none;white-space:nowrap}

/* ── Pixel icons. Rules below are generated from shared/icons.mjs. ── */
.k-px{position:relative;display:inline-block;width:18px;height:18px;flex:none}
.k-px::before{content:'';position:absolute;left:0;top:0;width:2px;height:2px}

/* ── Segmented control (Explore tab-group treatment, cyan-filled active) ── */
.k-seg{display:inline-flex;border:1px solid var(--k-cy-line);align-self:flex-start;max-width:100%;overflow:hidden}
.k-sg{padding:8px 13px;font-family:var(--k-fd);font-size:10.5px;font-weight:700;letter-spacing:0.1em;text-transform:uppercase;color:var(--k-dim);cursor:pointer;border-right:1px solid rgba(104,225,253,0.25);text-decoration:none;white-space:nowrap}
.k-sg:last-child{border-right:none}
.k-sg.on{background:var(--k-cy);color:var(--k-deep)}

/* ── Gate panel. Yellow is notice, never error: nothing is broken, something
      simply is not set up. One per affected view; the affordances that need
      what is missing disappear rather than sitting greyed out. ── */
.k-gate{border:1px solid var(--k-yl);background:rgba(255,216,74,0.05);padding:12px;display:flex;flex-direction:column;gap:8px;margin-bottom:14px}
.k-gate .k-g1{font-size:11px;color:var(--k-yl);line-height:1.5}
.k-gate .k-g2{font-family:var(--k-fm);font-size:10px;color:var(--k-dim);line-height:1.6}
.k-gate .k-go{align-self:flex-start;background:var(--k-elev);border:1px solid var(--k-line);color:var(--k-ink);font-family:var(--k-fd);font-size:10.5px;font-weight:700;letter-spacing:0.1em;text-transform:uppercase;padding:9px 14px;cursor:pointer;text-decoration:none}
.k-gate .k-go:hover{background:var(--k-card);color:var(--k-ink)}

/* ── Chips + status dots ── */
.k-chip{display:inline-flex;align-items:center;gap:6px;padding:4px 9px;border:1px solid var(--k-line);font-family:var(--k-fm);font-size:9.5px;letter-spacing:0.08em;color:var(--k-dim);text-transform:uppercase;white-space:nowrap}
.k-chip.cy{border-color:rgba(104,225,253,0.5);color:var(--k-cy)}
.k-chip.gn{border-color:rgba(77,255,175,0.5);color:var(--k-gn)}
.k-chip.yl{border-color:var(--k-yl);color:var(--k-yl)}
.k-dot{width:7px;height:7px;border-radius:50%;flex:none;background:var(--k-gn);box-shadow:0 0 6px var(--k-gn)}
.k-dot.cy{background:var(--k-cy);box-shadow:0 0 6px var(--k-cy)}
.k-dot.off{background:var(--k-faint);box-shadow:none}

/* ── Signal bars. Four CSS blocks, tinted by strength class - this is what
      replaces the emoji that could not be tinted. ── */
.k-sig{display:inline-flex;align-items:flex-end;gap:1.5px;height:11px;vertical-align:-1px}
.k-sig i{width:3px;background:var(--k-faint)}
.k-sig i:nth-child(1){height:3px}.k-sig i:nth-child(2){height:6px}
.k-sig i:nth-child(3){height:9px}.k-sig i:nth-child(4){height:11px}
.k-sig.s1 i:nth-child(-n+1),.k-sig.s2 i:nth-child(-n+2),.k-sig.s3 i:nth-child(-n+3),.k-sig.s4 i{background:var(--k-cy)}

/* ── Glyphs. CSS borders and blocks in currentColor: no font, no emoji,
      nothing for a substitution to take away. ── */
/* inline-block on every one of these is load-bearing: width/height do not apply
   to an inline box, so an inline triangle renders as a bordered rectangle. */
.k-tri-r,.k-tri-l,.k-bar,.k-pause,.k-spk{display:inline-block;vertical-align:middle}
.k-tri-r{width:0;height:0;border-left:9px solid currentcolor;border-top:6px solid transparent;border-bottom:6px solid transparent}
.k-tri-l{width:0;height:0;border-right:9px solid currentcolor;border-top:6px solid transparent;border-bottom:6px solid transparent}
.k-tri-r.sm{border-left-width:7px;border-top-width:5px;border-bottom-width:5px}
.k-tri-l.sm{border-right-width:7px;border-top-width:5px;border-bottom-width:5px}
.k-bar{width:2.5px;height:12px;background:currentcolor}
.k-pause{width:11px;height:13px;border-left:4px solid currentcolor;border-right:4px solid currentcolor}
.k-pause.sm{width:8px;height:10px;border-left-width:3px;border-right-width:3px}
.k-skip{display:inline-flex;align-items:center;gap:1.5px}
.k-spk{width:0;height:0;border-right:8px solid currentcolor;border-top:6px solid transparent;border-bottom:6px solid transparent;position:relative}
.k-spk::after{content:'';position:absolute;left:0;top:-3px;width:4px;height:6px;background:currentcolor}
.k-eq{display:inline-flex;align-items:flex-end;gap:2px;height:12px}
.k-eq i{width:3px;background:var(--k-cy);animation:k-eqb 1s ease-in-out infinite}
.k-eq i:nth-child(1){height:6px}
.k-eq i:nth-child(2){height:11px;animation-delay:0.25s}
.k-eq i:nth-child(3){height:8px;animation-delay:0.5s}
@keyframes k-eqb{0%,100%{transform:scaleY(0.5)}50%{transform:scaleY(1)}}

/* ── The audio player. One component, three sizes, one geometry - it replaces
      every native <audio controls> on both surfaces. Only the main action
      carries a fill, the chamfer and the bevel; secondary transport stays
      bordered and square, which is the CTA/ghost split the site already uses.
      The single state change between playing and paused is the main glyph. ── */
.k-pbtn{width:38px;height:38px;border:1px solid rgba(104,225,253,0.45);color:var(--k-cy);display:flex;align-items:center;justify-content:center;background:none;cursor:pointer;flex:none;padding:0}
.k-pbtn:hover{background:rgba(104,225,253,0.1)}
.k-pbtn.main{width:46px;height:46px;background:var(--k-cy);color:var(--k-deep);border:none;clip-path:var(--k-notch-b);box-shadow:var(--k-bevel)}
.k-pbtn.main:hover{background:#a5edff}
.k-pbtn.sm{width:28px;height:28px}
.k-player{background:var(--k-elev);border-top:1px solid var(--k-mg-line);display:none;align-items:center;gap:16px;padding:13px 22px;position:fixed;bottom:0;left:220px;right:0;z-index:110;height:76px}
.k-player.on{display:flex}
.k-ptit{font-family:var(--k-fd);font-size:13px;font-weight:700;color:var(--k-ink);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.k-part{font-family:var(--k-fm);font-size:9.5px;color:var(--k-mute);margin-top:2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.k-time{font-family:var(--k-fm);font-size:10px;color:var(--k-dim);flex:none;font-variant-numeric:tabular-nums}
.k-seek{flex:1;min-width:60px;height:16px;display:flex;align-items:center;cursor:pointer}
.k-seek .k-tr{position:relative;height:6px;width:100%;background:var(--k-sunk);border:1px solid var(--k-line);box-shadow:inset 0 2px 4px rgba(0,0,0,0.5)}
.k-seek .k-fl{position:absolute;left:0;top:0;bottom:0;background:var(--k-cy);box-shadow:0 0 8px rgba(104,225,253,0.5)}
.k-seek .k-th{position:absolute;top:50%;transform:translate(-50%,-50%);width:9px;height:13px;background:var(--k-ink)}
.k-seek.thin{height:10px}.k-seek.thin .k-tr{height:4px}.k-seek.thin .k-th{width:7px;height:10px}
.k-prow{display:flex;align-items:center;gap:10px}

/* ── Download overlay: the chooser and the byte-progress readout shared by the
      portal's Files browser and the companion's Notes. ── */
.k-dl{display:none;position:fixed;inset:0;background:rgba(8,5,15,0.86);z-index:320;justify-content:center;align-items:center;padding:16px}
.k-dl.on{display:flex}
.k-dl-box{background:var(--k-card);border:1px solid var(--k-line);clip-path:var(--k-notch);padding:20px;width:340px;max-width:92vw}
.k-dl-box h3{font-family:var(--k-fd);font-size:0.95em;font-weight:700;letter-spacing:0.08em;text-transform:uppercase;color:var(--k-cy);margin:0 0 7px}
.k-dl-size{font-family:var(--k-fm);font-size:0.78em;color:var(--k-dim);margin-bottom:14px;line-height:1.5}
.k-dl-btns{display:flex;flex-direction:column;gap:8px;margin-bottom:10px}
.k-dl-b{background:none;border:1px solid var(--k-line);color:var(--k-dim);font-family:var(--k-fd);font-size:0.76em;font-weight:700;letter-spacing:0.06em;text-transform:uppercase;padding:11px;cursor:pointer;width:100%}
.k-dl-b:hover{background:var(--k-elev);color:var(--k-ink)}
.k-dl-b.pri{background:var(--k-cy);border-color:var(--k-cy);color:var(--k-deep);clip-path:var(--k-notch-b);box-shadow:var(--k-bevel)}
.k-dl-b.pri:hover{background:#a5edff}
.k-dl-b:disabled{opacity:0.4;cursor:default}
.k-dl-bar{height:8px;background:var(--k-sunk);border:1px solid var(--k-line);overflow:hidden;margin:12px 0 8px}
.k-dl-fill{height:100%;width:0%;background:linear-gradient(90deg,var(--k-cy),var(--k-mg));transition:width 0.15s}
.k-dl-stat{font-family:var(--k-fm);font-size:0.82em;font-variant-numeric:tabular-nums}
.k-dl-rate{font-family:var(--k-fm);font-size:0.76em;color:var(--k-cy);margin-top:3px;font-variant-numeric:tabular-nums}
.k-dl-file{font-family:var(--k-fm);font-size:0.76em;color:var(--k-mute);margin-top:3px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.k-dl-warn{font-family:var(--k-fm);font-size:0.74em;line-height:1.5;color:var(--k-yl);border:1px solid rgba(255,216,74,0.4);background:rgba(255,216,74,0.06);padding:7px 9px;margin:12px 0 10px}

/* ── Phone. The drawer and its menu button are gone; the tab bar IS the
      navigation, on both documents, at the same breakpoint. ── */
@media(max-width:699px){
  .k-sb{display:none}
  .k-mast{display:flex}
  .k-tabs{display:grid}
  .k-main{padding-top:46px;padding-bottom:66px}
  .page-header{padding:11px 14px 10px}
  .page-header h1{font-size:1.25em}
  .page-header .status{font-size:0.62em}
  .k-player{left:0;height:52px;padding:7px 14px;gap:12px;bottom:66px}
  .k-player .k-ptit{font-size:11.5px}
  .k-mast .k-chip{padding:3px 7px;font-size:9px}
}
@media(max-width:380px){
  .k-mast .k-chip .k-lbl{display:none}
}

/* Pixel icons, expanded from shared/icons.mjs. */
.k-px-media::before{box-shadow:8px 0px,10px 0px,8px 2px,12px 2px,8px 4px,14px 4px,8px 6px,14px 6px,8px 8px,4px 10px,6px 10px,8px 10px,2px 12px,4px 12px,6px 12px,8px 12px,10px 12px,2px 14px,4px 14px,6px 14px,8px 14px,10px 14px,4px 16px,6px 16px,8px 16px}
.k-px-notes::before{box-shadow:6px 0px,8px 0px,10px 0px,6px 2px,8px 2px,10px 2px,6px 4px,8px 4px,10px 4px,6px 6px,8px 6px,10px 6px,6px 8px,8px 8px,10px 8px,2px 10px,14px 10px,4px 12px,6px 12px,8px 12px,10px 12px,12px 12px,8px 14px,4px 16px,6px 16px,8px 16px,10px 16px,12px 16px}
.k-px-listen::before{box-shadow:8px 0px,8px 2px,4px 4px,8px 4px,12px 4px,0px 6px,4px 6px,8px 6px,12px 6px,16px 6px,0px 8px,4px 8px,8px 8px,12px 8px,16px 8px,0px 10px,4px 10px,8px 10px,12px 10px,16px 10px,4px 12px,8px 12px,12px 12px,8px 14px,8px 16px}
.k-px-files::before{box-shadow:0px 2px,2px 2px,4px 2px,0px 4px,6px 4px,8px 4px,10px 4px,12px 4px,14px 4px,16px 4px,0px 6px,16px 6px,0px 8px,16px 8px,0px 10px,16px 10px,0px 12px,16px 12px,0px 14px,2px 14px,4px 14px,6px 14px,8px 14px,10px 14px,12px 14px,14px 14px,16px 14px}
.k-px-settings::before{box-shadow:4px 0px,6px 0px,0px 2px,2px 2px,4px 2px,6px 2px,8px 2px,10px 2px,12px 2px,14px 2px,16px 2px,4px 4px,6px 4px,12px 6px,14px 6px,0px 8px,2px 8px,4px 8px,6px 8px,8px 8px,10px 8px,12px 8px,14px 8px,16px 8px,12px 10px,14px 10px,6px 12px,8px 12px,0px 14px,2px 14px,4px 14px,6px 14px,8px 14px,10px 14px,12px 14px,14px 14px,16px 14px,6px 16px,8px 16px}
/* CF-KIT-CSS:END */

/* The page masthead (.page-header) is the shared kit's - see CF-KIT-CSS. */
.content{flex:1;overflow-y:auto;padding:16px 20px;padding-bottom:120px}
/* Settings is one destination with three segments; only Network renders in this
   document, so the other two are ordinary links into the companion. */
.seg-row{margin-bottom:16px}

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
/* Square glyph action, the artboard's row-action shape. Fixed box so a font
   substitution cannot change how wide a row's action cluster is. */
.btn-ico{width:26px;height:26px;padding:0;display:inline-flex;align-items:center;justify-content:center;font-size:0.95em;line-height:1}

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

/* The player bar is the shared kit's component (.k-player) - see CF-KIT-CSS
   above. It is the same geometry the companion uses, at three sizes. */

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
/* Strength is the kit's four CSS bars and "locked" is a text chip. Both used to
   be emoji, which render in fixed multi-colour and cannot be tinted - the one
   live instance of that defect class the design brief called out. */
.network-list .lock{font-family:var(--f-m);font-size:0.62em;font-weight:700;letter-spacing:0.1em;text-transform:uppercase;color:var(--text-mute);border:1px solid var(--border);padding:2px 6px;white-space:nowrap}
.wifi-input{width:100%;padding:9px 11px;background:var(--bg-sunk);border:1px solid var(--border);color:var(--text-primary);font-family:var(--f-m);font-size:0.86em;margin:8px 0}
.wifi-input:focus{border-color:var(--accent);outline:none}
.wifi-actions{display:flex;gap:8px;margin-top:12px}
.wifi-spinner{display:inline-block;width:16px;height:16px;border:2px solid var(--border);border-top-color:var(--accent);border-radius:50%;animation:spin 0.8s linear infinite;vertical-align:middle;margin-right:6px}
@keyframes spin{to{transform:rotate(360deg)}}

/* The connection facts (its own network, home WiFi, firmware version) moved into
   the shared kit's sidebar footer on desktop and its masthead chips on a phone,
   so both device documents state them in the same place.

   Item rows. Named for Voice notes, where they started; that page has retired
   into the companion's merged Notes view, but the Files browser renders the same
   shape, so the styles and their names stay. */
.vn-list{list-style:none}
.vn-item{background:var(--bg-secondary);border:1px solid var(--border);border-left:2px solid var(--accent);clip-path:var(--notch);padding:11px 12px;margin-bottom:8px}
.vn-top{display:flex;align-items:center;gap:8px}
.vn-chk{flex-shrink:0;width:16px;height:16px;accent-color:var(--accent);cursor:pointer}
.vn-name{flex:1;font-weight:700;font-size:0.9em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
.vn-acts{display:flex;gap:5px;flex-shrink:0}
.vn-acts .btn{text-decoration:none}
.vn-meta{font-family:var(--f-m);color:var(--text-mute);font-size:0.74em;margin:5px 0 2px;font-variant-numeric:tabular-nums}
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

/* The bulk download chooser is the shared kit's overlay (.k-dl) - the Files
   browser and the companion's Notes both drive the same code now. */

/* Phone. The menu button and the slide-out drawer are gone; the kit's bottom
   tab bar is the navigation on both device documents. */
@media(max-width:699px){
  .content{padding:12px 14px;padding-bottom:24px}
  .section-hdr{letter-spacing:0.1em}
  /* The track table becomes rows, which is the phone pattern every phase-B
     artboard uses: name in the display face over a mono meta line. Restyling
     the table beats a second render path - one source of truth for sorting,
     filtering and the now-playing highlight. */
  .track-table,.track-table tbody{display:block}
  .track-table thead{display:none}
  .track-table tr{display:flex;flex-wrap:wrap;align-items:center;padding:9px 0;border-bottom:1px solid var(--border)}
  .track-table td{border:none;padding:0}
  .track-table .col-title{flex:1 1 100%;min-width:0;max-width:none;order:1;font-size:1.02em}
  .track-table .col-artist{order:2;color:var(--text-mute)}
  .track-table .col-artist::after{content:' \00b7 '}
  .track-table .col-size{order:3;text-align:left;width:auto;flex:1;color:var(--text-mute)}
  .track-table .col-album{display:none}
  .track-table .col-acts{order:4;width:auto;flex:none}
  /* Row actions are always visible on a phone - there is no hover to reveal
     them, and the artboard's phone rows carry their action inline. */
  .track-table .col-acts .acts{opacity:1}
  .track-table tr.playing td{background:none}
  .track-table tr.playing td:first-child{box-shadow:none;color:var(--accent)}
  .track-table tr.playing{background:rgba(255,43,184,0.08)}
}
</style>
</head>
<body>

<!-- The navigation - sidebar on a laptop, bottom tab bar on a phone, plus the
     phone masthead - is built by the shared kit on load (CFK.nav below), from
     the same source the companion uses. There is no markup for it here on
     purpose: two hand-written copies is exactly how the two surfaces drifted
     apart in the first place. -->
<div class="k-main">
  <div class="page-header">
    <h1 id="pageTitle">Media</h1>
    <div class="status" id="statusBar"></div>
  </div>

  <div class="banner" id="captiveBanner">
    <span>For uploads, open in your browser:</span>
    <span class="url">192.168.4.1</span>
    <span class="close-banner" onclick="this.parentElement.classList.remove('show')">&times;</span>
  </div>

  <div class="content" id="mediaPage">
    <div class="drop-zone" id="dropZone">
      <p class="main-text">+ Add music</p>
      <p class="hint">Tap to browse, or drop files</p>
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

  <!-- Voice notes used to be a page here. It listed the same recordings the
       companion's Transcripts view listed, which was the sharpest edge of the
       seam between the two surfaces. It has retired into one Notes destination
       on /web/, where the transcription engine already lives; showPage('voice')
       and any saved #voice link redirect there. -->

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
    <!-- One Settings destination, three segments. Network renders here; the
         other two live in the companion document and are plain links, styled
         identically, because the segmented control is the same component on
         both sides and the crossing is meant to be invisible. -->
    <div class="seg-row">
      <div class="k-seg">
        <span class="k-sg on">Network</span>
        <a class="k-sg" href="/web/#settings/transcription">Transcription</a>
        <a class="k-sg" href="/web/#settings/data">Your data</a>
      </div>
    </div>

    <div class="wifi-card" id="wifiStatusCard">
      <h3>WiFi Connection</h3>
      <div id="wifiStatusContent">
        <div class="info-row"><span class="label">Status</span><span class="val" id="wifiStatusText">Not connected</span></div>
      </div>
    </div>

    <div class="wifi-card">
      <h3>Available Networks</h3>
      <ul class="network-list" id="networkList"></ul>
      <!-- A ghost button, not a filled CTA: scanning is a repeatable utility
           action, and the artboard reserves the cyan fill for the primary one. -->
      <div style="display:flex;gap:8px;margin-top:8px">
        <button class="btn btn-play" onclick="scanWifi()" id="scanBtn">Scan again</button>
      </div>
      <div id="wifiConnectForm" style="display:none">
        <div style="font-size:0.85em;margin-bottom:4px;color:var(--text-secondary)">Connecting to: <strong id="selectedSSID" style="color:var(--text-primary)"></strong></div>
        <input type="password" class="wifi-input" id="wifiPass" placeholder="Password (leave empty for open network)">
        <div class="wifi-actions">
          <button class="btn" onclick="cancelWifiConnect()">Cancel</button>
          <button class="btn btn-accent" onclick="doWifiConnect()" id="connectBtn">Connect</button>
        </div>
      </div>
    </div>

    <!-- "Its own network", not "access point"; "address", not "IP". The network
         name CyberFidget is the one proper noun these surfaces use. -->
    <div class="wifi-card">
      <h3>Its own network</h3>
      <div class="info-row"><span class="label">Name</span><span class="val">CyberFidget</span></div>
      <div class="info-row"><span class="label">Address</span><span class="val accent" id="apIPDisplay">192.168.4.1</span></div>
      <div class="info-row"><span class="label">Status</span><span class="val" style="color:var(--success)">Always available</span></div>
    </div>
  </div>
  <!-- The Live Playlist page was a nav entry and a one-line "coming soon"
       placeholder. It is not one of the five destinations the design register
       settles on, and it had no behaviour, so it has gone. -->
</div>

<!-- Filled in by the shared kit's player: bar size here on a laptop, mini above
     the tab bar on a phone. Replaces the native audio element on both surfaces. -->
<div id="playerBar"></div>

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

<!-- The bulk download chooser is built on demand by the shared kit. -->

<div class="toast" id="toast"></div>

<audio id="audio" preload="none"></audio>

<script>
/* CF-KIT-JS:BEGIN - GENERATED from portal-companion/src/shared/. Do not edit
   between these markers; see the CF-KIT-CSS note above. Regenerate with:
   cd portal-companion && npm run chrome:sync */
// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// THE SHARED KIT - behaviour half.
//
// One source, generated into both device documents by sync_chrome.mjs;
// verify_chrome.mjs fails if either copy drifts. See DEVICE-SURFACES-HANDOFF.md
// decision 4.
//
// Deliberately a CLASSIC script, not a module: the portal is a hand-authored
// PROGMEM literal with a plain <script>, and the companion's esbuild bundle can
// reference a global just fine. Everything hangs off CFK so the two documents'
// own top-level names cannot collide with it.
//
// The nav is built in JS rather than written as markup twice - that is what
// makes "byte-identical in both documents" mechanically true instead of a
// promise someone has to keep.

var CFK = (function () {

  // ── The five destinations, in the one order both documents use. ──────────
  //
  // `doc` is the document that RENDERS the destination. When it matches the
  // host document the item gets a local handler; otherwise it is a plain <a>
  // with no external-link marker and no transition copy, because crossing is
  // just a page load on the device's own network and the user is not supposed
  // to be able to tell.
  //
  // Settings is the one that spans both documents (Network here, Transcription
  // and Your data on the companion). It is listed as `portal` because the tab
  // always lands on Network - it renders where most entry points are and it
  // answers "am I connected", which is the top settings question. Segments then
  // link across.
  var DEST = [
    { id: 'media',    label: 'Media',    doc: 'portal',    href: '/#media' },
    { id: 'notes',    label: 'Notes',    doc: 'companion', href: '/web/#notes' },
    { id: 'listen',   label: 'Listen',   doc: 'companion', href: '/web/#listen' },
    { id: 'files',    label: 'Files',    doc: 'portal',    href: '/#files' },
    { id: 'settings', label: 'Settings', doc: 'portal',    href: '/#settings' },
  ];

  var host = 'portal';
  var active = 'media';
  var onLocal = null;

  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }

  function icon(name) { return el('span', 'k-px k-px-' + name); }

  function item(d, cls) {
    var a = el('a', cls);
    a.href = d.href;
    a.dataset.dest = d.id;
    if (d.doc === host) {
      a.addEventListener('click', function (ev) {
        ev.preventDefault();
        if (onLocal) onLocal(d.id);
      });
    }
    return a;
  }

  // ── Build the chrome. Both documents call this once, on load. ────────────
  // `hostDoc` is 'portal' or 'companion'; `localHandler` is called with a
  // destination id when the user picks one this document renders itself.
  function nav(hostDoc, activeId, localHandler) {
    host = hostDoc;
    onLocal = localHandler;

    var mast = el('div', 'k-mast');
    var brand = el('div', 'k-logo', 'CYBER FIDGET');
    mast.appendChild(brand);
    mast.appendChild(el('span', 'k-sp'));
    mast.appendChild(chip('mastDevice', 'k-chip gn', 'device'));
    mast.appendChild(chip('mastNet', 'k-chip cy', 'network'));

    var sb = el('aside', 'k-sb');
    var sbBrand = el('div', 'k-sb-brand');
    sbBrand.appendChild(el('div', 'k-logo', 'CYBER FIDGET'));
    sbBrand.appendChild(el('div', 'k-logo-sub', 'Your Fidget'));
    sb.appendChild(sbBrand);
    var sbNav = el('nav', 'k-sb-nav');
    var tabs = el('nav', 'k-tabs');

    DEST.forEach(function (d) {
      var nv = item(d, 'k-nv');
      nv.appendChild(icon(d.id));
      nv.appendChild(document.createTextNode(d.label));
      sbNav.appendChild(nv);

      var tab = item(d, 'k-tab');
      tab.appendChild(icon(d.id));
      tab.appendChild(el('span', 'k-lb', d.label));
      tabs.appendChild(tab);
    });

    sb.appendChild(sbNav);
    sb.appendChild(el('div', 'k-sb-foot'));

    document.body.insertBefore(sb, document.body.firstChild);
    document.body.insertBefore(mast, document.body.firstChild);
    document.body.appendChild(tabs);
    setActive(activeId);
  }

  function chip(id, cls, text) {
    var c = el('span', cls);
    c.id = id;
    c.appendChild(el('span', 'k-dot' + (cls.indexOf('cy') >= 0 ? ' cy' : '')));
    c.appendChild(el('span', 'k-lbl', text));
    return c;
  }

  function setActive(id) {
    active = id;
    var all = document.querySelectorAll('.k-nv,.k-tab');
    for (var i = 0; i < all.length; i++) {
      all[i].classList.toggle('on', all[i].dataset.dest === id);
    }
  }

  // ── Connection facts: the sidebar footer on desktop, the masthead chips on
  //    a phone. Replaces the portal's old separate connection bar and the
  //    companion's own header, so both surfaces state the same things. ──────
  //    s: { apIp, ssid, staIp, version }
  function conn(s) {
    var foot = document.querySelector('.k-sb-foot');
    if (foot) {
      foot.innerHTML = '';
      foot.appendChild(kv('k-dot', 'Its own network - ' + (s.apIp || '192.168.4.1')));
      foot.appendChild(kv('k-dot cy', s.ssid ? 'Home WiFi - ' + s.ssid : 'No home WiFi'));
      if (!s.ssid) foot.lastChild.firstChild.className = 'k-dot off';
      if (s.version) {
        var v = el('div', 'k-kv k-ver', 'version ' + s.version);
        foot.appendChild(v);
      }
    }
    var dev = document.getElementById('mastDevice');
    var net = document.getElementById('mastNet');
    if (dev) dev.lastChild.textContent = 'device';
    if (net) {
      net.lastChild.textContent = s.ssid || 'its own network';
      net.firstChild.className = s.ssid ? 'k-dot cy' : 'k-dot off';
    }
  }

  function kv(dotCls, text) {
    var row = el('div', 'k-kv');
    row.appendChild(el('span', dotCls));
    row.appendChild(el('span', null, text));
    return row;
  }

  // ── Signal strength as four CSS bars. The portal's WiFi list used a pair of
  //    lock emoji and a raw dBm number; emoji render in fixed multi-colour and
  //    cannot be tinted, which is the defect class the brief called out. ─────
  function sig(rssi) {
    var n = rssi > -50 ? 4 : rssi > -60 ? 3 : rssi > -70 ? 2 : 1;
    return '<span class="k-sig s' + n + '"><i></i><i></i><i></i><i></i></span>';
  }

  // ── Gate panel (register decision 8). One treatment, reused wherever
  //    something needs a part that is not installed. Yellow means notice:
  //    the device is working exactly as shipped. ─────────────────────────────
  //    o: { lead, body, action, href }
  function gate(o) {
    var g = el('div', 'k-gate');
    g.appendChild(el('div', 'k-g1', o.lead));
    g.appendChild(el('div', 'k-g2', o.body));
    var b = el('a', 'k-go', o.action || 'Set it up >');
    b.href = o.href;
    g.appendChild(b);
    return g;
  }

  // ── Formatting shared by both surfaces' lists and players ────────────────
  function fmtBytes(b) {
    if (b < 1024) return b + ' B';
    if (b < 1048576) return (b / 1024).toFixed(1) + ' KB';
    return (b / 1048576).toFixed(1) + ' MB';
  }
  function fmtTime(s) {
    if (isNaN(s) || s < 0) return '0:00';
    s = Math.floor(s);
    return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
  }
  function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
      .replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');
  }

  // ── The audio player. Replaces every native <audio controls> on both
  //    surfaces. `size` is 'bar' (76px, desktop media), 'mini' (52px, phone,
  //    sits above the tab bar) or 'inline' (28px controls in an expanded row).
  //    Returns a handle whose sync() the caller drives from timeupdate.
  //
  //    Only the main action carries the fill, the chamfer and the bevel; the
  //    one state change between playing and paused is that glyph.
  function player(mount, audio, size, opts) {
    opts = opts || {};
    var full = size !== 'inline';
    mount.className = full ? 'k-player' : 'k-prow';
    mount.innerHTML = '';

    var main = el('button', full ? 'k-pbtn main' : 'k-pbtn sm');
    main.type = 'button';
    var glyph = el('span', 'k-tri-r' + (full ? '' : ' sm'));
    main.appendChild(glyph);
    main.addEventListener('click', function () {
      if (audio.paused) audio.play(); else audio.pause();
    });

    var prev = null, next = null;
    if (size === 'bar' && opts.onPrev) {
      prev = el('button', 'k-pbtn');
      prev.type = 'button';
      var pg = el('span', 'k-skip');
      pg.appendChild(el('span', 'k-bar'));
      pg.appendChild(el('i', 'k-tri-l'));
      prev.appendChild(pg);
      prev.addEventListener('click', opts.onPrev);
      mount.appendChild(prev);
    }
    mount.appendChild(main);
    if (size === 'bar' && opts.onNext) {
      next = el('button', 'k-pbtn');
      next.type = 'button';
      var ng = el('span', 'k-skip');
      ng.appendChild(el('i', 'k-tri-r'));
      ng.appendChild(el('span', 'k-bar'));
      next.appendChild(ng);
      next.addEventListener('click', opts.onNext);
      mount.appendChild(next);
    }

    var info = null, title = null, artist = null;
    if (full) {
      info = el('div', null);
      info.style.cssText = 'min-width:0;flex:1';
      title = el('div', 'k-ptit', '-');
      artist = el('div', 'k-part', '');
      info.appendChild(title);
      info.appendChild(artist);
      mount.appendChild(info);
    }

    var cur = el('span', 'k-time', '0:00');
    var dur = el('span', 'k-time', '0:00');
    var seek = el('div', 'k-seek' + (full ? '' : ' thin'));
    var tr = el('div', 'k-tr');
    var fl = el('div', 'k-fl');
    var th = el('div', 'k-th');
    tr.appendChild(fl); tr.appendChild(th); seek.appendChild(tr);

    // The mini player is 52px of phone width with a tab bar under it - a seek
    // bar there would be a 4px target between two other targets, so it shows
    // the timecode pair only and the bar/inline sizes carry the scrub.
    if (size === 'mini') {
      var t = el('span', 'k-time', '0:00 / 0:00');
      mount.appendChild(t);
      cur = t; dur = null;
    } else {
      mount.appendChild(cur);
      mount.appendChild(seek);
      mount.appendChild(dur);
    }

    function seekTo(ev) {
      if (!audio.duration) return;
      var r = tr.getBoundingClientRect();
      var x = ((ev.touches ? ev.touches[0].clientX : ev.clientX) - r.left) / r.width;
      audio.currentTime = Math.max(0, Math.min(1, x)) * audio.duration;
    }
    seek.addEventListener('click', seekTo);

    function setGlyph() {
      glyph.className = audio.paused
        ? 'k-tri-r' + (full ? '' : ' sm')
        : 'k-pause' + (full ? '' : ' sm');
    }
    function sync() {
      setGlyph();
      var d = audio.duration || 0;
      var p = d ? (audio.currentTime / d) * 100 : 0;
      fl.style.width = p + '%';
      th.style.left = p + '%';
      if (dur) {
        cur.textContent = fmtTime(audio.currentTime);
        dur.textContent = fmtTime(d);
      } else {
        cur.textContent = fmtTime(audio.currentTime) + ' / ' + fmtTime(d);
      }
    }
    audio.addEventListener('timeupdate', sync);
    audio.addEventListener('play', setGlyph);
    audio.addEventListener('pause', setGlyph);
    audio.addEventListener('loadedmetadata', sync);
    sync();

    return {
      el: mount,
      sync: sync,
      show: function (on) { mount.classList.toggle('on', on !== false); },
      setTrack: function (t, a) {
        if (title) title.textContent = t || '-';
        if (artist) artist.textContent = a && a !== '-' ? a : '';
      },
    };
  }

  // ── Bulk download. Lifted from the portal, where it grew for Voice notes and
  //    the Files browser; the Notes merge moved one of those callers to the
  //    other document, so it lives here now and both call the same code.
  //
  //    The device stays a dumb one-file-at-a-time server: all bundling happens
  //    in the browser, transfers are strictly sequential (the async server
  //    wedges on concurrent reads), and a beforeunload guard keeps the user on
  //    the page while bytes are moving. iOS cannot do multi-file downloads, so
  //    there it is the single .zip or nothing.
  var dlItems = [], dlZipName = 'files.zip', dlTotal = 0;
  var dlAbort = null, dlActive = false, dlStartMs = 0, dlBox = null;

  var DL_CAP = { ios: 100 * 1048576, android: 200 * 1048576, desktop: 500 * 1048576 };

  function dlDeviceClass() {
    var ua = navigator.userAgent || '';
    if (/iP(hone|ad|od)/.test(ua) ||
        (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1)) return 'ios';
    if (/Android/.test(ua)) return 'android';
    return 'desktop';
  }

  // Built on first use so a surface that never bulk-downloads pays no markup.
  function dlEnsure() {
    if (dlBox) return dlBox;
    var o = el('div', 'k-dl');
    o.innerHTML =
      '<div class="k-dl-box">' +
      '<div class="k-dl-choice">' +
      '<h3 class="k-dl-title">Download files</h3>' +
      '<div class="k-dl-size"></div>' +
      '<div class="k-dl-btns">' +
      '<button class="k-dl-b pri k-dl-zip">Bundle as one .zip</button>' +
      '<button class="k-dl-b k-dl-indiv">Individual files</button>' +
      '</div><button class="k-dl-b k-dl-cancel">Cancel</button></div>' +
      '<div class="k-dl-prog" style="display:none">' +
      '<h3 class="k-dl-ptitle">Downloading...</h3>' +
      '<div class="k-dl-bar"><div class="k-dl-fill"></div></div>' +
      '<div class="k-dl-stat">0 B / 0 B</div>' +
      '<div class="k-dl-rate"></div><div class="k-dl-file"></div>' +
      '<div class="k-dl-warn">Keep this page open until it finishes.</div>' +
      '<button class="k-dl-b k-dl-stop">Cancel</button></div></div>';
    document.body.appendChild(o);
    dlBox = {
      root: o,
      q: function (c) { return o.querySelector('.' + c); },
    };
    dlBox.q('k-dl-zip').addEventListener('click', function () { dlStart('zip'); });
    dlBox.q('k-dl-indiv').addEventListener('click', function () { dlStart('individual'); });
    dlBox.q('k-dl-cancel').addEventListener('click', dlClose);
    dlBox.q('k-dl-stop').addEventListener('click', function () { if (dlAbort) dlAbort.abort(); });
    return dlBox;
  }

  var crcTab = null;
  function crc32Update(crc, buf) {
    if (!crcTab) {
      crcTab = new Uint32Array(256);
      for (var n = 0; n < 256; n++) {
        var c = n;
        for (var k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
        crcTab[n] = c >>> 0;
      }
    }
    for (var i = 0; i < buf.length; i++) crc = (crcTab[(crc ^ buf[i]) & 0xFF] ^ (crc >>> 8)) >>> 0;
    return crc >>> 0;
  }
  function zipLocal(nameBytes, crc, size) {
    var h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(6, 0x0800, true);
    h.setUint16(8, 0, true); h.setUint16(10, 0, true); h.setUint16(12, 0x21, true);
    h.setUint32(14, crc, true); h.setUint32(18, size, true); h.setUint32(22, size, true);
    h.setUint16(26, nameBytes.length, true); h.setUint16(28, 0, true);
    return new Uint8Array(h.buffer);
  }
  function zipCentral(nameBytes, crc, size, offset) {
    var h = new DataView(new ArrayBuffer(46));
    h.setUint32(0, 0x02014b50, true); h.setUint16(4, 20, true); h.setUint16(6, 20, true);
    h.setUint16(8, 0x0800, true); h.setUint16(10, 0, true); h.setUint16(12, 0, true);
    h.setUint16(14, 0x21, true); h.setUint32(16, crc, true); h.setUint32(20, size, true);
    h.setUint32(24, size, true); h.setUint16(28, nameBytes.length, true);
    h.setUint16(30, 0, true); h.setUint16(32, 0, true); h.setUint16(34, 0, true);
    h.setUint16(36, 0, true); h.setUint32(38, 0, true); h.setUint32(42, offset, true);
    return new Uint8Array(h.buffer);
  }
  function zipEOCD(count, cdSize, cdOffset) {
    var h = new DataView(new ArrayBuffer(22));
    h.setUint32(0, 0x06054b50, true); h.setUint16(4, 0, true); h.setUint16(6, 0, true);
    h.setUint16(8, count, true); h.setUint16(10, count, true); h.setUint32(12, cdSize, true);
    h.setUint32(16, cdOffset, true); h.setUint16(20, 0, true);
    return new Uint8Array(h.buffer);
  }

  async function dlFetchBytes(item, baseReceived) {
    var resp = await fetch(item.url, { signal: dlAbort.signal });
    if (!resp.ok) throw new Error('HTTP ' + resp.status);
    var reader = resp.body.getReader();
    var chunks = [], size = 0, crc = 0xFFFFFFFF;
    for (;;) {
      var r = await reader.read();
      if (r.done) break;
      chunks.push(r.value); size += r.value.length; crc = crc32Update(crc, r.value);
      dlSetProgress(baseReceived + size);
    }
    var data = new Uint8Array(size), p = 0;
    for (var i = 0; i < chunks.length; i++) { data.set(chunks[i], p); p += chunks[i].length; }
    return { data: data, crc: (crc ^ 0xFFFFFFFF) >>> 0 };
  }

  async function dlZip() {
    var enc = new TextEncoder();
    var parts = [], central = [], offset = 0, received = 0;
    for (var i = 0; i < dlItems.length; i++) {
      dlSetFile(i, dlItems[i].name);
      var nameBytes = enc.encode(dlItems[i].name);
      var got = await dlFetchBytes(dlItems[i], received);
      received += got.data.length;
      var lh = zipLocal(nameBytes, got.crc, got.data.length);
      parts.push(lh, nameBytes, got.data);
      central.push({ nameBytes: nameBytes, crc: got.crc, size: got.data.length, offset: offset });
      offset += lh.length + nameBytes.length + got.data.length;
    }
    var cdSize = 0;
    for (var j = 0; j < central.length; j++) {
      var e = central[j];
      var ch = zipCentral(e.nameBytes, e.crc, e.size, e.offset);
      parts.push(ch, e.nameBytes);
      cdSize += ch.length + e.nameBytes.length;
    }
    parts.push(zipEOCD(central.length, cdSize, offset));
    dlSave(new Blob(parts, { type: 'application/zip' }), dlZipName);
  }

  async function dlIndividual() {
    var received = 0;
    for (var i = 0; i < dlItems.length; i++) {
      var it = dlItems[i];
      dlSetFile(i, it.name);
      var got = await dlFetchBytes(it, received);
      received += got.data.length;
      // Typed so iOS/Safari names it correctly rather than .txt.
      dlSave(new Blob([got.data], { type: it.type || 'application/octet-stream' }), it.name);
      await new Promise(function (r) { setTimeout(r, 250); });   // let the server release the socket
    }
  }

  function dlSave(blob, filename) {
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url; a.download = filename;
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(url); }, 4000);
  }

  function dlFmtEta(s) {
    return s >= 60 ? (Math.floor(s / 60) + 'm ' + String(s % 60).padStart(2, '0') + 's') : (s + 's');
  }
  function dlSetProgress(received) {
    var b = dlEnsure();
    var pct = dlTotal > 0 ? Math.min(100, Math.round((received / dlTotal) * 100)) : 0;
    b.q('k-dl-fill').style.width = pct + '%';
    b.q('k-dl-stat').textContent = fmtBytes(received) + ' / ' + fmtBytes(dlTotal) + ' (' + pct + '%)';
    var elapsed = (Date.now() - dlStartMs) / 1000;
    if (elapsed > 0.4 && received > 0) {
      var rate = received / elapsed;
      var eta = rate > 0 ? Math.round(Math.max(0, dlTotal - received) / rate) : 0;
      b.q('k-dl-rate').textContent = fmtBytes(rate) + '/s' +
        (received < dlTotal ? '  -  ~' + dlFmtEta(eta) + ' left' : '');
    }
  }
  function dlSetFile(i, name) {
    dlEnsure().q('k-dl-file').textContent =
      'File ' + (i + 1) + ' of ' + dlItems.length + ': ' + name;
  }

  // Open the chooser. items: [{url,name,bytes,type}]; label is the plural noun
  // shown in the title ("voice notes" / "files").
  function dlBegin(items, zipName, label) {
    if (!items.length) return;
    var b = dlEnsure();
    dlItems = items; dlZipName = zipName;
    dlTotal = items.reduce(function (s, it) { return s + (it.bytes || 0); }, 0);
    var dev = dlDeviceClass(), cap = DL_CAP[dev] || DL_CAP.desktop;
    var over = dlTotal > cap;
    b.q('k-dl-title').textContent = 'Download ' + items.length + ' ' + label;
    b.q('k-dl-size').textContent = 'Total: ' + fmtBytes(dlTotal) +
      (over ? '  (too large to bundle on this device, ~' + fmtBytes(cap) + ' max)' : '');
    b.q('k-dl-zip').disabled = over;
    b.q('k-dl-indiv').style.display = dev === 'ios' ? 'none' : '';
    if (dev === 'ios' && over) {
      b.q('k-dl-size').textContent = 'Total: ' + fmtBytes(dlTotal) +
        ' is too large to bundle here. Download files one at a time using the ' +
        'Download button on each one.';
    }
    b.q('k-dl-choice').style.display = '';
    b.q('k-dl-prog').style.display = 'none';
    b.root.classList.add('on');
  }

  var dlToast = function () {};
  async function dlStart(mode) {
    var b = dlEnsure();
    b.q('k-dl-choice').style.display = 'none';
    b.q('k-dl-prog').style.display = '';
    b.q('k-dl-ptitle').textContent = mode === 'zip' ? 'Building your .zip...' : 'Downloading...';
    b.q('k-dl-fill').style.width = '0%';
    b.q('k-dl-rate').textContent = '';
    dlStartMs = Date.now();
    dlSetProgress(0);
    dlAbort = new AbortController();
    dlActive = true;
    try {
      if (mode === 'zip') await dlZip(); else await dlIndividual();
      dlToast(mode === 'zip' ? 'Bundle ready' : 'Downloads done');
    } catch (e) {
      if (e && e.name === 'AbortError') dlToast('Download cancelled');
      else dlToast('Download failed' + (e && e.message ? ': ' + e.message : ''));
    } finally {
      dlActive = false;
      dlClose();
    }
  }
  function dlClose() {
    var b = dlEnsure();
    b.root.classList.remove('on');
    b.q('k-dl-choice').style.display = '';
    b.q('k-dl-prog').style.display = 'none';
    b.q('k-dl-fill').style.width = '0%';
    b.q('k-dl-rate').textContent = '';
  }
  window.addEventListener('beforeunload', function (e) {
    if (dlActive) { e.preventDefault(); e.returnValue = ''; }
  });

  return {
    DEST: DEST,
    nav: nav,
    setActive: setActive,
    conn: conn,
    sig: sig,
    gate: gate,
    player: player,
    download: dlBegin,
    onToast: function (fn) { dlToast = fn; },
    fmtBytes: fmtBytes,
    fmtTime: fmtTime,
    esc: esc,
  };
})();
/* CF-KIT-JS:END */
</script>
<script>
// ─── State ───
let files=[], playlists=[], queue=[], queueIdx=-1, currentView='table';
let flatTracks=[], sortCol='title', sortAsc=true, searchQuery='';
let allFolders=[]; // cached folder paths for move/upload
let nowPlayingUrl='', playingSource=''; // 'tracks' or 'playlist'
// What the kit shows in the sidebar footer / masthead chips. Two endpoints feed
// it (status for the version, wifi/status for the networks), so it accumulates
// here and is re-applied rather than each caller clearing the other's half.
let deviceVersion='', connState={};
let currentPage='media', mediaSummary='';
const audio=document.getElementById('audio');
const $=id=>document.getElementById(id);

// ─── Utilities ───
function fmt(b){if(b<1024)return b+' B';if(b<1048576)return(b/1024).toFixed(1)+' KB';return(b/1048576).toFixed(1)+' MB'}
function fmtTime(s){if(isNaN(s))return'0:00';s=Math.floor(s);return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')}
function basename(p){const i=p.lastIndexOf('/');return i>=0?p.substring(i+1):p}
function stripExt(n){const i=n.lastIndexOf('.');return i>0?n.substring(0,i):n}
function toast(msg){const t=$('toast');t.textContent=msg;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2000)}
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;')}

// ─── Navigation ───
// Three of the five destinations render in this document; Notes and Listen are
// the companion's and the kit renders them as plain links. The menu button and
// the slide-out drawer are gone - the tab bar is the phone navigation on both
// documents now.
const PAGES={media:'Media',files:'Files',settings:'Settings'};
function showPage(p){
  // Voice notes retired into the companion's merged Notes view. Anything still
  // pointing here - a bookmark, the old nav - lands where the notes actually are.
  if(p==='voice'){location.href='/web/#notes';return}
  if(!PAGES[p])p='media';
  currentPage=p;
  $('mediaPage').style.display=p==='media'?'':'none';
  $('filesPage').style.display=p==='files'?'':'none';
  $('settingsPage').style.display=p==='settings'?'':'none';
  $('pageTitle').textContent=PAGES[p];
  showStatus();
  CFK.setActive(p);
  if(location.hash.slice(1)!==p)history.replaceState(null,'','#'+p);
  if(p==='files')loadBrowse(fbPath);
}
function showStatus(){
  $('statusBar').textContent=currentPage==='media'?mediaSummary:'';
}
function pageFromHash(){
  const h=(location.hash||'').replace('#','').split('/')[0].toLowerCase();
  return h==='voice'?'voice':(PAGES[h]?h:'media');
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
      // Square glyph buttons, as the artboard draws them: play, add to a
      // playlist, remove. All three stay on a phone - the artboard shows one,
      // but dropping delete here would leave no way to remove a track from
      // Media on a phone at all.
      '<td class="col-acts"><span class="acts">'+
        '<button class="btn btn-play btn-ico" title="Play" onclick="playTrack(\''+esc(t.path)+'\',\''+esc(t.title)+'\',\''+esc(t.artist)+'\',\'tracks\')"><i class="k-tri-r sm"></i></button>'+
        '<button class="btn btn-add btn-ico" title="Add to playlist" onclick="showAddToPl(event,\''+esc(t.path)+'\')">+</button>'+
        '<button class="btn btn-del btn-ico" title="Delete" onclick="delPath(\''+esc(t.path)+'\')">&times;</button>'+
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
    // Short enough to sit on the title's baseline at 390px, which is where the
    // artboards put it ("7 tracks · 24.0 MB"). Middot separator, matching every
    // other meta line on both surfaces. It describes Media, so only Media shows
    // it - a track count above the WiFi settings answers nothing.
    mediaSummary=s.files+' track'+(s.files===1?'':'s')+' · '+fmt(s.usedBytes);
    showStatus();
    deviceVersion=s.version||'';
    applyConn({});
  }catch(e){}
}
function applyConn(patch){
  Object.assign(connState,patch);
  CFK.conn({apIp:connState.apIp,ssid:connState.ssid,staIp:connState.staIp,version:deviceVersion});
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

// ─── Voice notes: retired from this document ───
// Browsing recordings lived here and transcribing them lived in the companion:
// two lists of the same files, which is most of what made the two surfaces read
// as two sites. They are now one Notes destination on /web/, beside the engine
// that turns them into text. The multi-select download and delete that grew here
// went with them - into the shared kit, so the Files browser below and the
// companion's Notes now run the same code (CFK.download).

// ─── Files browser (raw card, nested folder navigation) ───
// A power-user view of the whole SD card: navigate into one folder at a time
// (NOT a flat dump of every file), see size + modified date, multi-select,
// upload into the current folder, rename, and delete (files and whole folders).
// Downloads go through the shared kit's serialized fetch -> zip path.
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
  CFK.download(items,'files.zip','files');
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
// The transport, seek bar and timecodes are the shared kit's component - the
// same one the companion uses for its inline row players - so the native audio
// element is gone from both surfaces. It renders as the 76px bar on a laptop and
// the 52px mini above the tab bar on a phone; the kit decides which from width.
let _fromQueue=false;
const player=CFK.player($('playerBar'),audio,
  window.matchMedia('(max-width:699px)').matches?'mini':'bar',
  {onPrev:()=>playerPrev(),onNext:()=>playerNext()});
function playTrack(url,fallbackTitle,fallbackArtist,source){
  audio.src=url;audio.play();
  player.show(true);
  nowPlayingUrl=url;
  if(source)playingSource=source;
  let title=fallbackTitle||stripExt(basename(url));
  let artist=fallbackArtist||'';
  // Look up real ID3 metadata from flatTracks
  const found=flatTracks.find(t=>t.path===url);
  if(found){title=found.title||title;artist=found.artist||artist}
  player.setTrack(title,artist);
  // Build queue from all tracks (unless navigating within existing queue)
  if(!_fromQueue&&flatTracks.length){
    queue=flatTracks.map(t=>({url:t.path,title:t.title,artist:t.artist}));
    queueIdx=queue.findIndex(q=>q.url===url);
    if(queueIdx<0)queueIdx=0;
  }
  _fromQueue=false;
  highlightPlaying();
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

// Progress, scrubbing and the play/pause glyph are the kit player's job; all
// that is left here is advancing the queue.
audio.addEventListener('ended',playerNext);

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
    const sc=$('wifiStatusContent');
    $('apIPDisplay').textContent=s.ap_ip||'192.168.4.1';
    // The kit owns where these facts appear: sidebar footer on a laptop, the
    // masthead chips on a phone. Same two lines on the companion.
    applyConn({apIp:s.ap_ip,ssid:s.connected?s.ssid:'',staIp:s.ip});
    if(s.connected){
      const mdns=s.mdns||'';
      // Same copy rule as the panel below: "address", never "IP"; the mDNS name
      // is described by what it does rather than by the protocol that serves it.
      sc.innerHTML='<div class="info-row"><span class="label">Status</span><span class="val" style="color:var(--success)">Connected &#183; '+esc(s.ssid)+'</span></div>'+
        '<div class="info-row"><span class="label">Address</span><span class="val accent">'+s.ip+'</span></div>'+
        (mdns?'<div class="info-row"><span class="label">Also reachable at</span><span class="val accent">'+mdns+'</span></div>':'')+
        '<div style="margin-top:12px"><button class="btn btn-del" onclick="forgetWifi()">Forget network</button></div>';
    }else if(s.ssid&&s.status==='connecting'){
      sc.innerHTML='<div class="info-row"><span class="label">Status</span><span class="val"><span class="wifi-spinner"></span>Connecting to '+esc(s.ssid)+'...</span></div>';
      setTimeout(loadWifiStatus,2000);
    }else{
      sc.innerHTML='<div class="info-row"><span class="label">Status</span><span class="val">Not connected</span></div>';
    }
  }catch(e){}
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
    // Strength is four CSS bars from the kit and "locked" is a text chip. Both
    // were emoji, which are fixed multi-colour and cannot be tinted to match the
    // surface - the defect class the design brief singled out, and this list was
    // its last live instance.
    $('networkList').innerHTML=nets.map(n=>
      '<li onclick="selectNetwork(\''+esc(n.ssid)+'\')">'+
      '<span class="ssid">'+esc(n.ssid)+'</span>'+
      (n.secure?'<span class="lock">Locked</span>':'')+
      CFK.sig(n.rssi)+
      '</li>'
    ).join('');
  }catch(e){$('networkList').innerHTML='<li class="empty" style="padding:12px">Scan failed</li>'}
  finally{btn.disabled=false;btn.textContent='Scan again'}
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
// The kit builds the navigation before anything else runs, so the tab bar and
// the sidebar are up whether or not the device answers the API calls below.
CFK.nav('portal',pageFromHash(),showPage);
CFK.onToast(toast);
window.addEventListener('hashchange',()=>showPage(pageFromHash()));
showPage(pageFromHash());
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
