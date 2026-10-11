#!/usr/bin/env bash
# Copyright 2026 KAPPER NETWORK-COMMUNICATIONS GmbH <https://github.com/hknet/KiTTY>
# Released under the MIT licence, as KiTTY++ (see LICENCE in the repository).
#
# kittypp-demo.sh - the new terminal features of KiTTY++ 0.85.1.15, one step
# at a time, to look at by hand. NOT a test with a verdict: each step shows
# what you should see, Enter goes on, q quits (and cleans up).
#
# Run it on a Linux host, in a KiTTY++ 0.85.1.15 (or newer) window, with the
# window at least 80 x 24:
#     bash kittypp-demo.sh            all steps
#     bash kittypp-demo.sh 5          from step 5
# Needs bash 3+, awk, base64 (coreutils); any locale (the placeholders go as
# UTF-8 bytes, since bash before 4.2 has no \u). Nothing is installed or
# written. The KiTTY++ session's character set must be UTF-8.
#
# Steps:
#   1 underline styles and colours (SGR 4:n, 58, 21)
#   2 cell size answers (iTerm2 ReportCellSize, CSI 16 t)
#   3 a picture on the bottom row (the screen scrolls), then on a scroll
#     region's bottom margin (the region scrolls)
#   4 kitty relative placements (P/Q/H/V)
#   5 kitty animation (a=f, a=a, d=f)
#   6 an animated GIF (iTerm2 OSC 1337)
#   7 kitty Unicode placeholders (U=1)
#   8 program status (OSC 7501): title bar, overlay, notice
#
set -u
START=${1:-1}

# ---- helpers ----------------------------------------------------------------
cleanup() {
    printf '\e[r\e[0m'                  # region and attributes back
    printf '\e_Ga=d,d=A,q=2\e\\'        # every kitty picture, and its image
    printf '\e]7501;state=clear\e\\'    # every program status record
    printf '\e]133;A\e\\'               # a prompt: running jobs end
    printf '\e[?25h'
}
trap 'cleanup; stty sane 2>/dev/null; echo; exit 0' INT

pause() {
    printf '\n\e[2m-- Enter: next, q + Enter: quit --\e[0m '
    read -r k
    if [ "$k" = q ]; then cleanup; printf '\e[2J\e[H'; exit 0; fi
}
size() {                                # ROWS / COLS of the window, now
    local s
    s=$(stty size 2>/dev/null)
    ROWS=${s% *}; COLS=${s#* }
    [ -n "$ROWS" ] && [ "$ROWS" -gt 0 ] 2>/dev/null || ROWS=$(tput lines 2>/dev/null || echo 24)
    [ -n "$COLS" ] && [ "$COLS" -gt 0 ] 2>/dev/null || COLS=$(tput cols 2>/dev/null || echo 80)
}
step() {                                # step number, title
    size                                # read again: the window may have changed
    printf '\e[0m\e[2J\e[3J\e[H\e[1;7m %s. %s \e[0m  \e[2m(%dx%d)\e[0m\n\n' "$1" "$2" "$COLS" "$ROWS"
}
note() { printf '\e[36m%s\e[0m\n' "$*"; }

# One colour, w x h raw RGB, base64 (components 1..255: awk cannot print 0).
solid() {
    LC_ALL=C awk -v n=$(($1 * $2)) -v r=$3 -v g=$4 -v b=$5 \
        'BEGIN { for (i = 0; i < n; i++) printf "%c%c%c", r, g, b }' | base64 -w0
}
# A gradient, red across, green down: shows which part lands where.
grad() {
    LC_ALL=C awk -v w=$1 -v h=$2 'BEGIN {
        for (y = 0; y < h; y++) for (x = 0; x < w; x++)
            printf "%c%c%c", 1 + int(254 * x / w), 1 + int(254 * y / h), 128 }' | base64 -w0
}
# kitty graphics: control keys, then the base64 payload in chunks of 4096.
kg() {
    local ctl=$1 data=${2:-} len i=0 first=1 m chunk
    if [ -z "$data" ]; then printf '\e_G%s\e\\' "$ctl"; return; fi
    len=${#data}
    while [ $i -lt $len ]; do
        chunk=${data:i:4096}; i=$((i + 4096))
        if [ $i -lt $len ]; then m=1; else m=0; fi
        if [ $first = 1 ]; then printf '\e_G%s,m=%s;%s\e\\' "$ctl" "$m" "$chunk"; first=0
        else printf '\e_Gm=%s;%s\e\\' "$m" "$chunk"; fi
    done
}
ps() { printf '\e]7501;%s\e\\' "$1"; }           # program status report
b64() { printf %s "$1" | base64 -w0; }
at() { printf '\e[%d;%dH' "$1" "$2"; }           # row, column (1-based)

size
printf 'Window: %d columns x %d rows.\n' "$COLS" "$ROWS"
if [ "$COLS" -lt 80 ] || [ "$ROWS" -lt 24 ]; then
    printf 'At least 80 x 24 is needed - make the window larger and start again.\n'
    exit 1
fi

BLUE=$(solid 40 20 40 90 230)
YELLOW=$(solid 40 20 240 200 30)
GREEN=$(solid 40 20 60 180 90)

# ---- 1 ----------------------------------------------------------------------
if [ "$START" -le 1 ]; then
step 1 "Underline styles and colours"
printf '  \e[4mplain (SGR 4)\e[0m   \e[4:2mdouble (4:2)\e[0m   \e[21mdouble (21)\e[0m\n\n'
printf '  \e[4:3mcurly (4:3)\e[0m   \e[4:4mdotted (4:4)\e[0m   \e[4:5mdashed (4:5)\e[0m\n\n'
printf '  \e[4:3;58:2::255:60:60mred curly (58:2::r:g:b)\e[0m   '
printf '\e[4:1;58;5;45mcyan line (58;5;45)\e[0m   '
printf '\e[4:4;58:5:208;38;5;250morange dots, grey text\e[0m\n\n'
printf '  \e[4:3;58;2;80;200;80mgreen curly\e[59m back to the text colour (59)\e[0m\n'
note ""
note "See: five different line shapes; the coloured ones in their own colour,"
note "the text keeping its colour. Same in GDI and Direct2D (Application >"
note "KiTTY++ Settings > Terminal > Renderer, for new windows)."
pause
fi

# ---- 2 ----------------------------------------------------------------------
if [ "$START" -le 2 ]; then
step 2 "Cell size answers"
old=$(stty -g); stty -icanon -echo min 0 time 20
printf '\e]1337;ReportCellSize\a'
rep=$(dd bs=1 count=64 2>/dev/null | tr -d '\a' | sed 's/.*ReportCellSize=//')
printf '\e[16t'
rep16=$(dd bs=1 count=32 2>/dev/null | sed 's/.*\[6;//; s/t.*//')
stty "$old"
printf '  OSC 1337 ReportCellSize: height;width;scale = %s\n' "$rep"
printf '  CSI 16 t:                height;width px   = %s\n' "$rep16"
note ""
note "See: points x scale = pixels (e.g. 8.25 pt x 2.00 = 16.5 ~ 17 px at 200 %)."
note "Move the window to a monitor with another scaling and run step 2 again."
pause
fi

# ---- 3 ----------------------------------------------------------------------
if [ "$START" -le 3 ]; then
step 3 "A picture on the bottom row, then on a region's bottom margin"
rows=$ROWS
note "3a. Lines down to the window's last row; a picture 3 rows tall is then"
note "placed beside the LAST line, where 2 of its rows have no room."
note "See: the screen scrolled up 2, the picture's top level with \"last line\","
note "the whole picture on the bottom 3 rows (nothing cut off), the cursor on"
note "the last row right of it. Before, it hung below the window."
for ((r = 8; r < rows; r++)); do at $r 1; printf 'line %d' $r; done
at $rows 1; printf 'last line'
at $rows 30; kg "a=T,f=24,s=40,v=20,i=12,c=10,r=3,q=2" "$YELLOW"
printf ' \e[2m<- the cursor; Enter: next, q: quit\e[0m '
read -r k
if [ "$k" = q ]; then cleanup; printf '\e[2J\e[H'; exit 0; fi
kg "a=d,d=I,i=12,q=2"
printf '\e[0m\e[2J\e[3J\e[H\e[1;7m 3b. A picture on a scroll region'"'"'s bottom margin \e[0m\n\n'
n=0
for l in A B C D E F G H I; do at $((4 + n)) 1; printf '> line %s' $l; n=$((n + 1)); done
printf '\e[4;12r'
at 12 30; kg "a=T,f=24,s=40,v=20,i=11,c=10,r=3,q=2" "$BLUE"
printf '\e[r'; at 15 1
note "A scroll region of 9 lines (A-I, marked >). A picture 3 lines tall was"
note "placed beside line I, the region's last line: its own line feeds scroll"
note "the region up 2."
note "See: lines A and B gone, I two lines higher, the blue picture's top"
note "level with line I and whole below it (the two empty lines are the ones"
note "the region scrolled in). Before 0.85.1.15 it stayed where I had been and"
note "hung below the region."
pause
kg "a=d,d=I,i=11,q=2"
fi

# ---- 4 ----------------------------------------------------------------------
if [ "$START" -le 4 ]; then
step 4 "kitty relative placements"
at 4 5; kg "a=T,f=24,s=40,v=20,i=21,p=1,c=8,r=3,C=1,q=2" "$BLUE"
at 20 60; kg "a=T,f=24,s=40,v=20,i=22,p=1,c=4,r=1,P=21,Q=1,H=10,V=1,q=2" "$YELLOW"
at 9 1
note "Blue: the parent at row 4, column 5. Yellow: a child, sent while the"
note "cursor stood at row 20 column 60, anchored 10 cells right, 1 down."
note "See: yellow beside blue; the cursor not moved by the child."
pause
at 14 30; kg "a=p,i=21,p=1,c=8,r=3,C=1,q=2"
at 18 1
note "The parent placed again at row 14, column 30 (the child: row 15, column 40)."
note "See: the child moved with it; nothing left at the old places."
pause
kg "a=d,d=i,i=21,p=1,q=2"
at 18 1; printf '\e[J'
note "The parent deleted. See: the child is gone with it."
pause
kg "a=d,d=A,q=2"
fi

# ---- 5 ----------------------------------------------------------------------
if [ "$START" -le 5 ]; then
step 5 "kitty animation"
at 4 5; kg "a=T,f=24,s=40,v=20,i=31,c=12,r=4,C=1,q=2" "$BLUE"
kg "a=f,f=24,s=40,v=20,i=31,z=400,q=2" "$YELLOW"
kg "a=f,f=24,s=40,v=20,i=31,z=400,q=2" "$GREEN"
kg "a=a,i=31,r=1,z=400,q=2"
kg "a=a,i=31,s=3,q=2"
at 10 1
note "Three frames (blue, yellow, green), 400 ms each, played by the terminal,"
note "looping. Minimise the window for a moment: it waits, it does not draw."
pause
kg "a=a,i=31,s=1,c=2,q=2"
at 10 1; printf '\e[J'
note "Stopped and set to frame 2. See: yellow, standing still."
pause
kg "a=d,d=f,i=31,r=2,q=2"
at 10 1; printf '\e[J'
note "Frame 2 deleted. See: the picture shows another frame (green)."
pause
kg "a=d,d=A,q=2"
fi

# ---- 6 ----------------------------------------------------------------------
if [ "$START" -le 6 ]; then
step 6 "An animated GIF (iTerm2 OSC 1337)"
GIF='R0lGODlhIAAQAPEAAB5a5vDIHjy0WgAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQEKAAAACwAAAAAIAAQAAAC/wQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECIgQIAAAQIECBAgQIAAAQIECBAgQIAAAQIECBAgQIAAAQIsACH5BAQoAAAALAAAAAAgABAAAAL/TJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwYcKECRMmTJgwImHChAkTJkyYMGHChAkTJkyYMGHChAkTJkyYMGHChAkTJiwAIfkEBCgAAAAsAAAAACAAEAAAAv+UKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFGiRIkSJUqUKFEiokSJEiVKlChRokSJEiVKlChRokSJEiVKlChRokSJEiVKLAA7'
at 4 5
printf '\e]1337;File=inline=1;width=12;height=4;preserveAspectRatio=0:%s\a' "$GIF"
at 10 1
note "A GIF of three frames (blue, yellow, green), 400 ms each, as imgcat sends"
note "it. See: it plays, looping. Before 0.85.1.15 only blue showed."
note "With imgcat installed: imgcat any-animated.gif"
pause
printf '\e[2J'
fi

# ---- 7 ----------------------------------------------------------------------
if [ "$START" -le 7 ]; then
step 7 "kitty Unicode placeholders"
kg "a=T,f=24,s=80,v=40,i=42,U=1,c=8,r=4,q=2" "$(grad 80 40)"
# UTF-8 bytes, not $'\U...': bash leaves \U/\u unconverted in a non-UTF-8
# locale, and the host's locale is not ours to rely on
PH=$'\xf4\x8e\xbb\xae'                                  # U+10EEEE
DIA=($'\xcc\x85' $'\xcc\x8d' $'\xcc\x8e' $'\xcc\x90')   # U+0305 030D 030E 0310 = 0..3
note "A virtual placement of 8 x 4 cells, image id 42 (= foreground colour 42),"
note "and its placeholders printed as TEXT, one row per line:"
echo
for r in 0 1 2 3; do
    printf '    \e[38;5;42m%s%s' "$PH" "${DIA[r]}"
    for c in 1 2 3 4 5 6 7; do printf '%s' "$PH"; done
    printf '\e[0m   <- text row %d\n' "$r"
done
echo
note "See: a gradient (red across, green down) on those cells, no strange"
note "glyphs. The picture is text: it scrolls with it."
pause
# from the window's last row, so that every line scrolls the screen
at "$ROWS" 1
for i in 1 2 3 4 5; do printf '\n  scrolling line %d' "$i"; done
printf '\n'
note "Lines came in at the window's bottom: the screen scrolled."
note "See: the picture went up together with its text rows, still whole and"
note "still level with them. Inside tmux it works the same (tmux passes the"
note "placeholders as plain text)."
pause
kg "a=d,d=I,i=42,q=2"
note "Picture deleted. See: the placeholder cells are blank now."
pause
fi

# ---- 8 ----------------------------------------------------------------------
if [ "$START" -le 8 ]; then
step 8 "Program status (OSC 7501)"
note "8a. The title bar: a job at 10% .. 90%."
for p in 10 30 50 70 90; do ps "state=working:id=build:app=make:progress=$p"; sleep 0.6; done
note "See: the window title ends with (90%); the taskbar button shows a bar."
note "(Application > KiTTY++ Settings > Terminal > Show status information in"
note "the title bar must be ticked - it is by default.)"
pause
note "8b. The overlay: two or more jobs."
ps "state=working:id=build/test:app=ctest:msg=$(b64 'running 14 of 31')"
ps "state=blocked:id=deploy:app=pi:kind=question:msg=$(b64 'Push to staging?')"
ps "state=done:id=lint:app=clang-tidy:msg=$(b64 'no findings')"
note "See: a panel top right in the window - deploy waiting (amber), build 90%,"
note "build > test working (a moving bar), lint done (faded). Drag it by its"
note "header; its x hides it until a new job starts. It never takes the keys:"
note "type something now, it goes to the terminal."
pause
note "8c. The notice: switch to ANOTHER window within 8 seconds."
for s in 8 7 6 5 4 3 2 1; do printf '\r  %d ' $s; sleep 1; done; echo
ps "state=blocked:id=review:app=demo:kind=permission:msg=$(b64 'May I delete the old logs?')"
note "See (while this window is in the background): a notice near the clock,"
note "\"demo: Waiting for permission\", with a \"Go to the window\" button; the"
note "taskbar button flashes. With the title-bar status off, the first such"
note "notice also offers \"Show it in the title\" (once per installation)."
note "The Event Log (the window menu, Alt+Space > Event Log) has a line for each."
pause
printf '\e]133;A\e\\'
ps "state=clear"
note "Cleared. See: the overlay, the title marker and the taskbar state gone."
pause
fi

cleanup
printf '\e[2J\e[H'
echo "Done. Start a step again with:  bash $0 <step>"
