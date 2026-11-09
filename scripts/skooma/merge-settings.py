"""Merge driver for the viewer's settings files: merges by setting, not by line.

git config merge.sksettings.driver 'python -I <this> %O %A %B %P'   (set by git skooma bootstrap)

Each branch adds whole <key>Name</key><map>...</map> blocks, and git's line merge aligns
neighbouring insertions badly: every integrate conflicted, and resolving by hand lost settings.
Here a setting one side added is added, one side changed is changed, one side removed is removed,
and only a setting both sides changed differently is a conflict (both kept between markers).
Anything that does not parse as a flat list of settings falls back to git merge-file.
"""
import re
import subprocess
import sys

BLOCK = re.compile(r"(?P<lead>\n(?P<ind>[ \t]*)<key>(?P<name>[A-Za-z0-9_.\-]+)</key>\r?\n(?P=ind)<map>.*?\n(?P=ind)</map>)", re.S)


def parse(text):
    """(head, [(key, text)], tail). What lies between two settings (a marker comment) goes with the
    one after it; a setting listed twice is keyed by its count, so it still merges."""
    blocks, pos, head, seen = [], 0, None, {}
    for m in BLOCK.finditer(text):
        gap = text[pos:m.start()]
        if head is None:
            head, gap = gap, ""
        name = m.group("name")
        seen[name] = seen.get(name, 0) + 1
        key = name if seen[name] == 1 else "%s#%d" % (name, seen[name])
        blocks.append((key, gap + m.group("lead")))
        pos = m.end()
    if not blocks:
        return None
    return head, blocks, text[pos:]


def main():
    base_p, ours_p, theirs_p, name = sys.argv[1:5]
    read = lambda p: open(p, encoding="utf-8", newline="").read()
    base, ours, theirs = parse(read(base_p)), parse(read(ours_p)), parse(read(theirs_p))
    if not (base and ours and theirs) or ours[0] != theirs[0] or ours[2] != theirs[2]:
        return subprocess.call(["git", "merge-file", "-L", "ours", "-L", "base", "-L", "theirs", ours_p, base_p, theirs_p])

    b, o, t = dict(base[1]), dict(ours[1]), dict(theirs[1])
    conflicts = 0
    out = []
    for key, text in ours[1]:
        if key in t:
            mine, theirs_text, was = text, t[key], b.get(key)
            if mine == theirs_text or theirs_text == was:
                out.append((key, mine))
            elif mine == was:
                out.append((key, theirs_text))
            else:
                conflicts += 1
                out.append((key, "\n<<<<<<< ours" + mine + "\n=======" + theirs_text + "\n>>>>>>> theirs"))
        elif key in b and text != b[key]:
            conflicts += 1     # changed here, removed there
            out.append((key, "\n<<<<<<< ours (theirs removed it)" + text + "\n>>>>>>> theirs"))
        elif key not in b:
            out.append((key, text))     # added here
        # else: unchanged here and removed there: removed
    # settings added on their side go after the setting before them there that the result has
    order = [k for k, _ in theirs[1]]
    for i, (key, text) in enumerate(theirs[1]):
        if key in o or key in b:
            continue
        have = [k for k, _ in out]
        after = next((p for p in reversed(order[:i]) if p in have), None)
        at = have.index(after) + 1 if after else 0
        out.insert(at, (key, text))
    with open(ours_p, "w", encoding="utf-8", newline="") as f:
        f.write(ours[0] + "".join(text for _, text in out) + ours[2])
    if conflicts:
        sys.stderr.write("sksettings: %d setting(s) changed on both sides in %s\n" % (conflicts, name))
    return 1 if conflicts else 0


if __name__ == "__main__":
    sys.exit(main())
