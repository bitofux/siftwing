"""原创Unicode参考：逐字符覆盖集合、html.escape及HTML解析，独立于生产区间扫描。"""
import argparse
import html
from html.parser import HTMLParser
import itertools
import random
import subprocess

ASCII_FOLD = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")
MAX = 2**64 - 1
PLENTY = (65536, 100, 65536, 10**9, 4096, 10**6)


class Display(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.marked = False
        self.mask = []

    def handle_starttag(self, tag, attrs):
        assert tag == "mark" and not attrs and not self.marked, (tag, attrs)
        self.marked = True

    def handle_endtag(self, tag):
        assert tag == "mark" and self.marked, tag
        self.marked = False

    def handle_data(self, data):
        self.parts.append(data)
        self.mask.extend([self.marked] * len(data))


def reference(words, body, limits):
    text_bytes, terms, query_bytes, pairs, span_count, output_bytes = limits
    if not all(limits):
        return "ERR highlight.limits"
    if len(body) > text_bytes:
        return "ERR highlight.text_bytes"
    try:
        text = body.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return "ERR highlight.utf8"
    if "\0" in text:
        return "ERR highlight.nul"
    keys = set(words.translate(ASCII_FOLD).split("|")) - {""}
    if len(keys) > terms:
        return "ERR highlight.query_terms"
    if sum(len(key.encode()) for key in keys) > query_bytes:
        return "ERR highlight.query_bytes"
    if len(text) * sum(len(key) for key in keys) > pairs:
        return "ERR highlight.work"
    # 每个词独立find全部重叠命中，字符集合取并集；不模拟生产的起点→最长末端在线合并。
    covered = set()
    folded = text.translate(ASCII_FOLD)
    for key in keys:
        at = folded.find(key)
        while at >= 0:
            covered.update(range(at, at + len(key)))
            at = folded.find(key, at + 1)
    chunks = []
    spans = []
    for marked, group in itertools.groupby(range(len(text)), key=lambda i: i in covered):
        positions = list(group)
        start, end = positions[0], positions[-1] + 1
        escaped = html.escape(text[start:end], quote=True).replace("&#x27;", "&#39;")
        if marked:
            spans.append((len(text[:start].encode()), len(text[start:end].encode())))
            chunks.append("<mark>" + escaped + "</mark>")
        else:
            chunks.append(escaped)
    if len(spans) > span_count:
        return "ERR highlight.spans"
    rendered = "".join(chunks)
    if len(rendered.encode()) > output_bytes:
        return "ERR highlight.output_bytes"
    display = Display()
    display.feed(rendered)
    display.close()
    assert "".join(display.parts) == text and not display.marked
    assert display.mask == [i in covered for i in range(len(text))]
    protocol = f"OK {rendered.encode().hex() or '-'} {len(spans)}"
    return protocol + "".join(f" {offset}:{length}" for offset, length in spans)


def cases():
    fixed = ["", "Red RED red", "中国😀RED尾巴", "<中国人>&", "a<mark>aa</mark>",
             "&lt;<script>\"'&</script>", "👨‍👩‍👧", "éÉé", "\ufeffX", " \t\n ",
             "𐀀\U0010ffff尾", "zaaaa", "abcab", "a b a"]
    for body in fixed:
        for words in ("red|中国", "aa|aaa", "ab|bc|a", "😀|👩", "x", "é|é", "<|&|\"|'", "script|amp|mark"):
            yield words, body.encode(), PLENTY
    rng = random.Random(13220261012)
    alphabet = list("abczRED 012.,\n\t<&>\"'") + ["中", "国", "人", "😀", "👩", "‍", "é", "É", "́", "𐀀", "\ufeff"]
    vocabulary = ["red", "a", "b", "bc", "aba", "中国", "中国人", "国", "😀", "é", "é", "𐀀", "👩", "<", "&", "\"", "'", "amp"]
    for iteration in range(4000):
        keys = rng.choices(vocabulary, k=rng.randint(1, 6))
        words = "|".join(keys)
        body = "".join(rng.choices(alphabet, k=rng.randint(0, 90)))
        if iteration % 3 == 0:
            at = rng.randrange(len(body) + 1)
            body = body[:at] + keys[0].upper() + body[at:]
        limits = list(PLENTY)
        unique = set(keys)
        work = len(body) * sum(len(key) for key in unique)
        category = iteration % 14
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
        elif category in (5, 6):
            success = reference(words, body.encode(), PLENTY).split()
            size = 0 if success[1] == "-" else len(bytes.fromhex(success[1]))
            limits[5] = max(1, size - (category == 6))
        elif category in (7, 8):
            count = int(reference(words, body.encode(), PLENTY).split()[2])
            limits[4] = max(1, count - (category == 8))
        elif category == 9:
            limits[rng.randrange(6)] = 0
        yield words, body.encode(), tuple(limits)
    for bad in (b"\x80", b"\xc0\xaf", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xf0\x9f\x98", b"\xff", b"\xe2A"):
        for prefix in (b"", "中国a".encode()):
            yield "a", prefix + bad, PLENTY
    for body in (b"a\0z", b"\0\xff", b"\0", "中国\0".encode()):
        yield "a", body, PLENTY
    yield "中国|red", "头中国RED尾<&>".encode(), (MAX,) * 6
    yield "&", b"&", (1, 1, 1, 1, 1, 18)
    yield "&", b"&", (1, 1, 1, 1, 1, 17)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    args = parser.parse_args()
    samples = list(cases())
    protocol = "\n".join(f"{words.encode().hex()} {body.hex() or '-'} " + " ".join(map(str, limits))
                         for words, body, limits in samples) + "\n"
    expected = [reference(*sample) for sample in samples]
    for process in range(2):
        run = subprocess.run([args.probe], input=protocol, text=True, capture_output=True, timeout=40)
        if run.returncode:
            raise AssertionError(("probe failed", process, run.returncode, run.stderr))
        rows = run.stdout.splitlines()
        assert len(rows) == len(expected), (len(rows), len(expected), run.stderr)
        for index, (actual, want) in enumerate(zip(rows, expected)):
            assert actual == want, ("Unicode/HTML reference mismatch", process, index, samples[index], actual, want)
    print(f"highlight independent reference cases={len(samples)} processes=2 failures=0")


if __name__ == "__main__":
    main()
