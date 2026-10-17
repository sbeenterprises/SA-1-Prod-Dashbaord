#!/bin/bash
# Swiftships branding for an OSS Grafana install (called by deploy/install.sh,
# safe to run on its own after any Grafana upgrade):
#   sudo deploy/brand.sh            # apply
#   sudo deploy/brand.sh --restore  # put Grafana's own assets back
#
# Grafana OSS has no white-label configuration, so this overwrites the
# shipped assets in /usr/share/grafana/public: the nav/login logo, favicon,
# touch icon, the login-page background, the browser-tab title and the
# "Welcome to Grafana" login heading. Originals are kept as *.grafana-orig
# the first time so --restore works. An apt upgrade of grafana restores
# the stock files; re-run this script (or install.sh) afterwards.
#
# Wording can be tuned in /etc/usv/usv.env:
#   BRAND_TITLE="Swiftships USV Console"     (browser tab / loading screen)
#   BRAND_WELCOME="Welcome to Swiftships"    (login heading)
#   BRAND_SUBTITLE="Autonomous Dashboard"   (under the heading)
#   BRAND_APP_TITLE="SA-1 Dashboard"        (replaces "Grafana" next to the
#                                            logo in the navigation menu)
set -euo pipefail

GPUB=${GPUB:-/usr/share/grafana/public}
BR="$(cd "$(dirname "$0")/branding" && pwd)"
[[ -f /etc/usv/usv.env ]] && source /etc/usv/usv.env
TITLE=${BRAND_TITLE:-Swiftships USV Console}
WELCOME=${BRAND_WELCOME:-Welcome to Swiftships}

[[ $EUID -eq 0 ]] || { echo "run as root: sudo deploy/brand.sh"; exit 1; }
[[ -d $GPUB/img ]] || { echo "Grafana public dir not found at $GPUB"; exit 1; }

keep() { [[ -f "$1" && ! -f "$1.grafana-orig" ]] && cp -p "$1" "$1.grafana-orig" || true; }

if [[ ${1:-} == "--restore" ]]; then
  find "$GPUB" -name '*.grafana-orig' | while read -r orig; do
    mv -f "$orig" "${orig%.grafana-orig}"
  done
  rm -f "$GPUB"/img/swiftships_*
  echo "Grafana stock assets restored."
  exit 0
fi

# ---------------------------------------------------------------- icons
# The nav bar and login page are dark: use the white-knockout mark there.
# The favicon keeps the navy artwork (light browser tabs).
for f in grafana_icon.svg grafana_mask_icon.svg grafana_mask_icon_white.svg; do
  keep "$GPUB/img/$f"
  cp "$BR/swiftships_logo_light.svg" "$GPUB/img/$f"
done
keep "$GPUB/img/fav32.png";          cp "$BR/fav32.png" "$GPUB/img/fav32.png"
keep "$GPUB/img/apple-touch-icon.png"; cp "$BR/apple-touch-icon.png" "$GPUB/img/apple-touch-icon.png"
# Some builds carry hashed copies of the icon under build/; replace those too.
find "$GPUB/build" -name 'grafana_icon*.svg' 2>/dev/null | while read -r f; do
  keep "$f"; cp "$BR/swiftships_logo_light.svg" "$f"
done
# Assets the dashboards reference: header band wordmarks and the geomap
# vessel symbol (Grafana loads marker SVGs from public/img).
cp "$BR/swiftships_wordmark.png" "$BR/swiftships_wordmark_light.png" \
   "$BR/swiftships_mark.png" "$BR/swiftships_mark_96.png" \
   "$BR/swiftships_wordmark_chip.png" "$BR/swiftships_wordmark_chip_sm.png" \
   "$BR/swiftships_boat.svg" "$GPUB/img/"
# Remove the custom map pin an earlier build installed: Grafana 13.2 draws
# custom SVG map markers as black squares, so the maps use built-in shapes.
rm -f "$GPUB/img/icons/marker/usv_pin.svg"

# ------------------------------------------------------- login background
# Navy gradient in place of Grafana's orange/purple artwork (both themes).
mk_bg() {
cat > "$1" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1920 1080" preserveAspectRatio="xMidYMid slice">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#0b2a49"/>
      <stop offset="0.55" stop-color="#08243f"/>
      <stop offset="1" stop-color="#041526"/>
    </linearGradient>
    <linearGradient id="w" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#00bcd4" stop-opacity="0"/>
      <stop offset="0.5" stop-color="#00bcd4" stop-opacity="0.35"/>
      <stop offset="1" stop-color="#00bcd4" stop-opacity="0"/>
    </linearGradient>
  </defs>
  <rect width="1920" height="1080" fill="url(#g)"/>
  <path d="M0 760 C 320 700, 520 820, 860 770 S 1500 700, 1920 760 L1920 1080 L0 1080 Z" fill="#06203a" opacity="0.9"/>
  <path d="M0 800 C 300 760, 600 860, 960 810 S 1600 760, 1920 800" fill="none" stroke="url(#w)" stroke-width="3"/>
  <path d="M0 860 C 300 820, 600 920, 960 870 S 1600 820, 1920 860" fill="none" stroke="url(#w)" stroke-width="2"/>
</svg>
EOF
}
# Grafana 13 loads hashed copies (build/static/img/g8_login_dark.<hash>.svg);
# older builds use img/. Replace every copy, wherever it lives.
find "$GPUB" \( -name 'g8_login_*.svg' -o -name 'login_background_*.svg' \) \
     -not -name '*.grafana-orig' | while read -r f; do
  keep "$f"; mk_bg "$f"
done

# ------------------------------------------------- title and login text
# index.html is a Go template; [[.AppTitle]] is the hard-coded "Grafana".
IDX="$GPUB/views/index.html"
keep "$IDX"
sed -i -e "s|\[\[\.AppTitle\]\]|$TITLE|g" \
       -e "s|Loading Grafana|Loading $TITLE|g" "$IDX"

# The login heading is a string literal in a JS bundle (name is hashed).
grep -rl --include='*.js' -e 'Welcome to Grafana' "$GPUB/build" 2>/dev/null | while read -r f; do
  keep "$f"
  sed -i "s|Welcome to Grafana|$WELCOME|g" "$f"
done || true   # no match (already branded) must not abort under pipefail

# ------------------------------------------------------ login page style
# Grafana serves its hashed build assets (logo, login artwork, JS chunks)
# with a one-year immutable cache, so a browser that saw stock Grafana
# once keeps showing it after the files above are overwritten. The login
# look is therefore ALSO applied from views/index.html, which browsers
# never cache: CSS swaps the logo and artwork for Swiftships-named copies
# (cache-busted by content hash) and sets the heading/subtitle text.
#
# Verified Grafana 13 login DOM:
#   body.login-page > ... > div (::before = fixed background artwork)
#     > div > div.login-content-box > div > img(logo) + div > h1(heading)
SUBTITLE=${BRAND_SUBTITLE:-Autonomous Dashboard}
APP_TITLE=${BRAND_APP_TITLE:-SA-1 Dashboard}
cp "$BR/swiftships_logo_light.svg" "$GPUB/img/swiftships_login_logo.svg"
mk_bg "$GPUB/img/swiftships_login_bg.svg"
LOGO_V=$(md5sum "$GPUB/img/swiftships_login_logo.svg" | cut -c1-8)
BG_V=$(md5sum "$GPUB/img/swiftships_login_bg.svg" | cut -c1-8)
CSS_FILE=$(mktemp)
cat > "$CSS_FILE" <<CSS
<style id="swiftships-brand">
/* Swiftships branding (deploy/brand.sh). Cache-proof: assets below carry a
   content hash, so browsers holding Grafana's stock files still see this. */
img[src*="grafana_icon"]{content:url("/public/img/swiftships_login_logo.svg?v=$LOGO_V")}
body.login-page div:has(> div > .login-content-box)::before{background-image:url("/public/img/swiftships_login_bg.svg?v=$BG_V")!important;opacity:1!important}
body.login-page .login-content-box{background:rgba(8,36,63,.92)!important;border:1px solid rgba(0,188,212,.35)!important;border-radius:10px!important;box-shadow:0 24px 64px rgba(0,0,0,.5)!important}
body.login-page .login-content-box img{filter:drop-shadow(0 2px 10px rgba(0,188,212,.35))}
body.login-page .login-content-box h1{font-size:0!important;line-height:0!important;color:#e8f1f8!important;letter-spacing:.02em}
body.login-page .login-content-box h1::before{content:"$WELCOME";display:block;font-size:28px;line-height:1.3;font-weight:500}
body.login-page .login-content-box img+div::after{content:"$SUBTITLE";display:block;font-size:14px;font-weight:600;letter-spacing:.14em;text-transform:uppercase;color:#00bcd4;margin-top:10px}
body.login-page .login-content-box label,body.login-page .login-content-box legend{color:#c9d6e2!important}
body.login-page button[type="submit"]{background:#1e6692!important;border-color:#1e6692!important;color:#fff!important}
body.login-page button[type="submit"]:hover{background:#00bcd4!important;border-color:#00bcd4!important;color:#04121f!important}
body.login-page input:focus,body.login-page input:focus-visible{box-shadow:0 0 0 2px #00bcd4!important;outline:none!important}
body.login-page a{color:#5fc9dd!important}
/* Navy theme: page background before Grafana has drawn (swiftships-theme-js) */
html.sw-navy,html.sw-navy body{background:#13203B!important}
</style>
<script id="swiftships-brand-js">
/* The navigation menu header shows the product name "Grafana" next to the
   logo (a constant in the OSS build, not a setting). Replace that text
   node, and any later re-render of it, with the vessel console name. */
(function(){var T=$(printf '%s' "$APP_TITLE" | sed 's/[\\"]/\\&/g; s/^/"/; s/$/"/');
function fix(n){if(n.nodeType===3){if(n.nodeValue.trim()==="Grafana")n.nodeValue=T;return}
if(n.nodeType!==1||n.tagName==="SCRIPT"||n.tagName==="STYLE")return;for(var c=n.firstChild;c;c=c.nextSibling)fix(c)}
new MutationObserver(function(ms){for(var i=0;i<ms.length;i++){var m=ms[i];if(m.type==="characterData")fix(m.target);else for(var j=0;j<m.addedNodes.length;j++)fix(m.addedNodes[j])}})
.observe(document.documentElement,{childList:true,subtree:true,characterData:true});})();
</script>
<script id="swiftships-fit-js">
/* Fit every dashboard to the screen (no scrolling) on any display: the
   pages are laid out to 19 grid rows for a 1080p console; here the whole
   page is zoomed so the grid's bottom edge meets the bottom of the window,
   down on a laptop, up on a large screen. The zoom is capped so Grafana
   always sees a layout at least 1300 px wide (zooming further would make
   it switch to its narrow one-column layout, which then no longer fits).
   Measured at zoom 1, re-evaluated only when the window size or the
   dashboard changes; off with ?fit=0 in the URL. */
(function(){
if(/[?&]fit=0(&|$)/.test(location.search))return;
var PAD=8,MIN=0.6,MINW=1300,t=null,key='';
function grid(){return document.querySelector('.react-grid-layout');}
function scrollOf(e){var s=0;for(e=e.parentNode;e&&e.nodeType===1;e=e.parentNode)s+=e.scrollTop||0;return s;}
function fit(){
  var g=grid(),b=document.body;
  if(!g){b.style.zoom='';key='';return;}
  var k=window.innerWidth+'x'+window.innerHeight+'|'+g.style.height+'|'+location.pathname;
  if(k===key)return;
  var prev=b.style.zoom;
  b.style.zoom='1';
  var r=g.getBoundingClientRect(),top=r.top+scrollOf(g),h=parseFloat(g.style.height)||r.height;
  if(!(h>200)||!(top>0)){b.style.zoom=prev;return;}
  var z=window.innerHeight/(top+h+PAD);
  z=Math.min(Math.max(1,window.innerWidth/MINW),Math.max(MIN,z));
  b.style.zoom=z.toFixed(3);key=k;
}
function sched(){clearTimeout(t);t=setTimeout(fit,250);}
window.addEventListener('resize',sched);
new MutationObserver(sched).observe(document.documentElement,{childList:true,subtree:true});
sched();
})();
</script>
CSS
cat >> "$CSS_FILE" <<'THEME'
<script id="swiftships-theme-js">
/* Theme picker (the "Theme" variable at the top of every dashboard,
   v1.2.33): Dark and Light are Grafana's own themes; Navy is the dark
   theme with the Swiftships navy page, panel and border colours. The
   choice is remembered in this browser and carried between tabs as
   var-theme. Grafana picks Light or Dark when the page is served, so a
   change reloads the page once. Navy rewrites the dark theme's colour
   literals as Grafana inserts its style rules, before anything is drawn. */
(function(){
var K='swiftships-theme',V=['dark','navy','light'];
function get(){try{return localStorage.getItem(K)||'';}catch(e){return '';}}
function put(v){try{localStorage.setItem(K,v);}catch(e){}}
function valid(v){return V.indexOf(v)>=0;}
var u=new URL(location.href),q=u.searchParams,tp=q.get('theme')||'',vt=q.get('var-theme')||'';
var want=valid(vt)?vt:(valid(get())?get():'dark');
/* The Light / Dark chips (?theme=) win over a stale var-theme. */
if(tp==='light')want='light';else if(tp==='dark'&&want==='light')want='dark';
put(want);
if(/^\/d\//.test(u.pathname)&&vt!==want){q.set('var-theme',want);history.replaceState(history.state,'',u.toString());}
var applied=want;
if(want==='navy'){
  document.documentElement.classList.add('sw-navy');
  /* Pairs of (dark theme colour, navy colour). Grafana's index.html is a
     Go template whose delimiters are two opening or two closing square
     brackets: never write two in a row in any injected block (v1.2.33 did,
     and Grafana sent an empty page; tools/smoke_test.sh checks for it). */
  var FROM=[/#111217/gi,/rgb\(17, ?18, ?23\)/g,/#181b1f/gi,/rgb\(24, ?27, ?31\)/g,
    /#22252b/gi,/rgb\(34, ?37, ?43\)/g,/rgba\(204, ?204, ?220, ?0\.12\)/g,
    /rgba\(204, ?204, ?220, ?0\.2\)/g];
  var TO=['#13203B','rgb(19, 32, 59)','#1B2B4B','rgb(27, 43, 75)',
    '#22355A','rgb(34, 53, 90)','#2B3F66','#35507F'];
  function navy(t){if(typeof t!=='string')return t;for(var i=0;i<FROM.length;i++)t=t.replace(FROM[i],TO[i]);return t;}
  var ins=CSSStyleSheet.prototype.insertRule;
  CSSStyleSheet.prototype.insertRule=function(r,i){return ins.call(this,navy(r),i);};
  /* Style elements written as text (theme CSS, development builds). */
  new MutationObserver(function(ms){for(var a=0;a<ms.length;a++)for(var b=0;b<ms[a].addedNodes.length;b++){
    var n=ms[a].addedNodes[b];if(n.nodeType===1&&n.tagName==='STYLE'&&!n.id){var t=navy(n.textContent);if(t!==n.textContent)n.textContent=t;}}})
  .observe(document.documentElement,{childList:true,subtree:true});
}
function bootLight(){var b=window.grafanaBootData;
  if(b&&b.user)return !!(b.user.lightTheme||b.user.theme==='light');
  return !!(document.body&&document.body.classList.contains('theme-light'));}
function go(v){var x=new URL(location.href);x.searchParams.set('var-theme',v);
  x.searchParams.set('theme',v==='light'?'light':'dark');
  var n=0;try{n=+sessionStorage.getItem(K+'-n')||0;sessionStorage.setItem(K+'-n',n+1);}catch(e){}
  if(n<3)location.replace(x.toString());}
/* Served in the wrong Grafana theme for the choice: reload once. */
document.addEventListener('DOMContentLoaded',function(){
  if((want==='light')!==bootLight())go(want);
  else try{sessionStorage.removeItem(K+'-n');}catch(e){}});
/* Choice changed in the dropdown: Grafana updates var-theme in the URL. */
setInterval(function(){var v=new URL(location.href).searchParams.get('var-theme');
  if(valid(v)&&v!==applied){applied=v;put(v);try{sessionStorage.removeItem(K+'-n');}catch(e){}go(v);}},400);
})();
</script>
THEME
# Replace any previous block, then insert before </head>.
perl -0pi -e 's|<style id="swiftships-brand">.*?</style>\n?||s; s|<script id="swiftships-brand-js">.*?</script>\n?||s; s|<script id="swiftships-fit-js">.*?</script>\n?||s; s|<script id="swiftships-theme-js">.*?</script>\n?||s' "$IDX"
CSS_FILE="$CSS_FILE" perl -0pi -e 'BEGIN{local $/; open(F, "<", $ENV{CSS_FILE}) or die; $css=<F>; close F} s|</head>|$css</head>|' "$IDX"
rm -f "$CSS_FILE"

# Grafana parses views/index.html once at startup and serves it from memory:
# a running server keeps showing the old title/CSS until restarted.
if [[ -z ${GPUB_NO_RESTART:-} ]] && systemctl is-active --quiet grafana-server 2>/dev/null; then
  systemctl restart grafana-server
  echo "grafana-server restarted (index.html is cached in memory at startup)."
fi

echo "Swiftships branding applied (title: '$TITLE', login: '$WELCOME')."
echo "Reload the browser with Ctrl+F5 to drop cached Grafana icons."
