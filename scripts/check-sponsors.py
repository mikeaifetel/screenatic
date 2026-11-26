#!/usr/bin/env python3
"""Checks docs/sponsors*.json before a push (the site deploys on every push to main).

Usage: python3 scripts/check-sponsors.py [file ...]   (default: docs/sponsors.json docs/sponsors-dev.json)

Errors (exit 1): anything the apps would drop or show wrong. Warnings: things worth a second look.
Text length is counted in units, a CJK / full-width character = 2, so one limit fits the card in
every language: title <= 40 units (2 lines), body <= 150 units (4 lines with margin).
"""
import datetime
import json
import os
import re
import sys
import unicodedata

LANGS = {"en", "zh-CN", "zh-TW", "ja", "ko", "es", "pt-BR", "fr", "de", "it"}
BASE_LANGS = {"zh", "pt"}  # accepted as fallback keys ("zh" for both Chinese variants)
TITLE_UNITS = 40
BODY_UNITS = 150
MAX_BYTES = 64 * 1024
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
REGION = re.compile(r"^[A-Z]{2}$")


def units(s: str) -> int:
    return sum(2 if unicodedata.east_asian_width(c) in ("W", "F") else 1 for c in s)


def check(path: str) -> int:
    errors, warnings = [], []
    raw = open(path, "rb").read()
    if len(raw) > MAX_BYTES:
        errors.append(f"file is {len(raw)} bytes; the apps ignore files over {MAX_BYTES}")
    try:
        root = json.loads(raw)
    except ValueError as e:
        print(f"{path}: ERROR not valid JSON: {e}")
        return 1
    if "country" in root:
        errors.append('remove "country": the site\'s worker adds it per request (site/worker.js)')
    items = root.get("items")
    if not isinstance(items, list):
        print(f"{path}: ERROR no \"items\" list")
        return 1
    today = datetime.datetime.now(datetime.timezone.utc).date().isoformat()
    ids = set()
    for i, o in enumerate(items):
        where = f"item {i} ({o.get('id', '?') if isinstance(o, dict) else '?'})"
        err = lambda m: errors.append(f"{where}: {m}")
        warn = lambda m: warnings.append(f"{where}: {m}")
        if not isinstance(o, dict):
            err("not an object")
            continue
        if not isinstance(o.get("id"), str) or not re.match(r"^[a-z0-9][a-z0-9-]{0,39}$", o["id"]):
            err("id must be a-z, 0-9 and \"-\", up to 40 characters (it is in the QR URL)")
        elif o["id"] in ids:
            err("duplicate id")
        else:
            ids.add(o["id"])
        if o.get("kind") not in ("house", "sponsored"):
            err('kind must be "house" or "sponsored"')
        for field, limit, required in (("title", TITLE_UNITS, True), ("body", BODY_UNITS, False)):
            m = o.get(field)
            if m is None:
                if required:
                    err(f"missing {field}")
                continue
            if not isinstance(m, dict) or not m:
                err(f"{field} must be an object of language -> text")
                continue
            if "en" not in m:
                err(f"{field} has no \"en\" text (the fallback for other languages)")
            for lang, text in m.items():
                if lang not in LANGS and lang not in BASE_LANGS:
                    err(f"{field}.{lang}: unknown language (use {', '.join(sorted(LANGS))})")
                if not isinstance(text, str) or not text.strip():
                    err(f"{field}.{lang}: empty")
                    continue
                u = units(text)
                if u > limit:
                    err(f"{field}.{lang}: {u} units, limit {limit} (CJK counts 2): {text[:40]}…")
                if text != text.strip() or "\n" in text:
                    warn(f"{field}.{lang}: leading/trailing spaces or a line break")
        langs = o.get("languages")
        if langs is not None:
            if not isinstance(langs, list) or any(l not in LANGS for l in langs):
                err(f"languages must list our tags: {', '.join(sorted(LANGS))}")
            else:
                for l in langs:
                    if l not in (o.get("title") or {}) and l.split("-")[0] not in (o.get("title") or {}):
                        warn(f"shown in {l} but has no {l} title (falls back to en)")
        for key in ("regions", "excludeRegions"):
            v = o.get(key)
            if v is not None and (not isinstance(v, list) or any(not isinstance(r, str) or not REGION.match(r) for r in v)):
                err(f"{key} must list 2-letter upper-case country codes")
        for key in ("start", "end"):
            v = o.get(key)
            if v is not None and (not isinstance(v, str) or not DATE.match(v)):
                err(f"{key} must be YYYY-MM-DD (UTC)")
        if isinstance(o.get("start"), str) and isinstance(o.get("end"), str) and o["start"] > o["end"]:
            err("start is after end")
        if isinstance(o.get("end"), str) and o["end"] < today:
            warn(f"ended on {o['end']}; remove it")
        for key, limit in (("qr", 300), ("image", 2048)):
            v = o.get(key)
            if v is not None and (not isinstance(v, str) or not v.startswith("https://") or len(v) > limit):
                err(f"{key} must be an https:// URL up to {limit} characters")
        if o.get("qr") is None:
            warn("no qr: the card has no way to act on it")
        if o.get("image", "").startswith("https://screenatic.com/"):
            local = os.path.join("docs", o["image"][len("https://screenatic.com/"):])
            if not os.path.exists(local):
                err(f"image not in the repo: {local}")
            elif not local.lower().endswith((".png", ".jpg", ".jpeg")):
                err("image must be PNG or JPEG (the apps can't draw SVG)")
            elif os.path.getsize(local) > 1024 * 1024:
                err("image over 1 MB (the Android app skips it)")
        w = o.get("weight", 1)
        if not isinstance(w, int) or w < 0:
            err("weight must be a whole number >= 0")
        elif w == 0:
            warn("weight 0: never shown")
    for m in errors:
        print(f"{path}: ERROR {m}")
    for m in warnings:
        print(f"{path}: warning {m}")
    print(f"{path}: {len(items)} item(s), {len(errors)} error(s), {len(warnings)} warning(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    files = sys.argv[1:] or ["docs/sponsors.json", "docs/sponsors-dev.json"]
    sys.exit(max(check(f) for f in files))
