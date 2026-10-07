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
CSS
# Replace any previous block, then insert before </head>.
perl -0pi -e 's|<style id="swiftships-brand">.*?</style>\n?||s; s|<script id="swiftships-brand-js">.*?</script>\n?||s' "$IDX"
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
