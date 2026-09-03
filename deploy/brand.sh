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
# Assets for the dashboards' own header band (text panels reference these).
cp "$BR/swiftships_wordmark.png" "$BR/swiftships_wordmark_light.png" \
   "$BR/swiftships_mark.png" "$BR/swiftships_mark_96.png" \
   "$BR/swiftships_wordmark_chip.png" "$GPUB/img/"

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
for f in g8_login_dark.svg g8_login_light.svg login_background_dark.svg login_background_light.svg; do
  [[ -f "$GPUB/img/$f" ]] || continue
  keep "$GPUB/img/$f"; mk_bg "$GPUB/img/$f"
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
done

# ------------------------------------------------------ login page style
# Scoped to Grafana's login classes (login-content-box, login-branding,
# login-form); harmless where a class is absent. Navy card on the navy
# background, cyan focus/primary, Swiftships type colour.
if ! grep -q 'id="swiftships-brand"' "$IDX"; then
  sed -i 's|</head>|<style id="swiftships-brand">\
.login-content-box{background:rgba(8,36,63,.90)!important;border:1px solid rgba(0,188,212,.35)!important;box-shadow:0 24px 64px rgba(0,0,0,.5)!important;border-radius:10px!important}\
.login-branding h1,.login-content-box h1{color:#e8f1f8!important;letter-spacing:.02em}\
.login-branding img,.login-content-box img{filter:drop-shadow(0 2px 8px rgba(0,188,212,.35))}\
.login-form button[type=submit],.login-content-box button[type=submit]{background:#1e6692!important;border-color:#1e6692!important}\
.login-form button[type=submit]:hover,.login-content-box button[type=submit]:hover{background:#00bcd4!important;border-color:#00bcd4!important;color:#04121f!important}\
.login-content-box input:focus{box-shadow:0 0 0 2px #00bcd4!important}\
</style></head>|' "$IDX"
fi

echo "Swiftships branding applied (title: '$TITLE', login: '$WELCOME')."
echo "Reload the browser with Ctrl+F5 to drop cached Grafana icons."
