#!/usr/bin/env bash
# 16:9 logo for marth Follower Overhaul (1920x1080) — the banner's motif and
# house style (dark ground, ember-crimson glow, round companion's shield, gold
# P052 type) recomposed for a 16:9 canvas: shield as a centered hero in the
# upper third above stacked, centered typography. Matches the sibling logo-16x9
# format. Use for splash / Nexus header / video thumbnail. Sibling of
# make_banner.sh.
set -euo pipefail
cd "$(dirname "$0")"
W=1920; H=1080
P052=/usr/share/fonts/opentype/urw-base35/P052-Roman.otf
P052I=/usr/share/fonts/opentype/urw-base35/P052-Italic.otf

# Round shield, centered horizontally, upper third (hero). ~1.9x the banner
# shield, re-centred on cx=960.
CX=960; CY=330; R=185
RI=122
RB=50

# 1) Base: near-black vertical gradient + soft ember radial glow.
magick -size ${W}x${H} gradient:'#16181d-#08090b' \
  \( -size ${W}x${H} radial-gradient:'#2b1a16-#000000' -evaluate multiply 0.9 \) \
  -compose screen -composite base.png

# 2) The shield: blurred crimson glow underlayer, translucent face, glowing
#    inner ring, gold strap rivets, crisp gold rim + boss, bright glint. Same
#    recipe as the banner, scaled.
magick base.png \
  \( -clone 0 -fill none -stroke '#e86a52' -strokewidth 16 \
     -draw "circle $CX,$CY $CX,$((CY-R))" -blur 0x13 \) \
  -compose screen -composite \
  -stroke none -fill 'rgba(190,90,66,0.16)' \
  -draw "circle $CX,$CY $CX,$((CY-R))" \
  -fill 'rgba(232,106,82,0.30)' -draw "circle $CX,$CY $CX,$((CY-RI))" \
  -stroke '#f0937e' -strokewidth 2.6 -fill none \
  -draw "circle $CX,$CY $CX,$((CY-RI))" \
  -stroke none -fill 'rgba(245,223,168,0.85)' \
     -draw "circle $((CX-85)),$((CY-85)) $((CX-85)),$((CY-78))" \
     -draw "circle $((CX+85)),$((CY-85)) $((CX+85)),$((CY-78))" \
     -draw "circle $((CX-85)),$((CY+85)) $((CX-85)),$((CY+78))" \
     -draw "circle $((CX+85)),$((CY+85)) $((CX+85)),$((CY+78))" \
  -fill none -stroke '#e8c87e' -strokewidth 3.4 \
     -draw "circle $CX,$CY $CX,$((CY-R))" \
  -stroke '#c9a45c' -strokewidth 2.0 \
     -draw "circle $CX,$CY $CX,$((CY-RB))" \
  -stroke none -fill '#b98a4e' -draw "circle $CX,$CY $CX,$((CY-26))" \
  -fill 'rgba(245,223,168,0.9)' -draw "translate $((CX-110)),$((CY-110)) rotate 40 rectangle -6,-26 6,26" \
  shield.png

# 3) Typography — centered stack below the shield.
magick shield.png -gravity north \
  -font "$P052" \
  -fill '#9a8a5e' -pointsize 44 -kerning 22 -annotate +0+590 "m a r t h" \
  -fill '#eae1cb' -pointsize 128 -kerning 8 -annotate +0+648 "FOLLOWER" \
  -fill '#c9a45c' -pointsize 40 -kerning 34 -annotate +6+800 "O V E R H A U L" \
  -font "$P052I" -fill '#8d939e' -pointsize 30 -kerning 1 \
  -annotate +0+888 "programmable companions  —  gambit rules, loot runs, a party that pulls its weight" \
  logo-16x9.png
rm -f base.png shield.png
echo "wrote logo-16x9.png (${W}x${H})"
