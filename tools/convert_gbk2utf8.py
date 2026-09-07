#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
convert_gbk2utf8.py - GB2312/GBK/GB18030 source files -> UTF-8
==============================================================
原因背景:
  VSCode / Claude Code / eIDE 只写 UTF-8; 而 Keil 生态(串口助手/老例程)是 GBK(ANSI/码页936)。
  工程里 GBK 文件一旦被按 UTF-8 读-改-写回, 中文注释会变成永久乱码(不可逆)。
  本脚本把 GBK 家族文件整体转成 UTF-8, 让"文件编码"和"编辑工具链"统一, 从根上消除该问题。

检测规则:
  1. 能按 UTF-8 严格解码           -> 已经是 UTF-8/纯 ASCII, 跳过(不动, 无 BOM 也不加)
  2. 不能, 但能按 GBK 严格解码     -> 转 UTF-8 (GBK 是 GB2312 的超集, 按 GBK 解即可覆盖 GB2312 内容)
  3. 也不能, 能按 GB18030 严格解码 -> 转 UTF-8 (兜底, 覆盖生僻字)
  4. 都不行                        -> 报告为 unknown, 跳过, 人工检查(可能有 BOM/损坏/其它编码)

安全措施:
  * 默认 dry-run 只打印清单, 加 --apply 才真正写盘
  * 写盘是"临时文件 + 原子替换"(os.replace), 中途失败不会弄坏原文件
  * 换行符(CRLF/LF)原样保留; 只转换需要转的文件, ASCII/UTF-8 文件一个字节都不碰
  * 建议转码前先 git commit, 转完用 git diff 验收(应只见中文注释行变化)

用法:
  python tools/convert_gbk2utf8.py                       # dry-run: 列出工程内所有待转文件
  python tools/convert_gbk2utf8.py --only api,user       # dry-run: 只看 api/ 和 user/ 目录
  python tools/convert_gbk2utf8.py --root D:/myproj --apply   # 真正执行转换
  python tools/convert_gbk2utf8.py --apply --verbose     # 执行并显示所有被跳过的文件
"""
import argparse
import os
import tempfile
import time

SKIP_DIRS = {
    ".git", ".pack", ".eide", ".vscode", ".build", ".vs", "__pycache__",
    "Objects", "Listings", "Debug", "Output", "build",
}
DEFAULT_EXTS = (".c", ".h", ".cpp", ".hpp", ".s", ".asm", ".S", ".inc")


def detect_encoding(data: bytes):
    """返回 None(已是 UTF-8/ASCII) / 'gbk' / 'gb18030' / 'unknown'"""
    try:
        data.decode("utf-8")
        return None
    except UnicodeDecodeError:
        pass
    for enc in ("gbk", "gb18030"):  # gb18030 是 gbk 的超集, 兜底用
        try:
            data.decode(enc)
            return enc
        except UnicodeDecodeError:
            pass
    return "unknown"


def convert_file(path: str, src_enc: str) -> None:
    """把 path 从 src_enc 转为 UTF-8, 原子替换原文件"""
    with open(path, "rb") as f:
        raw = f.read()
    text = raw.decode(src_enc)          # 解码; CRLF 在这里原样保留在 text 里
    new = text.encode("utf-8")          # 重新编码为 UTF-8
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), suffix=".conv.tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(new)
        os.replace(tmp, path)           # 原子替换: 要么成功要么原文件不变
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def main():
    ap = argparse.ArgumentParser(
        description="Convert GB2312/GBK/GB18030 source files to UTF-8. "
                    "Dry-run by default; pass --apply to write.")
    ap.add_argument("--root", default=None,
                    help="directory to scan (default: current working directory)")
    ap.add_argument("--only", default="",
                    help="comma-separated top-level dirs to restrict scan, e.g. api,user")
    ap.add_argument("--ext", default=",".join(DEFAULT_EXTS),
                    help="file extensions to consider")
    ap.add_argument("--apply", action="store_true",
                    help="actually write files (default: dry-run)")
    ap.add_argument("--verbose", action="store_true",
                    help="also list files already utf-8/ascii")
    args = ap.parse_args()

    exts = {e.strip().lower().lstrip(".") for e in args.ext.split(",") if e.strip()}
    only = {d.strip() for d in args.only.split(",") if d.strip()}

    root = args.root if args.root is not None else os.getcwd()
    if not os.path.isdir(root):
        print("[error] root not found:", root)
        return 1

    converted = skipped = unknown = failed = 0
    t0 = time.time()

    for dp, dirs, fns in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        rel_dir = os.path.relpath(dp, root)
        if only:
            if rel_dir == ".":
                # 根目录: 只进入允许的子目录
                dirs[:] = [d for d in dirs if d in only]
            elif rel_dir.split(os.sep)[0] not in only:
                dirs[:] = []
                continue
        for fn in sorted(fns):
            if os.path.splitext(fn)[1].lower().lstrip(".") not in exts:
                continue
            path = os.path.join(dp, fn)
            rel = os.path.relpath(path, root)
            with open(path, "rb") as f:
                data = f.read()
            src = detect_encoding(data)

            if src is None:
                skipped += 1
                if args.verbose:
                    print("[skip utf8] ", rel)
            elif src == "unknown":
                unknown += 1
                print("[?? unknown] ", rel)
            else:
                converted += 1
                if args.apply:
                    try:
                        convert_file(path, src)
                        print("[converted]  ", rel, "(%s -> utf-8)" % src)
                    except Exception as e:
                        failed += 1
                        print("[FAILED]     ", rel, "->", e)
                else:
                    print("[to convert] ", rel, "(%s -> utf-8)" % src)

    print("-" * 60)
    if args.apply:
        print("done: %d converted, %d already utf-8/ascii, %d unknown, %d failed "
              "(%.2fs)" % (converted, skipped, unknown, failed, time.time() - t0))
        print("verify now:  git diff --stat && git diff")
    else:
        print("DRY-RUN only, nothing written: %d would convert, %d already utf-8/ascii, "
              "%d unknown (%.2fs)" % (converted, skipped, unknown, time.time() - t0))
        print("1) git add -A && git commit   # backup before writing")
        print("2) python tools/convert_gbk2utf8.py --apply")
    return 0


if __name__ == "__main__":
    sys_exit = main()
    raise SystemExit(sys_exit if sys_exit is not None else 0)
