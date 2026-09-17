#!/usr/bin/env bash
# tools/deploy.sh — put the current build in front of the user's hands.
#
# teidraw has TWO system-wide installs, and both must track the repo or the
# user silently runs an old binary:
#
#   Linux    /usr/local/bin/teidraw
#            launched by the `teidraw` command / the niri Mod+Shift+T bind.
#
#   Windows  <wslop>:/opt/src/teidraw/build/teidraw.exe
#            the Start Menu shortcut runs it straight out of the wslop
#            checkout over \\wsl.localhost\NixOS\opt\src\teidraw\build\ ,
#            so the Windows exe must be BUILT INSIDE that checkout (a copy
#            from the dev box is not the same file and would rot).
#
# A deploy is therefore: sync the repo to wslop, build both targets, install
# the Linux binary, and verify both with a throwaway render. Run it after
# anything that changes editor/ or assets/ — and commit first, because only
# committed history travels to wslop.
#
#   tools/deploy.sh              # deploy both
#   tools/deploy.sh --linux      # Linux install only
#   tools/deploy.sh --win        # Windows (wslop) only
#   tools/deploy.sh --check      # report drift, change nothing
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WSLOP_HOST="${WSLOP_HOST:-wslop}"
WSLOP_REPO="${WSLOP_REPO:-/opt/src/teidraw}"
LINUX_INSTALL="${LINUX_INSTALL:-/usr/local/bin/teidraw}"
NIX="${NIX:-nix develop --command}"

do_linux=auto; do_win=auto; check_only=false
for arg in "$@"; do
    case "$arg" in
        --linux) do_linux=yes; do_win=no ;;
        --win)   do_win=yes; do_linux=no ;;
        --check) check_only=true ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "deploy.sh: unknown argument '$arg'" >&2; exit 2 ;;
    esac
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

# A tiny board used only to prove a fresh binary renders. Written under
# /tmp (Linux) or %TEMP% (Windows) — never into the user's board folders.
make_check_board() {
    local dir="$1"
    mkdir -p "$dir"
    cat > "$dir/board.json" <<'JSON'
{"v":2,"nextId":2,"cam":{"x":0,"y":0,"z":1},"shapes":[
 {"id":1,"type":0,"text":"deploy check","family":0,"tsize":1,"x":40,"y":40}]}
JSON
}

# ── Linux ─────────────────────────────────────────────────────────────────
deploy_linux() {
    local built="$REPO/build/teidraw"
    if $check_only && [[ -x "$built" ]]; then
        :   # --check is read-only: compare what is already built
    else
        say "Linux: building (make -C editor linux)"
        (cd "$REPO" && $NIX make -C editor linux -j"$(nproc)" >/dev/null)
    fi
    [[ -x "$built" ]] || { echo "deploy.sh: $built missing after build" >&2; exit 1; }

    local want have
    want="$(md5sum "$built" | cut -d' ' -f1)"
    have="$(md5sum "$LINUX_INSTALL" 2>/dev/null | cut -d' ' -f1 || echo none)"

    if $check_only; then
        [[ "$want" == "$have" ]] && say "Linux: up to date" || \
            say "Linux: DRIFT — installed $(short "$have") vs built $(short "$want")"
        return
    fi
    if [[ "$want" == "$have" ]]; then
        say "Linux: already current ($(short "$have"))"
    else
        sudo -n true 2>/dev/null || { echo "deploy.sh: need passwordless sudo to install $LINUX_INSTALL" >&2; exit 1; }
        [[ -f "$LINUX_INSTALL" ]] && sudo cp -a "$LINUX_INSTALL" "$LINUX_INSTALL.bak-$(date +%Y%m%d%H%M)"
        sudo install -m 755 "$built" "$LINUX_INSTALL"
        say "Linux: installed $LINUX_INSTALL ($(short "$have") -> $(short "$want"))"
    fi

    local probe; probe="$(mktemp -d)"
    make_check_board "$probe/board"
    if "$LINUX_INSTALL" "$probe/board" --shot "$probe/shot.png" --frames 6 >/dev/null 2>&1 \
       && [[ -s "$probe/shot.png" ]]; then
        say "Linux: verified (headless render ok)"
    else
        echo "deploy.sh: Linux verify FAILED" >&2; exit 1
    fi
    rm -rf "$probe"
}

# ── Windows (built in the wslop checkout the shortcut points at) ──────────
deploy_win() {
    if $check_only; then   # read-only: report, touch nothing
        local lhead rhead rexe
        lhead="$(git -C "$REPO" rev-parse --short HEAD)"
        rhead="$(ssh "$WSLOP_HOST" "cd '$WSLOP_REPO' && git rev-parse --short HEAD")"
        rexe="$(ssh "$WSLOP_HOST" "stat -c '%y' '$WSLOP_REPO/build/teidraw.exe' 2>/dev/null | cut -d. -f1 || echo missing")"
        if [[ "$lhead" == "$rhead" ]]; then
            say "Windows: wslop at $rhead (current), exe built $rexe"
        else
            say "Windows: DRIFT — local $lhead vs wslop $rhead (exe built $rexe)"
        fi
        return
    fi

    say "Windows: syncing repo to $WSLOP_HOST:$WSLOP_REPO"
    local dirty
    dirty="$(git -C "$REPO" status --porcelain | head -1 || true)"
    [[ -n "$dirty" ]] && echo "deploy.sh: note — uncommitted changes stay behind (only committed history travels)"

    local bundle; bundle="$(mktemp /tmp/teidraw-sync-XXXX.bundle)"
    git -C "$REPO" bundle create "$bundle" HEAD >/dev/null
    scp -q "$bundle" "$WSLOP_HOST:/tmp/teidraw-sync.bundle"
    rm -f "$bundle"

    # third_party/ is gitignored (vendored imgui for non-nix builds) but the
    # wslop tree may carry it as real files: keep them across the hard reset.
    ssh "$WSLOP_HOST" "cd '$WSLOP_REPO' && \
        tar czf /tmp/teidraw-thirdparty.tgz third_party 2>/dev/null || true; \
        git fetch /tmp/teidraw-sync.bundle HEAD:refs/heads/deploy-incoming >/dev/null && \
        git reset --hard deploy-incoming >/dev/null && \
        tar xzf /tmp/teidraw-thirdparty.tgz 2>/dev/null || true; \
        git log --oneline -1"

    say "Windows: building the exe inside the wslop checkout (that path IS the install)"
    ssh "$WSLOP_HOST" "cd '$WSLOP_REPO' && $NIX make -C editor -j4 >/dev/null && ls -la build/teidraw.exe | awk '{print \$5, \$6, \$7, \$8}'"

    say "Windows: verifying on the host"
    # the check board is pushed from here so no shell quoting games are needed
    local win_tmp="/mnt/c/Users/headpats/AppData/Local/Temp/teidraw-deploy"
    ssh "$WSLOP_HOST" "mkdir -p '$win_tmp'"
    local probe_dir; probe_dir="$(mktemp -d)"
    make_check_board "$probe_dir/board"
    scp -q "$probe_dir/board/board.json" "$WSLOP_HOST:$win_tmp/board.json"
    rm -rf "$probe_dir"
    ssh "$WSLOP_HOST" "cd '$WSLOP_REPO/build' && ./teidraw.exe 'C:\\Users\\headpats\\AppData\\Local\\Temp\\teidraw-deploy' --shot 'C:\\Users\\headpats\\AppData\\Local\\Temp\\teidraw-deploy.png' --frames 6 2>&1 | tail -1"
}

short() { echo "${1:0:8}"; }

if [[ "$do_linux" != no ]]; then deploy_linux; fi
if [[ "$do_win" != no ]]; then deploy_win; fi
say "done"
