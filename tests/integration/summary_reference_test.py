"""原创Unicode参考：Python严格解码、find定位及枚举合法窗口，不调用生产解码/截取算法。"""
import argparse
import random
import subprocess

ASCII_FOLD = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")
MAX = 2**64 - 1
PLENTY = (65536, 100, 65536, 10**9, 4096)


def reference(words, body, window, leading, limits):
    content_bytes, terms, query_bytes, pairs, max_window = limits
    if not all(limits):
        return "ERR summary.limits"
    if not window or window > max_window or leading >= window:
        return "ERR summary.options"
    if len(body) > content_bytes:
        return "ERR summary.content_bytes"
    try:
        text = body.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return "ERR summary.utf8"
    if "\0" in text:
        return "ERR summary.nul"
    keys = set(words.translate(ASCII_FOLD).split("|")) - {""}
    if len(keys) > terms:
        return "ERR summary.query_terms"
    if sum(len(key.encode("utf-8")) for key in keys) > query_bytes:
        return "ERR summary.query_bytes"
    if len(text) * sum(len(key) for key in keys) > pairs:
        return "ERR summary.work"
    positions = [text.translate(ASCII_FOLD).find(key) for key in keys]
    hits = [position for position in positions if position >= 0]
    width = min(window, len(text))
    if hits:
        first = min(hits)
        # 枚举能包含首命中起点的合法窗口，选前文长度最接近期望者；独立于生产clamp公式。
        starts = [start for start in range(len(text) - width + 1) if start <= first < start + width]
        start = min(starts, key=lambda index: (abs(first - index - leading), index))
    else:
        start = 0
    out = text[start:start + width].encode("utf-8")
    offset = len(text[:start].encode("utf-8"))
    return f"OK {out.hex() or '-'} {offset} {int(bool(hits))} {int(start > 0)} {int(start + width < len(text))}"


def cases():
    fixed = ["", "0123Red tail", "中国😀RED尾巴", "头中国尾", "👨‍👩‍👧", "éÉé", "\ufeffX", " \t\n ", "𐀀\U0010ffff尾", "zaaaa"]
    for body in fixed:
        for words in ("red|中国", "aa|aaa", "😀|👩", "x", "é|é"):
            for window in (1, 2, 5, 20):
                for leading in (0, window - 1):
                    yield words, body.encode(), window, leading, PLENTY
    rng = random.Random(13120261012)
    alphabet = list("abczRED 012.,\n\t") + ["中", "国", "甲", "😀", "👩", "‍", "é", "É", "́", "𐀀", "\ufeff"]
    vocabulary = ["red", "blue", "a", "bc", "aba", "中国", "😀", "甲", "é", "é", "𐀀", "👩"]
    for iteration in range(3000):
        keys = rng.choices(vocabulary, k=rng.randint(1, 6))
        words = "|".join(keys)
        body = "".join(rng.choices(alphabet, k=rng.randint(0, 80)))
        if iteration % 3 == 0:
            at = rng.randrange(len(body) + 1)
            body = body[:at] + keys[0].upper() + body[at:]
        window = rng.randint(1, 50)
        leading = rng.randrange(window)
        limits = list(PLENTY)
        unique = set(keys)
        work = len(body) * sum(len(key) for key in unique)
        category = iteration % 12
        if category == 0:
            limits[3] = max(1, work)
        elif category == 1:
            limits[3] = max(1, work - 1)
        elif category == 2:
            limits[0] = max(1, len(body.encode()) - 1)
        elif category == 3:
            limits[1] = max(1, len(unique) - 1)
        elif category == 4:
            limits[2] = max(1, sum(len(key.encode()) for key in unique) - 1)
        elif category == 5:
            limits[4] = max(1, window - 1)
        elif category == 6:
            limits[rng.randrange(5)] = 0
        yield words, body.encode(), window, leading, tuple(limits)
    for bad in (b"\x80", b"\xc0\xaf", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xf0\x9f\x98", b"\xff", b"\xe2A"):
        for prefix in (b"", "中国a".encode()):
            yield "a", prefix + bad, 1, 0, PLENTY
    for body in (b"a\0z", b"\0\xff", b"\0", "中国\0".encode()):
        yield "a", body, 1, 0, PLENTY
    for leading in (0, MAX - 1, MAX):
        yield "中国|red", "头中国RED尾".encode(), MAX, leading, (MAX,) * 5
    for limits in ((3, 1, 3, 2, 2), (3, 1, 3, 1, 2)):
        yield "中国", "中".encode(), 1, 0, limits


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    args = parser.parse_args()
    samples = list(cases())
    protocol = "\n".join(f"{words.encode().hex()} {body.hex() or '-'} {window} {leading} " + " ".join(map(str, limits))
                         for words, body, window, leading, limits in samples) + "\n"
    expected = [reference(*sample) for sample in samples]
    for process in range(2):
        run = subprocess.run([args.probe], input=protocol, text=True, capture_output=True, timeout=40)
        if run.returncode:
            raise AssertionError(("probe failed", process, run.returncode, run.stderr))
        rows = run.stdout.splitlines()
        assert len(rows) == len(expected), (len(rows), len(expected), run.stderr)
        for index, (actual, want) in enumerate(zip(rows, expected)):
            assert actual == want, ("Unicode reference mismatch", process, index, samples[index], actual, want)
    print(f"summary independent reference cases={len(samples)} processes=2 failures=0")


if __name__ == "__main__":
    main()
