#!/usr/bin/env bash
# Nexus banner for marth Follower Overhaul (1300x372).
# Motif: a round companion's shield with a glowing ember-crimson ring — the
# follower at your side, holding the line while the party works. House style
# matches the sibling overhauls (dark ground, gold type, P052): only the motif
# + accent (amethyst/essence green -> ember crimson) and the copy differ.
set -euo pipefail
cd "$(dirname "$0")"
W=1300; H=372
P052=/usr/share/fonts/opentype/urw-base35/P052-Roman.otf
P052I=/usr/share/fonts/opentype/urw-base35/P052-Italic.otf

# Round shield (face-on), sat on the right: outer rim, inner ring, center boss.
CX=1092; CY=196; R=98            # face: centre + radius
RI=64                            # inner ring radius
RB=26                            # boss radius

# 1) Base: near-black vertical gradient + soft ember radial glow.
magick -size ${W}x${H} gradient:'#16181d-#08090b' \
  \( -size ${W}x${H} radial-gradient:'#2b1a16-#000000' -evaluate multiply 0.9 \) \
  -compose screen -composite base.png

# 2) The shield: a blurred crimson glow underlayer, translucent face, a softly
#    glowing inner ring, four gold strap rivets, then a crisp gold rim + boss
#    and a bright glint.
magick base.png \
  \( -clone 0 -fill none -stroke '#e86a52' -strokewidth 11 \
     -draw "circle $CX,$CY $CX,$((CY-R))" -blur 0x9 \) \
  -compose screen -composite \
  -stroke none -fill 'rgba(190,90,66,0.16)' \
  -draw "circle $CX,$CY $CX,$((CY-R))" \
  -fill 'rgba(232,106,82,0.30)' -draw "circle $CX,$CY $CX,$((CY-RI))" \
  -stroke '#f0937e' -strokewidth 2.0 -fill none \
  -draw "circle $CX,$CY $CX,$((CY-RI))" \
  -stroke none -fill 'rgba(245,223,168,0.85)' \
     -draw "circle $((CX-45)),$((CY-45)) $((CX-45)),$((CY-41))" \
     -draw "circle $((CX+45)),$((CY-45)) $((CX+45)),$((CY-41))" \
     -draw "circle $((CX-45)),$((CY+45)) $((CX-45)),$((CY+41))" \
     -draw "circle $((CX+45)),$((CY+45)) $((CX+45)),$((CY+41))" \
  -fill none -stroke '#e8c87e' -strokewidth 2.6 \
     -draw "circle $CX,$CY $CX,$((CY-R))" \
  -stroke '#c9a45c' -strokewidth 1.6 \
     -draw "circle $CX,$CY $CX,$((CY-RB))" \
  -stroke none -fill '#b98a4e' -draw "circle $CX,$CY $CX,$((CY-14))" \
  -fill 'rgba(245,223,168,0.9)' -draw "translate $((CX-58)),$((CY-58)) rotate 40 rectangle -4,-16 4,16" \
  shield.png

# 3) Typography (left-aligned so it clears the shield).
magick shield.png \
  -font "$P052" -gravity northwest \
  -fill '#9a8a5e' -pointsize 30 -kerning 14 -annotate +72+58 "m a r t h" \
  -fill '#eae1cb' -pointsize 84 -kerning 5 -annotate +68+92 "FOLLOWER" \
  -fill '#c9a45c' -pointsize 26 -kerning 21 -annotate +76+206 "O V E R H A U L" \
  -font "$P052I" -fill '#8d939e' -pointsize 22 -kerning 1 \
  -annotate +74+270 "programmable companions  —  gambit rules, loot runs, a party that pulls its weight" \
  nexus-banner.png
rm -f base.png shield.png
echo "wrote nexus-banner.png (${W}x${H})"
