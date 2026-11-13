#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SKK辞書 → CTRPF 日本語キーボード用辞書 (skk_jisyo.dic) 変換スクリプト

使い方:
    python skk2ctrpf.py SKK-JISYO.M [SKK-JISYO.jinmei ...] -o skk_jisyo.dic

  - 複数の辞書を指定すると、前に書いた辞書の候補から順に並びます。
  - 送りなし (よみがひらがなだけ) の項目だけを使います。
  - できた skk_jisyo.dic を SDカードの /luma/plugins/ に置いてください。

出力形式 (KeyboardImpl.cpp の JpInput::Lookup と対応):
    "SKKU" + u32 件数 + u32 オフセット[件数] + "よみ\\t候補1/候補2/...\\n" の行
    行はよみの UTF-8 バイト順に並べ、末尾に 256 バイトの 0 を付けます。
"""
import argparse
import re
import struct
import sys

READING_RE = re.compile(r'^[ぁ-ゖー]+$')  # ひらがな + ー


def read_text(path):
    data = open(path, 'rb').read()
    for enc in ('utf-8', 'euc_jis_2004', 'euc_jp'):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            pass
    return data.decode('euc_jis_2004', errors='replace')


def parse_skk(text, table, order):
    for line in text.splitlines():
        if not line or line.startswith(';'):
            continue
        sp = line.find(' /')
        if sp <= 0:
            continue
        reading = line[:sp]
        if not READING_RE.match(reading):
            continue  # 送りあり・英字の項目は使わない
        for cand in line[sp + 2:].split('/'):
            cand = cand.split(';', 1)[0].strip()   # 注釈を除く
            if not cand or '(' in cand or '[' in cand or '\t' in cand or '\n' in cand:
                continue                            # Lisp式などは除く
            lst = table.setdefault(reading, [])
            if reading not in order:
                order[reading] = True
            if cand not in lst:
                lst.append(cand)


def main():
    ap = argparse.ArgumentParser(description='SKK辞書をCTRPF用に変換')
    ap.add_argument('inputs', nargs='+', help='SKK辞書ファイル (EUC-JP / UTF-8)')
    ap.add_argument('-o', '--output', default='skk_jisyo.dic', help='出力ファイル名')
    args = ap.parse_args()

    table, order = {}, {}
    for path in args.inputs:
        parse_skk(read_text(path), table, order)

    keys = sorted(table.keys(), key=lambda k: k.encode('utf-8'))
    lines = [(k + '\t' + '/'.join(table[k]) + '\n').encode('utf-8') for k in keys]

    header_size = 8 + 4 * len(lines)
    offsets, pos = [], header_size
    for ln in lines:
        offsets.append(pos)
        pos += len(ln)

    with open(args.output, 'wb') as f:
        f.write(b'SKKU' + struct.pack('<I', len(lines)))
        f.write(struct.pack('<%dI' % len(offsets), *offsets))
        for ln in lines:
            f.write(ln)
        f.write(b'\0' * 256)

    print('%d 語を書き出しました → %s (%.1f KB)' % (len(lines), args.output, pos / 1024))


if __name__ == '__main__':
    main()
