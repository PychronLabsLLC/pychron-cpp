"""Rebuilds configs/examples/nmgrl from `elctl import-line` output.

    elctl import-line <the NMGRL valve setupfiles folder> --out <dir>
    python3 tools/rebuild_nmgrl_example.py <dir>      # from the repo root

Removes the controller endpoints from the comments, names the system, trims
the canvas's empty margins, squeezes its vertical spacing to 88% and sets
open_valve_color = "inherit". The legacy files themselves are not in the repo.
"""
import re, sys, pathlib, tomllib
src = pathlib.Path(sys.argv[1]); dst = pathlib.Path("configs/examples/nmgrl")
line = (src/"extraction_line.toml").read_text()
line, n = re.subn(r"^(# legacy \w+) at [^\n]*?: pychron-cpp", r"\1: pychron-cpp", line, flags=re.M); assert n == 5
line = line.replace('name = "valve"\n', 'name = "nmgrl_valve"\n', 1)
old = (dst/"extraction_line.toml").read_text()
(dst/"extraction_line.toml").write_text(old[:old.index("# Converted from a legacy")] + line)

text = (src/"canvas.toml").read_text()
c = tomllib.loads(text)
iv = []
for k in ("valve","manual_valve","rough_valve"):
    for e in c.get(k, []): iv.append((e["pos"][1]-15, e["pos"][1]+15))
for k in ("stage","pipette"):
    for e in c.get(k, []):
        h = e.get("size",[50,50])[1]; iv.append((e["pos"][1]-h/2, e["pos"][1]+h/2))
for e in c.get("label", []): iv.append((e["pos"][1], e["pos"][1]+16))
TOP = min(a for a,b in iv); BOTTOM = max(b for a,b in iv)
S, MARGIN = 0.88, 20
n_ = lambda v: str(int(round(v)))
out = []; sec = None
pair = re.compile(r"^(\w+) = \[(-?[\d.]+), (-?[\d.]+)\]$")
for l in text.splitlines():
    m = re.match(r"^\[+(\w+)\]+$", l)
    if m: sec = m.group(1)
    m = pair.match(l)
    if m and sec != "canvas":
        k, x, y = m.group(1), float(m.group(2)), float(m.group(3))
        if k == "pos": l = f"pos = [{n_(x)}, {n_((y - TOP) * S + MARGIN)}]"
        elif k == "size": l = f"size = [{n_(x)}, {n_(max(10, y * S))}]"
        elif k.endswith("_offset"): l = f"{k} = [{n_(x)}, {n_(y * S)}]"
    elif m and sec == "canvas" and m.group(1) == "size":
        l = f"size = [{n_(float(m.group(2)))}, {n_((BOTTOM - TOP) * S + 2 * MARGIN)}]"
    out.append(l)
t = "\n".join(out) + "\n"
old = "# world units to pixels. Check the drawing and adjust positions as needed.\n"; assert old in t
t = t.replace(old, "# world units to pixels. Then made more compact by hand: the empty margins above\n# and below trimmed and the vertical spacing squeezed to 88%, so `elctl\n# import-line` no longer reproduces this file exactly.\n")
old = "connection_width = 5\n"; assert old in t
t = t.replace(old, old + '# an open valve wears the colour of the region it joins (default: "green")\nopen_valve_color = "inherit"\n')
(dst/"canvas.toml").write_text("# Canvas of the NMGRL valve box (extraction_line.toml in this directory).\n" + t)
print("canvas size", re.search(r"size = \[.*\]", t).group(0))
