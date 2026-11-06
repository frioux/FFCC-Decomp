#!/usr/bin/env python3
"""Serial, COW-cloned, headless-pi agent loop for FFCC-Decomp.

One iteration:
  1. refresh `base/` to origin/staging and build it (warm build dir for cheap clones)
  2. pick the next unit target from build/GCCP01/report.json (project selector rules)
  3. `cp -ac base clone-<unit>` (APFS clonefile: fast, block-shared) + fresh branch
  4. run ONE headless `pi -p` agent inside the clone (fresh context, serial)
  5. gate on report.json: unit fuzzy must improve and `ninja` must pass
  6. publish a fork branch + PR against `staging` only when the gate passes

Usage:
  python3 tools/pi_loop.py --dry-run            # show next target + prompt, do nothing
  python3 tools/pi_loop.py --iterations 3       # run 3 serial iterations
  python3 tools/pi_loop.py --unit main/mes --minutes 30 --no-push
"""

import argparse
import json
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
UPSTREAM = "zcanann/FFCC-Decomp"
FORK_OWNER = "frioux"
UNIT = "GCCP01"

# WORK_SPLIT.md "CONFIRMED DEAD-ENDS": exhausted by earlier campaigns.
DEAD_END_UNITS = {
    "main/bonus_menu",
    "main/wm_menu",
    "main/menu_cmd",
    "main/MenuUtil",
    "main/shopmenu",
    "main/singmenu",
}

MAX_ATTEMPTS = 3


def sh(cmd, cwd, timeout=None, check=False, log=None):
    """Run a command, tee its output to `log` when given, return CompletedProcess."""
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        timeout=timeout,
        capture_output=True,
        text=True,
    )
    if log:
        with open(log, "a") as f:
            f.write(f"$ {' '.join(shlex.quote(c) for c in cmd)}\n")
            f.write(proc.stdout or "")
            f.write(proc.stderr or "")
            f.write(f"[exit {proc.returncode}]\n")
    if check and proc.returncode != 0:
        sys.stderr.write((proc.stdout or "")[-4000:] + (proc.stderr or "")[-4000:])
        raise SystemExit(f"command failed ({proc.returncode}): {' '.join(cmd)}")
    return proc


def git(cwd, *args, **kw):
    return sh(["git", *args], cwd, **kw)


# ---------------------------------------------------------------- base + clones


def configure_args():
    """Reuse this machine's configure flags (e.g. --gba-compilers) from build.ninja."""
    try:
        text = (REPO / "build.ninja").read_text()
    except OSError:
        return ["--version", UNIT]
    m = re.search(r"^configure_args = (.*)$", text, re.MULTILINE)
    return m.group(1).split() if m else ["--version", UNIT]


def refresh_base(work, log):
    base = work / "base"
    if not base.exists():
        sh(["cp", "-ac", str(REPO), str(base)], work, check=True, log=log)
    git(base, "fetch", "origin", "staging", "-q", log=log)
    git(base, "checkout", "-f", "-q", "--detach", "origin/staging", log=log)
    git(base, "clean", "-qfd", log=log)
    # A checkout bumps download_tool.py's mtime, which makes ninja re-download (and
    # fail offline) every toolchain binary we already have. Backdate it instead.
    sh(["touch", "-t", "202401010000", "tools/download_tool.py"], base, check=True, log=log)
    # Same flags as this machine's existing build, so a warm build dir stays warm.
    sh(["python3", "configure.py", *configure_args()], base, check=True, log=log)
    ninja(base, log)
    return base


def make_clone(work, base, unit, branch, log):
    slug = unit.split("/")[-1]
    clone = work / f"{time.strftime('%m%d-%H%M%S')}-{slug}"
    sh(["cp", "-ac", str(base), str(clone)], work, check=True, log=log)
    git(clone, "switch", "-q", "-c", branch, log=log)
    return clone


def ninja(cwd, log, timeout=3600):
    return sh(["ninja"], cwd, timeout=timeout, check=True, log=log)


# ---------------------------------------------------------------------- targets


def load_report(clone):
    with open(clone / f"build/{UNIT}/report.json") as f:
        return json.load(f)


def report_units(report):
    """unit name -> (fuzzy, matched_code, total_code)."""
    out = {}
    for unit in report.get("units", []):
        m = unit.get("measures") or {}
        try:
            out[unit["name"]] = (
                float(m.get("fuzzy_match_percent") or 0.0),
                int(m.get("matched_code") or 0),
                int(m.get("total_code") or 0),
            )
        except (KeyError, TypeError, ValueError):
            continue
    return out


def open_pr_blob():
    proc = sh(
        ["gh", "pr", "list", "--repo", UPSTREAM, "--state", "open",
         "--json", "headRefName,title", "--limit", "100"],
        REPO,
    )
    if proc.returncode != 0:
        return ""
    try:
        prs = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return ""
    return " ".join(f"{p.get('headRefName', '')} {p.get('title', '')}" for p in prs)


def candidates(base):
    """Viable targets per the project selector, minus units with no real source."""
    sys.path.insert(0, str(base / "tools"))
    try:
        import agent_select_target as ast
    finally:
        sys.path.pop(0)
    report_path = base / f"build/{UNIT}/report.json"
    words = {}
    for cand in ast.extract_candidates(report_path):
        src = cand.get("source_path") or ""
        if not src.startswith("src/"):
            continue  # gba/* binaries and other source-less units are out of scope
        words[cand["name"]] = cand
    return words


def pick_target(base, state, forced, blob):
    report = load_report(base)
    units = report_units(report)
    funcs = {unit["name"]: unit.get("functions", []) for unit in report.get("units", [])}
    cands = candidates(base)
    scored = []
    for name, cand in cands.items():
        fuzzy, matched, total = units.get(name, (0.0, 0, 0))
        if total == 0 or fuzzy >= 99.999:
            continue
        attempts = state["units"].get(name, {}).get("attempts", 0)
        best = state["units"].get(name, {}).get("best_delta", 0.0)
        if attempts >= MAX_ATTEMPTS or (attempts >= 2 and best <= 0):
            continue
        stem = Path(cand["source_path"]).stem
        if blob and re.search(rf"\b{re.escape(stem)}\b", blob, re.IGNORECASE):
            continue  # an open PR already owns this unit
        cand = dict(cand)
        cand["worst_functions"] = worst_functions(funcs.get(name, []))
        scored.append(((name in DEAD_END_UNITS, -(total - matched)), cand))
    if forced:
        wanted = [c for _, c in scored if c["name"] in forced]
        if not wanted:
            raise SystemExit(f"forced unit(s) not selectable: {forced}")
        return wanted[0]
    if not scored:
        raise SystemExit("no selectable targets left")
    scored.sort(key=lambda item: item[0])
    return scored[0][1]


def worst_functions(functions, limit=6):
    """Unit functions with the most unmatched bytes, best guess at where to start."""
    rows = []
    for f in functions:
        fuzzy = float(f.get("fuzzy_match_percent") or 100.0)
        size = int(f.get("size") or 0)
        if fuzzy >= 99.99 or size == 0:
            continue
        rows.append((size * (100.0 - fuzzy) / 100.0, f["name"], fuzzy, size))
    rows.sort(reverse=True)
    return [f"{name} ({fuzzy:.1f}%, {size}b)" for _, name, fuzzy, size in rows[:limit]]


def prompt_for(cand, clone, branch, minutes):
    name = cand["name"]
    fuzzy = cand["fuzzy_match"]
    total = cand["total_code"]
    funcs = ", ".join(cand["worst_functions"]) or "none above 99%"
    return f"""Automated serial loop iteration: you are the only agent on this machine. Work alone; never ask questions, never wait for input.

REPO: {clone} (your cwd). BRANCH: {branch} (already checked out, based on origin/staging).
TARGET UNIT: {name} ({cand['source_path']}), {total} code bytes, currently {fuzzy:.2f}% fuzzy.
Biggest remaining functions: {funcs}
Wall-clock budget: about {minutes} minutes. Do real work the whole time; do not stop early to report.

GOAL: raise this unit's fuzzy_match_percent in build/GCCP01/report.json by recovering plausible original source.
Start by reading AGENTS.md and the lever catalog + CONFIRMED DEAD-ENDS list in WORK_SPLIT.md (both in your cwd).
Measure with `ninja` then build/GCCP01/report.json (source of truth for the score). Per symbol:
  build/tools/objdiff-cli diff -p . -u {name} -o - <symbol> > /tmp/{Path(name).stem}-<symbol>.json

HARD RULES
- Commit locally on {branch} as you go. NEVER push, never create/delete/switch branches, never rebase or reset --hard, never touch origin refs.
- No hardcoded addresses, no fake lbl_/fn_ names, no manual vtables/RTTI/ctors/dtors, no per-unit compiler-flag overrides, no pragma tricks to buy score (see AGENTS.md Critical Rules).
- Keep changes unit-local. If a shared header/struct/symbol fix is genuinely required, keep it minimal and call it out in your summary.
- `ninja` must succeed before you stop (it produces build/GCCP01/ok and refreshes report.json).
- Revert experiments that do not pay off; leave the tree in its best-scoring plausible state.

FINISH with a plain-text summary of at most 20 lines: unit fuzzy before -> after, each function improved with before -> after, what you changed, what you tried that failed, and any shared/file edits.
"""


# --------------------------------------------------------------------- measure


def unit_measure(clone, unit):
    fuzzy, _, _ = report_units(load_report(clone)).get(unit, (0.0, 0, 0))
    return fuzzy


def publish(clone, unit, branch, before, after, agent_out, log, work):
    status = git(clone, "status", "--porcelain", log=log).stdout.strip()
    if status:
        git(clone, "add", "-A", log=log)
        git(clone, "commit", "-q", "-m", f"loop: {unit} {before:.2f} -> {after:.2f}%",
            log=log)
    ahead = git(clone, "rev-list", "--count", "origin/staging..HEAD", log=log).stdout.strip()
    if ahead in ("", "0"):
        return None, "no commits to publish"
    git(clone, "push", "-q", "fork", f"HEAD:refs/heads/{branch}", log=log, check=True)
    body = work / "logs" / f"{branch.replace('/', '_')}.md"
    body.write_text(
        f"Automated loop iteration (`tools/pi_loop.py`).\n\n"
        f"- unit: `{unit}`\n"
        f"- unit fuzzy: {before:.2f}% -> {after:.2f}%\n"
        f"- branch: `{branch}`\n\n"
        f"### Agent summary\n\n```\n{agent_out.strip()[-4000:]}\n```\n"
    )
    proc = sh(
        ["gh", "pr", "create", "--repo", UPSTREAM, "--base", "staging",
         "--head", f"{FORK_OWNER}:{branch}",
         "--title", f"loop: {unit} {before:.2f} -> {after:.2f}%",
         "--body-file", str(body)],
        clone,
        log=log,
    )
    if proc.returncode != 0:
        return None, f"gh pr create failed: {proc.stderr.strip()[-500:]}"
    return proc.stdout.strip().splitlines()[-1], "ok"


# ---------------------------------------------------------------------- driver


def run_iteration(args, work, state):
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = work / "logs" / f"{stamp}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    print(f"[loop] base refresh (origin/staging) -> {work/'base'}", flush=True)
    base = refresh_base(work, log)

    if args.unit:
        forced = args.unit
        blob = ""
    else:
        forced, blob = None, open_pr_blob()
    cand = pick_target(base, state, forced, blob)
    unit = cand["name"]
    slug = unit.split("/")[-1].lower()
    branch = f"loop/{slug}-{stamp}"
    print(f"[loop] target: {unit} ({cand['fuzzy_match']:.2f}%)", flush=True)

    clone = make_clone(work, base, unit, branch, log)
    prompt = prompt_for(cand, clone, branch, args.minutes)
    (work / "logs" / f"{stamp}.prompt.txt").write_text(prompt)
    print(f"[loop] cloning done -> {clone}; launching pi agent ({args.minutes}m)", flush=True)

    started = time.time()
    proc = sh(
        ["pi", "-p", "--model", args.model, "--thinking", args.thinking, prompt],
        clone,
        timeout=args.minutes * 60,
        log=log,
    )
    agent_out = (proc.stdout or "") + ("\n" + proc.stderr if proc.stderr else "")
    print(f"[loop] agent exited {proc.returncode} in {int(time.time()-started)}s", flush=True)

    result = {"unit": unit, "branch": branch, "clone": str(clone), "log": str(log),
              "agent_exit": proc.returncode, "before": cand["fuzzy_match"]}
    try:
        ninja(clone, log)
    except SystemExit as exc:
        result.update(published=None, note=f"build failed: {exc}")
        return result
    after = unit_measure(clone, unit)
    result["after"] = after
    result["delta"] = round(after - cand["fuzzy_match"], 4)
    print(f"[loop] gate: {unit} {cand['fuzzy_match']:.2f}% -> {after:.2f}% "
          f"({result['delta']:+.4f})", flush=True)

    entry = state["units"].setdefault(unit, {"attempts": 0, "best_delta": 0.0})
    entry["attempts"] += 1
    entry["best_delta"] = max(entry["best_delta"], result["delta"])
    entry["last_delta"] = result["delta"]

    if result["delta"] <= 0:
        result["published"] = None
        result["note"] = "no improvement, discarded"
        return result
    if args.no_push:
        result["published"] = None
        result["note"] = "improvement kept locally (--no-push)"
        return result
    url, note = publish(clone, unit, branch, cand["fuzzy_match"], after, agent_out, log, work)
    result.update(published=url, note=note)
    print(f"[loop] publish: {url or note}", flush=True)
    return result


def prune(work, keep):
    clones = sorted(
        (p for p in work.iterdir() if p.is_dir() and p.name != "base" and p.name != "logs"),
        key=lambda p: p.stat().st_mtime,
    )
    for old in clones[: max(0, len(clones) - keep)]:
        shutil.rmtree(old, ignore_errors=True)


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--iterations", type=int, default=1, help="serial iterations to run")
    p.add_argument("--minutes", type=int, default=45, help="minutes per agent")
    p.add_argument("--unit", action="append", help="force a unit (repeatable), skips ranking")
    p.add_argument("--model", default="openrouter/~anthropic/claude-sonnet-latest")
    p.add_argument("--thinking", default="high")
    p.add_argument("--work-dir", default="/tmp/ffcc-loop")
    p.add_argument("--no-push", action="store_true", help="keep wins local, do not open PRs")
    p.add_argument("--keep-clones", type=int, default=4)
    p.add_argument("--dry-run", action="store_true", help="print target + prompt, then exit")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    work = Path(args.work_dir)
    work.mkdir(parents=True, exist_ok=True)
    (work / "logs").mkdir(exist_ok=True)
    state_path = work / "state.json"
    state = {"units": {}}
    if state_path.exists():
        state = json.loads(state_path.read_text())

    if not args.no_push and not args.dry_run:
        if sh(["gh", "auth", "status"], REPO).returncode != 0:
            raise SystemExit("gh is not authenticated; rerun with --no-push or auth gh")

    if args.dry_run:
        base = refresh_base(work, None)
        cand = pick_target(base, state, args.unit, "" if args.unit else open_pr_blob())
        print(prompt_for(cand, work / "clone", "loop/<slug>", args.minutes))
        return 0

    for i in range(args.iterations):
        print(f"[loop] === iteration {i+1}/{args.iterations} ===", flush=True)
        result = run_iteration(args, work, state)
        state_path.write_text(json.dumps(state, indent=1, sort_keys=True))
        with open(work / "iterations.jsonl", "a") as f:
            f.write(json.dumps(result, sort_keys=True) + "\n")
        prune(work, args.keep_clones)
    return 0


if __name__ == "__main__":
    sys.exit(main())
