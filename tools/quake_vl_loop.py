#!/usr/bin/env python3
"""Local Quake play loop: Xmux screenshot → Ollama VL → short key actions.

Spectator (human watches):
  xmux attach quake-gl --no-reconnect

Example:
  python3 tools/quake_vl_loop.py --session quake-gl --model qwen3-vl:4b --ticks 40
"""

from __future__ import annotations

import argparse
import base64
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

DEFAULT_SYSTEM = """You are playing classic Quake (GL) from first person.
Look at the screenshot (HUD at bottom: armor health shells).
Reply with ONLY one JSON object, no markdown, no thinking:
{"see":"corridor|wall|enemy|item|menu|dark|sky","move":"forward|back|none","turn":"left|right|none","fire":true|false,"ms":300}
Rules:
- ms between 200 and 500 only
- If wall fills the view or you are too close to metal, turn left or right and move none or short back
- If open corridor/room, move forward
- If you see a monster/enemy/soldier, fire true and face them (turn if needed)
- Never open menus; if menu visible use move none turn none fire false
- Prefer short advances; do not hold forward into walls
"""


def run(cmd: list[str], timeout: float = 30.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def xmux_screenshot(
    session: str,
    out: Path,
    *,
    max_w: int,
    quality: int,
    gray: bool,
) -> str:
    """Return path, or 'UNCHANGED' if --if-changed skipped write."""
    cmd = [
        "xmux",
        "screenshot",
        session,
        "-o",
        str(out),
        "--max",
        str(max_w),
        "--quality",
        str(quality),
    ]
    if gray:
        cmd.append("--gray")
    # Always write for VL (if-changed can stall first ticks)
    r = run(cmd, timeout=15.0)
    text = (r.stdout or "").strip() or str(out)
    if r.returncode != 0:
        err = (r.stderr or r.stdout or "screenshot failed").strip()
        raise RuntimeError(err)
    return text.splitlines()[-1] if text else str(out)


def xmux_key(
    session: str,
    combo: str,
    *,
    window: str,
    delay_ms: int = 0,
    down: bool = False,
    up: bool = False,
    repeat: int = 1,
) -> None:
    cmd = ["xmux", "key", session, combo, "-w", window, "--delay-ms", str(delay_ms)]
    if down:
        cmd.append("--down")
    if up:
        cmd.append("--up")
    if repeat > 1 and not down and not up:
        cmd.extend(["--repeat", str(repeat)])
    r = run(cmd, timeout=10.0)
    if r.returncode != 0:
        # Non-fatal: log and continue
        sys.stderr.write(f"key {combo}: {(r.stderr or r.stdout or '').strip()}\n")


def hold_key(
    session: str,
    combo: str,
    ms: int,
    *,
    window: str,
) -> None:
    xmux_key(session, combo, window=window, delay_ms=0, down=True)
    time.sleep(max(ms, 50) / 1000.0)
    xmux_key(session, combo, window=window, delay_ms=0, up=True)


def ollama_chat(
    *,
    host: str,
    model: str,
    image_path: Path,
    system: str,
    timeout: float,
) -> tuple[dict[str, Any], float, str]:
    b64 = base64.b64encode(image_path.read_bytes()).decode("ascii")
    body: dict[str, Any] = {
        "model": model,
        "stream": False,
        "format": "json",
        "options": {
            "temperature": 0.15,
            "num_predict": 220,
        },
        "messages": [
            {"role": "system", "content": system},
            {
                "role": "user",
                "content": "Act now. JSON only.",
                "images": [b64],
            },
        ],
    }
    # Disable thinking when API supports it (faster); still parse thinking fallback
    body["think"] = False

    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        f"{host.rstrip('/')}/api/chat",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    t0 = time.perf_counter()
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
    elapsed = time.perf_counter() - t0
    payload = json.loads(raw.decode("utf-8"))
    msg = payload.get("message", {}) or {}
    # qwen3-vl often fills "thinking" and leaves content empty
    content = (msg.get("content") or "").strip()
    if not content:
        content = (msg.get("thinking") or "").strip()
    action = parse_action(content)
    return action, elapsed, content


def parse_action(content: str) -> dict[str, Any]:
    text = content.strip()
    obj: dict[str, Any] | None = None

    # Strip markdown fences if model misbehaves
    if "```" in text:
        m = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.S)
        if m:
            try:
                obj = json.loads(m.group(1))
            except json.JSONDecodeError:
                obj = None

    # Prefer last JSON object in the blob (often after thinking)
    if obj is None:
        candidates = re.findall(r"\{[^{}]+\}", text, re.S)
        for cand in reversed(candidates):
            try:
                cand_obj = json.loads(cand)
            except json.JSONDecodeError:
                continue
            # Must look like our schema
            if any(k in cand_obj for k in ("move", "turn", "fire", "see", "ms")):
                obj = cand_obj
                break

    # Free-text heuristic when VL only "thinks" (qwen3-vl:4b often does this)
    if obj is None:
        low = text.lower()
        see = "unknown"
        move = "forward"
        turn = "none"
        fire = False
        ms = 350
        if "menu" in low:
            see, move, turn, fire, ms = "menu", "none", "none", False, 200
        elif any(w in low for w in ("enemy", "monster", "soldier", "grunt", "ogre", "fiend")):
            see, move, turn, fire, ms = "enemy", "forward", "none", True, 400
        elif any(w in low for w in ("too close", "against the wall", "faceplant", "blocked")):
            see, move, turn, fire, ms = "wall", "none", "left", False, 300
        elif "wall" in low and "corridor" not in low and "open" not in low:
            see, move, turn, fire, ms = "wall", "none", "right", False, 300
        elif "corridor" in low or "hallway" in low or "tunnel" in low or "doorway" in low:
            see, move, turn, fire, ms = "corridor", "forward", "none", False, 400
        elif "dark" in low:
            see, move, turn, fire, ms = "dark", "forward", "none", False, 300
        obj = {"see": see, "move": move, "turn": turn, "fire": fire, "ms": ms}

    see = str(obj.get("see", "unknown")).lower()
    move = str(obj.get("move", "none")).lower()
    turn = str(obj.get("turn", "none")).lower()
    fire = bool(obj.get("fire", False))
    try:
        ms = int(obj.get("ms", 300))
    except (TypeError, ValueError):
        ms = 300

    if move not in ("forward", "back", "none"):
        move = "none"
    if turn not in ("left", "right", "none"):
        turn = "none"
    # Normalize multi-word see
    if "enemy" in see:
        see = "enemy"
    elif "wall" in see:
        see = "wall"
    elif "corridor" in see:
        see = "corridor"
    elif "dark" in see:
        see = "dark"
    elif "menu" in see:
        see = "menu"

    ms = max(200, min(500, ms))

    # Safety: wall → don't charge
    if see in ("wall", "menu") and move == "forward":
        move = "none"
        if turn == "none":
            turn = "left"
        fire = False
    if see == "enemy":
        fire = True

    return {"see": see, "move": move, "turn": turn, "fire": fire, "ms": ms}


def apply_action(
    session: str,
    action: dict[str, Any],
    *,
    window: str,
) -> None:
    ms = int(action["ms"])
    turn = action["turn"]
    move = action["move"]
    fire = bool(action["fire"])

    if turn == "left":
        hold_key(session, "Left", max(ms // 2, 150), window=window)
    elif turn == "right":
        hold_key(session, "Right", max(ms // 2, 150), window=window)

    if fire:
        xmux_key(session, "Control_L", window=window, delay_ms=45, repeat=2)

    if move == "forward":
        hold_key(session, "Up", ms, window=window)
        # Micro second step without new VL (human-ish)
        time.sleep(0.08)
        hold_key(session, "Up", min(ms, 250), window=window)
    elif move == "back":
        hold_key(session, "Down", max(ms // 2, 150), window=window)


def bootstrap_game(session: str, window: str) -> None:
    """Best-effort: close console noise, give weapons + god."""
    # Toggle console open
    xmux_key(session, "grave", window=window, delay_ms=40)
    time.sleep(0.25)
    # Type commands (xmux type)
    r = run(
        [
            "xmux",
            "type",
            session,
            "-w",
            window,
            "impulse 9; god; gamma 0.5",
            "--delay-ms",
            "8",
        ],
        timeout=15.0,
    )
    if r.returncode != 0:
        sys.stderr.write(f"type: {(r.stderr or '').strip()}\n")
    xmux_key(session, "Return", window=window, delay_ms=40)
    time.sleep(0.2)
    xmux_key(session, "grave", window=window, delay_ms=40)
    time.sleep(0.3)
    xmux_key(session, "2", window=window, delay_ms=40)
    time.sleep(0.15)


def main() -> int:
    ap = argparse.ArgumentParser(description="Quake Xmux + Ollama VL play loop")
    ap.add_argument("--session", default="quake-gl")
    ap.add_argument("--window", default="glquake")
    ap.add_argument("--model", default="qwen3-vl:4b")
    ap.add_argument("--host", default="http://127.0.0.1:11434")
    ap.add_argument("--ticks", type=int, default=50)
    ap.add_argument("--max-width", type=int, default=320)
    ap.add_argument("--quality", type=int, default=40)
    ap.add_argument("--no-gray", action="store_true")
    ap.add_argument("--shot", type=Path, default=Path("/tmp/quake_vl_frame.jpg"))
    ap.add_argument("--log", type=Path, default=Path("/tmp/quake_vl.log"))
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--bootstrap", action="store_true", help="impulse 9 + god once")
    ap.add_argument("--dry-run", action="store_true", help="VL only, no keys")
    args = ap.parse_args()

    args.log.parent.mkdir(parents=True, exist_ok=True)
    log_f = args.log.open("a", encoding="utf-8")

    def log(msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        log_f.write(line + "\n")
        log_f.flush()

    log(
        f"start session={args.session} model={args.model} ticks={args.ticks} "
        f"shot={args.shot}"
    )
    print("SPECTATOR: xmux attach quake-gl --no-reconnect", flush=True)

    if args.bootstrap and not args.dry_run:
        log("bootstrap impulse9+god")
        bootstrap_game(args.session, args.window)

    for i in range(1, args.ticks + 1):
        tick_t0 = time.perf_counter()
        try:
            path = xmux_screenshot(
                args.session,
                args.shot,
                max_w=args.max_width,
                quality=args.quality,
                gray=not args.no_gray,
            )
            img_path = Path(path if path != "UNCHANGED" else args.shot)
            if not img_path.is_file():
                log(f"tick {i}: no image {img_path}")
                time.sleep(0.3)
                continue

            action, vl_s, raw = ollama_chat(
                host=args.host,
                model=args.model,
                image_path=img_path,
                system=DEFAULT_SYSTEM,
                timeout=args.timeout,
            )
            nbytes = img_path.stat().st_size
            log(
                f"tick {i:03d} vl={vl_s:.2f}s img={nbytes}B "
                f"see={action['see']} move={action['move']} "
                f"turn={action['turn']} fire={action['fire']} ms={action['ms']}"
            )
            if args.dry_run:
                log(f"  raw={raw[:120]!r}")
            else:
                apply_action(args.session, action, window=args.window)

        except urllib.error.URLError as e:
            log(f"tick {i}: ollama error {e}")
            time.sleep(1.0)
        except Exception as e:
            log(f"tick {i}: error {type(e).__name__}: {e}")
            time.sleep(0.5)

        # Pace: avoid hammering if VL was instant
        elapsed = time.perf_counter() - tick_t0
        if elapsed < 0.15:
            time.sleep(0.15 - elapsed)

    log("done")
    log_f.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
