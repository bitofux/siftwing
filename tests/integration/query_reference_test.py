"""原创固定种子查询；ASCII正则/Counter独立参考，不调用生产文本codec。"""
import argparse
import collections
import random
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    args = parser.parse_args()
    maximum = 2**64 - 1
    cases = [(b"THE Red red BLUE 17 red", 1, 10, 100, None),
             (b"AND OR red", 0, 1, 1, None),
             (b"a", 1, maximum, maximum, None)]
    for text in [b"", b" \t\r\n", b"?!", b"THE the", "中文😀".encode()]:
        for mode in [0, 1]:
            cases.append((text, mode, 1, 100, "query.empty"))
    cases += [(b"\xff", 99, 0, 0, "query.limits"),
              (b"\xff", 99, 0, 100, "query.mode"),
              (b"\xff", 1, 0, 100, "query.top_k"),
              (b"red", 1, 101, 100, "query.top_k"),
              (b"red", 1, 10, 9, "query.top_k"),
              (b"red\xff", 1, 1, 100, "normalize.utf8"),
              (b"a\0b", 1, 1, 100, "normalize.nul"),
              (b"a" * 1001, 1, 1, 100, "normalize.input_bytes"),
              (b"a" * 101, 1, 1, 100, "tokenizer.token_bytes")]
    generator = random.Random(1272026)
    words = ["RED", "blue", "The", "17", "AND", "OR", "a1", "UNKNOWN", "中文", "😀", "é", "e\u0301"]
    separators = [" ", "\t", "\r\n", "+", "！", "　", "'", "-", "\u00a0", "\u2003"]
    for _ in range(240):
        raw = "".join(generator.choice(words) + generator.choice(separators)
                      for _ in range(generator.randrange(1, 21))).encode()
        cases.append((raw, generator.randrange(2), generator.randrange(1, 101), 100, None))
    wire = "".join(f"{mode} {k} {limit} {raw.hex() or '_'}\n" for raw, mode, k, limit, _ in cases)
    expected = []
    translation = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")
    for raw, mode, k, _, failure in cases:
        if failure is not None:
            expected.append("ERROR " + failure)
            continue
        counts = collections.Counter(word for word in re.findall(r"[a-z0-9]+", raw.decode().translate(translation))
                                     if word != "the")
        if not counts:
            expected.append("ERROR query.empty")
            continue
        expected.append(f"OK {mode} {k} {sum(counts.values())} {len(counts)}")
        expected.extend(f"T {word.encode().hex()} {counts[word]}" for word in sorted(counts))
    # 多个全新进程都核独立参考；同一进程连续请求也不得累计TF或泄露先前选项。
    for iteration in range(2):
        result = subprocess.run([args.probe], input=wire, text=True, capture_output=True, timeout=30)
        if result.returncode != 0 or result.stdout.splitlines() != expected:
            actual = result.stdout.splitlines()
            mismatch = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                            min(len(actual), len(expected)))
            raise AssertionError(f"iteration={iteration}; exit={result.returncode}; line={mismatch}; "
                                 f"actual={actual[mismatch:mismatch+1]}; expected={expected[mismatch:mismatch+1]}; "
                                 f"stderr={result.stderr}")
    print(f"query independent cases={len(cases)} processes=2 checked_lines={2 * len(expected)} failures=0")


if __name__ == "__main__":
    main()
