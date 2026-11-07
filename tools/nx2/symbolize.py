# symbolize.py <exe> <samples.txt.raw> [--out <dir>] [--top N]
#
# Names the "lichii.exe+0x..." frames the sampler writes to <out>.raw (EDEN_NX2_SAMPLE or the
# debug server's `sample`) with llvm-symbolizer, which reads the PDB of a MinGW build that dbghelp
# can't (compile with -gcodeview -gline-tables-only, link with -Wl,--pdb=). Prints self and
# inclusive counts per thread and function, and writes <raw>.sym.folded for flame graphs.
# Native Windows Python; llvm-symbolizer from MSYS2's mingw-w64-ucrt-x86_64-llvm-tools.
import argparse, collections, os, re, shutil, subprocess, sys

SYMBOLIZER = shutil.which("llvm-symbolizer") or r"C:\msys64\ucrt64\bin\llvm-symbolizer.exe"


def short(name):
    """Drops template and parameter lists so one function aggregates as one name."""
    out, depth = [], 0
    for c in name:
        if c in "<(":
            depth += 1
        elif c in ">)" and depth:
            depth -= 1
        elif depth == 0:
            out.append(c)
    return re.sub(r"\s+", " ", "".join(out)).strip()[:140]


def symbolize(exe, offsets):
    """offset -> name (innermost inlined function at the address)."""
    if not offsets:
        return {}
    query = "\n".join(f"{o:#x}" for o in offsets) + "\n"
    r = subprocess.run([SYMBOLIZER, f"--obj={exe}", "--relative-address", "--inlines",
                        "--output-style=JSON", "--demangle"],
                       input=query, capture_output=True, text=True, check=True)
    import json
    names = {}
    for offset, line in zip(offsets, r.stdout.splitlines()):
        try:
            frames = json.loads(line).get("Symbol", [])
        except json.JSONDecodeError:
            frames = []
        fn = frames[0].get("FunctionName", "") if frames else ""
        names[offset] = short(fn) if fn else f"?+{offset:#x}"
    return names


def main():
    p = argparse.ArgumentParser()
    p.add_argument("exe")
    p.add_argument("raw")
    p.add_argument("--top", type=int, default=40)
    a = p.parse_args()
    module = os.path.basename(a.exe).lower()
    stacks = []
    offsets = set()
    for line in open(a.raw, encoding="utf-8"):
        body, _, count = line.rstrip("\n").rpartition(" ")
        thread, *frames = body.split(";")
        parsed = []
        # Frames are root first; all but the last (the leaf) are return addresses.
        for i, f in enumerate(frames):
            m = re.fullmatch(r"(.+)\+0x([0-9a-f]+)", f)
            if m and m.group(1).lower() == module:
                off = int(m.group(2), 16) - (0 if i == len(frames) - 1 else 1)
                offsets.add(off)
                parsed.append(off)
            else:
                parsed.append(f.split("+")[0] if "+" in f else f)
        stacks.append((thread, parsed, int(count)))
    names = symbolize(a.exe, sorted(offsets))
    name = lambda f: names[f] if isinstance(f, int) else f

    per_thread = collections.defaultdict(lambda: (collections.Counter(), collections.Counter(), [0]))
    folded = collections.Counter()
    for thread, frames, count in stacks:
        self_c, incl_c, total = per_thread[thread]
        total[0] += count
        named = [name(f) for f in frames]
        if named:
            self_c[named[-1]] += count
        for n in set(named):
            incl_c[n] += count
        folded[";".join([thread] + named)] += count
    for thread in sorted(per_thread):
        self_c, incl_c, total = per_thread[thread]
        t = max(total[0], 1)
        print(f"=== {thread}: {total[0]} samples")
        print("-- self")
        for n, c in self_c.most_common(a.top):
            print(f"{c:7} {100 * c / t:5.1f}% {n}")
        print("-- inclusive")
        for n, c in incl_c.most_common(a.top + 20):
            print(f"{c:7} {100 * c / t:5.1f}% {n}")
        print()
    with open(a.raw + ".sym.folded", "w", encoding="utf-8") as f:
        for k, c in folded.items():
            f.write(f"{k} {c}\n")


if __name__ == "__main__":
    main()
