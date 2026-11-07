# Skooma: what is private, sourced by the hooks.  Installed to .git/hooks by `git skooma bootstrap`.
#
# Private work never reaches a public branch or a public remote: `local` (main plus the
# private work, the F5 build), private-* branches, spikes, amends of either, and their
# backups.  Only the remotes in skooma.privatePushUrl (by URL) may take them.
#
# One awk pass sorts every local branch, since reference-transaction runs on every ref move.

skooma_private_scan() {
    {
        git for-each-ref --format='HEAD %(refname:short)' refs/heads/
        git config --get-regexp '^branch\..*\.skooma(-base)?$' 2>/dev/null
    } | awk '
        $1 == "HEAD" { heads[++n] = $2; next }
        {
            key = $1; val = $2
            sub(/^branch\./, "", key)
            if (key ~ /\.skooma-base$/) { sub(/\.skooma-base$/, "", key); base[key] = val }
            else if (key ~ /\.skooma$/) { sub(/\.skooma$/, "", key); cls[key] = val }
        }
        function priv(b, d) {
            if (b ~ /^(local|private-.*|spike-.*|backup\/local|backup\/private-.*|backup\/spike-.*)$/) return 1
            if (cls[b] == "private" || cls[b] == "spike") return 1
            if (cls[b] == "amend" && base[b] != "" && base[b] != b && d < 8) return priv(base[b], d + 1)
            return 0
        }
        END { for (i = 1; i <= n; i++) print (priv(heads[i], 0) ? "P " : "U ") heads[i] }
    '
}

SKOOMA_SCAN=$(skooma_private_scan)

skooma_is_private() {
    case "$1" in
    local | private-* | spike-* | backup/local | backup/private-* | backup/spike-*) return 0 ;;
    esac
    printf '%s\n' "$SKOOMA_SCAN" | grep -qxF "P $1"
}

# Prints the private branch whose commits $1 would carry beyond everything public, and
# succeeds; fails when there is none.  $2 is the branch being moved (left out of the public
# set, since its new value is what is judged); the rest are more public revisions or
# rev-list options, such as the branch's old value or --remotes=<remote>.
skooma_private_leak() {
    _tip=$1; _self=$2; shift 2
    # local last, so a leak names the private branch it came from where there is one.
    _priv=$(printf '%s\n' "$SKOOMA_SCAN" | sed -n 's/^P //p' | grep -vxF "$_self" | grep -vx local)
    printf '%s\n' "$SKOOMA_SCAN" | grep -qx 'P local' && [ "$_self" != local ] && _priv="$_priv local"
    [ -n "$_priv" ] || return 1
    _pub=$(printf '%s\n' "$SKOOMA_SCAN" | sed -n 's/^U //p' | grep -vxF "$_self" | sed 's|^|refs/heads/|')
    # History already declared public (main as it was when local began), if Nexii set it.
    _base=$(git config --get skooma.publicBase 2>/dev/null)
    [ -n "$_base" ] && _pub="$_pub $_base"
    for _p in $_priv; do
        for _mb in $(git merge-base --all "refs/heads/$_p" "$_tip" 2>/dev/null); do
            # A commit both share that no public branch holds came from the private one.
            if [ -n "$(git rev-list -n 1 "$_mb" --not $_pub "$@" 2>/dev/null)" ]; then
                echo "$_p"
                return 0
            fi
        done
    done
    return 1
}
