"""原创词序列的独立Python集合/Counter参考，区分文档TF和推荐累计，并保留重复文档。"""
import argparse
from collections import Counter
import json
from pathlib import Path
import random
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    args = parser.parse_args()
    rng = random.Random(113)
    words = ['the', 'The', '中国', '的', 'a1', 'one', 'one', 'é', '😀', '#', "a's", 'a', 's', '𠀀']
    with tempfile.TemporaryDirectory(prefix='siftwing-frequency-reference-') as directory:
        root = Path(directory)
        en, cn = root / 'en', root / 'cn'
        en.write_bytes(b'\xef\xbb\xbf THE\r\n the\n#\na\'s\n')
        cn.write_text('的\n', encoding='utf-8')
        stopwords = {'the', '#', "a's", '的'}
        for case in range(40):
            documents = [[], ['the', '的'], ['中国', '中国', 'one', 'one'], ['中国', '中国', 'one', 'one']]
            documents += [[rng.choice(words) for _ in range(rng.randrange(0, 100))]
                          for _ in range(rng.randrange(1, 10))]
            payload = ''.join('@\n' + ''.join(token.encode().hex() + '\n' for token in document)
                              for document in documents).encode()
            result = subprocess.run([args.probe, str(en), str(cn)], input=payload, capture_output=True)
            assert result.returncode == 0, (case, result.stdout, result.stderr)
            report = json.loads(result.stdout)
            assert [bytes.fromhex(w).decode() for w in report['stopwords_hex']] == sorted(stopwords, key=str.encode)
            combined = Counter()
            assert len(report['documents']) == len(documents)
            for document, actual in zip(documents, report['documents']):
                filtered = [word for word in document if word not in stopwords]
                counts = Counter(filtered)
                assert [bytes.fromhex(w).decode() for w in actual['tokens_hex']] == filtered
                assert actual['total_tokens'] == len(filtered)
                expected = [[word.encode().hex(), counts[word]] for word in sorted(counts, key=str.encode)]
                assert actual['terms'] == expected
                combined.update(counts)
            corpus = report['recommendation']
            assert corpus['documents'] == len(documents)
            assert corpus['total_tokens'] == sum(combined.values())
            assert corpus['terms'] == [[word.encode().hex(), combined[word]] for word in sorted(combined, key=str.encode)]
    print('term-frequency independent reference batches=40 failures=0')


if __name__ == '__main__':
    main()
