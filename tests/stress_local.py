#!/usr/bin/env python3
"""
Stress test locale per zftpd (macOS/Linux) che simula il caso "tanti file piccoli + churn".

Prerequisiti:
- Server zftpd già in esecuzione su host/porta indicati (default 127.0.0.1:2121).
- Credenziali FTP valide (default user=test, pass=test).

Cosa fa:
1. Genera un dataset di molti file piccoli in dir annidate (default /tmp/zftpd-tiny).
2. Avvia un thread di churn che cancella/riscrive file durante il transfer.
3. Esegue RETR paralleli via FTP (ftplib) su tutti i file del dataset.

Uso tipico (server avviato con root=/tmp):
    python3 tests/stress_local.py --remote-base /zftpd-tiny

Se il server ha root impostata su /tmp/zftpd-tiny, usa:
    python3 tests/stress_local.py --remote-base /

Parametri:
  --host, --port, --user, --password : connessione FTP
  --root          : path locale dove creare il dataset
  --remote-base   : prefisso remoto (es. /zftpd-tiny o /)
  --dirs          : numero di sottodirectory (default 8)
  --files         : file per directory (default 400)
  --concurrency   : worker FTP paralleli (default 6)
  --duration      : secondi di churn prima di fermare (default 60)
"""

from __future__ import annotations

import argparse
import os
import random
import string
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from ftplib import FTP
from pathlib import Path
from typing import List


def make_dataset(root: Path, dirs: int, files_per_dir: int) -> List[Path]:
    root.mkdir(parents=True, exist_ok=True)
    rels: List[Path] = []
    for d in range(1, dirs + 1):
        sub = root / f"dir{d}"
        sub.mkdir(parents=True, exist_ok=True)
        for i in range(1, files_per_dir + 1):
            size = 1024 + random.randint(0, 3072)
            payload = ("X" * size).encode("ascii")
            path = sub / f"f{i}.bin"
            with open(path, "wb") as fh:
                fh.write(payload)
            rels.append(path.relative_to(root))
    return rels


def churn_worker(root: Path, stop_evt: threading.Event) -> None:
    files = list(root.rglob("*.bin"))
    while not stop_evt.is_set():
        # Cancella una manciata di file e riscrive con contenuto diverso
        sample = random.sample(files, k=min(25, len(files))) if files else []
        for p in sample:
            try:
                p.unlink(missing_ok=True)
            except OSError:
                pass
        for p in sample:
            try:
                payload = ("Y" * (1500 + random.randint(0, 2048))).encode("ascii")
                p.parent.mkdir(parents=True, exist_ok=True)
                with open(p, "wb") as fh:
                    fh.write(payload)
            except OSError:
                pass
        time.sleep(0.5)


def fetch_file(host: str, port: int, user: str, password: str, remote_base: str, relpath: Path) -> str:
    remote_path = f"{remote_base.rstrip('/')}/{relpath.as_posix()}"
    # ftplib richiede percorso assoluto: assicurati che inizi con '/'
    if not remote_path.startswith("/"):
        remote_path = "/" + remote_path
    try:
        with FTP() as ftp:
            ftp.connect(host=host, port=port, timeout=5)
            ftp.login(user=user, passwd=password)
            # usa una sink in memoria
            ftp.retrbinary(f"RETR {remote_path}", lambda _: None)
        return "ok"
    except Exception as exc:  # pragma: no cover - diagnosi runtime
        return f"err:{type(exc).__name__}:{exc}"


def main() -> int:
    parser = argparse.ArgumentParser(description="Stress locale zftpd (mirror parziale)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2121)
    parser.add_argument("--user", default="test")
    parser.add_argument("--password", default="test")
    parser.add_argument("--root", default="/tmp/zftpd-tiny")
    parser.add_argument("--remote-base", default="/zftpd-tiny")
    parser.add_argument("--dirs", type=int, default=8)
    parser.add_argument("--files", type=int, default=400)
    parser.add_argument("--concurrency", type=int, default=6)
    parser.add_argument("--duration", type=int, default=60)
    args = parser.parse_args()

    root = Path(args.root)
    print(f"[prep] dataset in {root} ...")
    rels = make_dataset(root, args.dirs, args.files)
    print(f"[prep] generati {len(rels)} file")

    stop_evt = threading.Event()
    churn_thr = threading.Thread(target=churn_worker, args=(root, stop_evt), daemon=True)
    churn_thr.start()

    results = []
    start = time.time()
    with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futs = [pool.submit(fetch_file, args.host, args.port, args.user, args.password, args.remote_base, rel) for rel in rels]
        for fut in as_completed(futs):
            results.append(fut.result())
            if time.time() - start > args.duration:
                break

    stop_evt.set()
    churn_thr.join(timeout=2)

    ok = sum(1 for r in results if r == "ok")
    err = [r for r in results if r != "ok"]
    print(f"[done] ok={ok}, err={len(err)} (durata ~{int(time.time()-start)}s)")
    if err:
        print(f"[sample errors] {err[:5]}")
    return 0 if not err else 1


if __name__ == "__main__":
    raise SystemExit(main())
