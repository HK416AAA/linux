#!/usr/bin/env python3
"""Convert the Ubuntu userspace initramfs (cpio.gz) into an ext4 image and an
Android sparse image, verifying every stage so neither artefact can be
silently corrupted.

Pipeline and checks (every check is fatal):

  1. ``gzip -t`` on the cpio archive and a full listing of the archive.
  2. Extract the cpio into a scratch directory and record a filesystem
     manifest (type, mode, uid, gid, size, symlink target, device numbers).
  3. The archive listing must agree with the extracted tree, so a truncated
     or unreadable archive cannot slip through.
  4. ``mke2fs -d`` the extracted tree into a raw ext4 image.
  5. ``e2fsck -fn`` the image; any error fails the build.
  6. ``debugfs rdump`` the image back into a second directory and compare its
     manifest against the cpio one.
  7. ``diff -r`` the cpio tree and the ext4 tree, byte for byte.
  8. Encode the Android sparse image, decode it again in-process and require
     the decoded bytes to be identical to the raw image; when ``simg2img`` is
     available its output is compared as well.

Requires root for steps 2-6 (uid/gid and device nodes) plus cpio, mke2fs,
e2fsck and debugfs on the host.
"""

import argparse
import hashlib
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

SPARSE_MAGIC = 0xED26FF3A
CHUNK_TYPE_RAW = 0xCAC1
CHUNK_TYPE_FILL = 0xCAC2
CHUNK_TYPE_SKIP = 0xCAC3
SPARSE_HEADER = struct.Struct("<IHHHHIIII")
CHUNK_HEADER = struct.Struct("<HHII")
BLOCK_SIZE = 4096

MANIFEST_FORMAT = "{path}\t{type}\t{mode:o}\t{uid}\t{gid}\t{size}\t{extra}"


def run(cmd, **kwargs):
    kwargs.setdefault("check", True)
    return subprocess.run(cmd, **kwargs)


def sha256(path, chunk=1 << 20):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            data = handle.read(chunk)
            if not data:
                break
            digest.update(data)
    return digest.hexdigest()


def extract_cpio(cpio_path, dest):
    os.makedirs(dest, exist_ok=True)
    cmd = "gzip -dc {} | cpio -idm --quiet -n --no-absolute-filenames".format(
        "'" + os.path.abspath(cpio_path).replace("'", "'\\''") + "'"
    )
    result = subprocess.run(cmd, shell=True, cwd=dest, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr[-4000:])
        raise SystemExit("cpio extraction failed with {}".format(result.returncode))


def archive_entries(cpio_path):
    cmd = "gzip -dc {} | cpio -it --quiet -n".format(
        "'" + os.path.abspath(cpio_path).replace("'", "'\\''") + "'"
    )
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr[-4000:])
        raise SystemExit("cpio listing failed with {}".format(result.returncode))
    return [line for line in result.stdout.splitlines() if line.strip()]


def describe(path, root):
    st = os.lstat(path)
    rel = os.path.relpath(path, root)
    mode = st.st_mode
    fmt = stat.S_IFMT(mode)
    if stat.S_ISLNK(mode):
        extra = os.readlink(path)
        size = 0
    elif stat.S_ISCHR(mode) or stat.S_ISBLK(mode):
        extra = "{}:{}".format(os.major(st.st_rdev), os.minor(st.st_rdev))
        size = 0
    elif stat.S_ISREG(mode):
        extra = ""
        size = st.st_size
    else:
        extra = ""
        size = 0
    return MANIFEST_FORMAT.format(
        path=rel,
        type=fmt,
        mode=stat.S_IMODE(mode),
        uid=st.st_uid,
        gid=st.st_gid,
        size=size,
        extra=extra,
    )


def manifest(root, skip_top=()):
    entries = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        top = os.path.abspath(dirpath) == os.path.abspath(root)
        for name in dirnames + filenames:
            if top and name in skip_top:
                continue
            entries.append(describe(os.path.join(dirpath, name), root))
    entries.sort()
    return entries


def device_nodes(root):
    nodes = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        for name in dirnames + filenames:
            path = os.path.join(dirpath, name)
            st = os.lstat(path)
            if stat.S_ISCHR(st.st_mode) or stat.S_ISBLK(st.st_mode):
                nodes.append((os.path.relpath(path, root), st))
    return nodes


STAT_TYPE = {
    "character special": stat.S_IFCHR,
    "block special": stat.S_IFBLK,
}


def verify_devices(image, nodes, dest):
    """debugfs rdump drops device nodes, so check them with stat and recreate
    them in the dumped tree for the follow-up comparisons."""
    for rel, st in nodes:
        result = subprocess.run(
            ["debugfs", "-R", "stat /{}".format(rel), image],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise SystemExit("debugfs stat failed for /{}".format(rel))
        text = result.stdout
        match_type = re.search(r"Type:\s+([a-z ]+?)\s+Mode:\s+([0-7]+)", text)
        match_ids = re.search(r"User:\s+(\d+)\s+Group:\s+(\d+)", text)
        match_dev = re.search(
            r"Device major/minor number:\s+(\d+):(\d+)", text
        )
        if not (match_type and match_ids and match_dev):
            raise SystemExit("cannot parse debugfs stat for /{}".format(rel))
        kind, mode_text = match_type.group(1).strip(), match_type.group(2)
        expected_kind = "character special" if stat.S_ISCHR(st.st_mode) else "block special"
        if kind != expected_kind or STAT_TYPE.get(kind) != stat.S_IFMT(st.st_mode):
            raise SystemExit("device type mismatch for /{}: {}".format(rel, kind))
        if int(mode_text, 8) != stat.S_IMODE(st.st_mode):
            raise SystemExit("device mode mismatch for /{}".format(rel))
        if (int(match_ids.group(1)), int(match_ids.group(2))) != (st.st_uid, st.st_gid):
            raise SystemExit("device ownership mismatch for /{}".format(rel))
        if (int(match_dev.group(1)), int(match_dev.group(2))) != (
            os.major(st.st_rdev), os.minor(st.st_rdev)
        ):
            raise SystemExit("device number mismatch for /{}".format(rel))
        target = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        os.mknod(target, st.st_mode, os.makedev(os.major(st.st_rdev), os.minor(st.st_rdev)))
        os.chown(target, st.st_uid, st.st_gid)
    return len(nodes)


def compare_manifests(first, second, label_first, label_second, limit=20):
    only_first = sorted(set(first) - set(second))
    only_second = sorted(set(second) - set(first))
    if not only_first and not only_second:
        return
    print("manifest mismatch between {} and {}".format(label_first, label_second))
    for line in only_first[:limit]:
        print("  only in {}: {}".format(label_first, line))
    for line in only_second[:limit]:
        print("  only in {}: {}".format(label_second, line))
    raise SystemExit("manifest verification failed")


def tree_files(root, skip_top=()):
    entries = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        top = os.path.abspath(dirpath) == os.path.abspath(root)
        for name in dirnames + filenames:
            if top and name in skip_top:
                continue
            path = os.path.join(dirpath, name)
            st = os.lstat(path)
            rel = os.path.relpath(path, root)
            if stat.S_ISREG(st.st_mode):
                entries[rel] = ("file", st.st_size, sha256(path))
            elif stat.S_ISLNK(st.st_mode):
                entries[rel] = ("link", 0, os.readlink(path))
            else:
                entries[rel] = ("other", 0, "")
    return entries


def compare_payload(first, second, skip_top=(), limit=20):
    left = tree_files(first, skip_top)
    right = tree_files(second, skip_top)
    problems = []
    for rel in sorted(set(left) | set(right)):
        if left.get(rel) != right.get(rel):
            problems.append("{}\n    cpio: {}\n    ext4: {}".format(
                rel, left.get(rel), right.get(rel)))
    if problems:
        print("\n".join(problems[:limit]))
        raise SystemExit("cpio tree and ext4 tree differ")


def needed_size(root):
    total = 0
    count = 0
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            st = os.lstat(path)
            count += 1
            if stat.S_ISREG(st.st_mode):
                total += (st.st_size + BLOCK_SIZE - 1) // BLOCK_SIZE * BLOCK_SIZE
    overhead = max(64 << 20, count * 512)
    size = max(128 << 20, int(total * 1.10) + overhead)
    return (size + (4 << 20) - 1) // (4 << 20) * (4 << 20)


def build_ext4(root, image, label):
    size = needed_size(root)
    for attempt in range(4):
        blocks = size // BLOCK_SIZE
        if os.path.exists(image):
            os.unlink(image)
        try:
            run(
                [
                    "mke2fs", "-q", "-F", "-t", "ext4",
                    "-b", str(BLOCK_SIZE), "-I", "256", "-i", "8192", "-m", "0",
                    "-L", label,
                    "-E", "lazy_itable_init=0,lazy_journal_init=0",
                    "-O", "^has_journal",
                    "-d", root,
                    image, str(blocks),
                ],
                capture_output=True, text=True,
            )
            break
        except subprocess.CalledProcessError as exc:
            if attempt == 3:
                print((exc.stderr or "")[-4000:])
                raise
            size = size * 3 // 2
            print("ext4   : image too small, retrying with {} bytes".format(size))
    result = subprocess.run(["e2fsck", "-fn", image], capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout[-4000:])
        print(result.stderr[-2000:])
        raise SystemExit("e2fsck reported errors on the generated image")
    return size


def rdump(image, dest):
    os.makedirs(dest, exist_ok=True)
    result = subprocess.run(
        ["debugfs", "-R", "rdump / {}".format(dest), image],
        capture_output=True, text=True,
    )
    if result.returncode != 0 or "error" in result.stderr.lower():
        print(result.stdout[-2000:])
        print(result.stderr[-2000:])
        raise SystemExit("debugfs rdump failed")


def classify(block):
    if block == b"\0" * BLOCK_SIZE:
        return "skip", block
    if block == block[:1] * BLOCK_SIZE:
        return "fill", block[:4]
    return "raw", block


def encode_sparse(raw):
    padded = len(raw) % BLOCK_SIZE
    if padded:
        raw = raw + b"\0" * (BLOCK_SIZE - padded)
    total_blocks = len(raw) // BLOCK_SIZE
    chunks = []
    index = 0
    while index < total_blocks:
        kind, value = classify(raw[index * BLOCK_SIZE:(index + 1) * BLOCK_SIZE])
        count = 1
        if kind == "skip":
            while index + count < total_blocks and classify(
                raw[(index + count) * BLOCK_SIZE:(index + count + 1) * BLOCK_SIZE]
            )[0] == "skip":
                count += 1
            chunks.append((CHUNK_TYPE_SKIP, count, b""))
        elif kind == "fill":
            while index + count < total_blocks:
                next_kind, next_value = classify(
                    raw[(index + count) * BLOCK_SIZE:(index + count + 1) * BLOCK_SIZE]
                )
                if next_kind != "fill" or next_value != value:
                    break
                count += 1
            chunks.append((CHUNK_TYPE_FILL, count, value))
        else:
            while index + count < total_blocks and classify(
                raw[(index + count) * BLOCK_SIZE:(index + count + 1) * BLOCK_SIZE]
            )[0] == "raw":
                count += 1
            chunks.append((CHUNK_TYPE_RAW, count, raw[index * BLOCK_SIZE:(index + count) * BLOCK_SIZE]))
        index += count

    out = bytearray()
    out += SPARSE_HEADER.pack(
        SPARSE_MAGIC, 1, 0, SPARSE_HEADER.size, CHUNK_HEADER.size,
        BLOCK_SIZE, total_blocks, len(chunks), 0,
    )
    for chunk_type, blocks, payload in chunks:
        out += CHUNK_HEADER.pack(chunk_type, 0, blocks, CHUNK_HEADER.size + len(payload))
        out += payload
    return bytes(out), total_blocks


def decode_sparse(blob):
    magic, major, _minor, file_hdr, chunk_hdr, block_size, total_blocks, total_chunks, _sum = \
        SPARSE_HEADER.unpack_from(blob, 0)
    if magic != SPARSE_MAGIC or major != 1:
        raise SystemExit("bad sparse header")
    if file_hdr != SPARSE_HEADER.size or chunk_hdr != CHUNK_HEADER.size:
        raise SystemExit("unexpected sparse header sizes")
    if block_size != BLOCK_SIZE:
        raise SystemExit("unexpected sparse block size")
    offset = file_hdr
    blocks = []
    for _ in range(total_chunks):
        chunk_type, _reserved, chunk_blocks, total_size = CHUNK_HEADER.unpack_from(blob, offset)
        offset += chunk_hdr
        payload_size = total_size - chunk_hdr
        payload = blob[offset:offset + payload_size]
        offset += payload_size
        if chunk_type == CHUNK_TYPE_RAW:
            if payload_size != chunk_blocks * block_size:
                raise SystemExit("raw chunk size mismatch")
            blocks.append(payload)
        elif chunk_type == CHUNK_TYPE_FILL:
            if payload_size != 4:
                raise SystemExit("fill chunk size mismatch")
            blocks.append(payload * (chunk_blocks * block_size // 4))
        elif chunk_type == CHUNK_TYPE_SKIP:
            if payload_size != 0:
                raise SystemExit("skip chunk size mismatch")
            blocks.append(b"\0" * (chunk_blocks * block_size))
        else:
            raise SystemExit("unknown sparse chunk type {:#x}".format(chunk_type))
    if offset != len(blob):
        raise SystemExit("trailing bytes in sparse image")
    decoded = b"".join(blocks)
    if len(decoded) // block_size != total_blocks:
        raise SystemExit("sparse block count mismatch")
    return decoded


def verify_sparse(raw_path, sparse_path):
    with open(raw_path, "rb") as handle:
        raw = handle.read()
    with open(sparse_path, "rb") as handle:
        blob = handle.read()
    decoded = decode_sparse(blob)
    if decoded[:len(raw)] != raw:
        raise SystemExit("in-process sparse round trip differs from the ext4 image")
    if any(decoded[len(raw):]):
        raise SystemExit("sparse padding is not zero")
    simg2img = shutil.which("simg2img")
    if simg2img:
        handle, decoded_path = tempfile.mkstemp(prefix="simg2img.")
        os.close(handle)
        try:
            run([simg2img, sparse_path, decoded_path], capture_output=True)
            with open(decoded_path, "rb") as fd:
                external = fd.read()
            if external[:len(raw)] != raw:
                raise SystemExit("simg2img decoded bytes differ from the ext4 image")
        finally:
            os.unlink(decoded_path)
        print("sparse : cross-checked with simg2img")
    else:
        print("sparse : simg2img not installed, in-process round trip only")


def main():
    parser = argparse.ArgumentParser(description="cpio.gz -> ext4 -> Android sparse")
    parser.add_argument("--cpio", required=True, help="userspace initramfs (.cpio.gz)")
    parser.add_argument("--raw-out", required=True, help="raw ext4 image output path")
    parser.add_argument("--sparse-out", required=True, help="Android sparse image output path")
    parser.add_argument("--label", default="charlotte-root", help="filesystem label")
    parser.add_argument("--workdir", default=None, help="scratch directory")
    args = parser.parse_args()

    if os.geteuid() != 0:
        raise SystemExit("run as root so uid/gid and device nodes survive the round trip")

    cpio = os.path.abspath(args.cpio)
    raw = os.path.abspath(args.raw_out)
    sparse = os.path.abspath(args.sparse_out)
    for path in (raw, sparse):
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)

    print("cpio   : {} ({} bytes)".format(cpio, os.path.getsize(cpio)))
    run(["gzip", "-t", cpio])
    listed = archive_entries(cpio)
    print("cpio   : gzip ok, {} archive entries".format(len(listed)))

    work = args.workdir or tempfile.mkdtemp(prefix="cpio2ext4.")
    os.makedirs(work, exist_ok=True)
    tree = os.path.join(work, "cpio-tree")
    dumped = os.path.join(work, "ext4-dump")
    for path in (tree, dumped):
        if os.path.exists(path):
            shutil.rmtree(path)

    extract_cpio(cpio, tree)
    tree_manifest = manifest(tree)
    if len(tree_manifest) + 1 < len(listed):
        raise SystemExit(
            "cpio listing has {} entries but only {} were extracted".format(
                len(listed), len(tree_manifest)
            )
        )
    print("cpio   : {} filesystem entries, listing agrees".format(len(tree_manifest)))

    size = build_ext4(tree, raw, args.label)
    print("ext4   : {} bytes, e2fsck clean".format(size))

    rdump(raw, dumped)
    nodes = device_nodes(tree)
    if nodes:
        checked = verify_devices(raw, nodes, dumped)
        print("ext4   : {} device nodes verified".format(checked))
    skip_top = ()
    if not any(entry == "lost+found" or entry.startswith("lost+found/")
               for entry in tree_manifest):
        skip_top = ("lost+found",)
    dump_manifest = manifest(dumped, skip_top=skip_top)
    compare_manifests(tree_manifest, dump_manifest, "cpio", "ext4")
    print("ext4   : metadata matches the cpio")
    compare_payload(tree, dumped, skip_top=skip_top)
    print("ext4   : payload matches the cpio")

    with open(raw, "rb") as handle:
        blob, total_blocks = encode_sparse(handle.read())
    with open(sparse, "wb") as handle:
        handle.write(blob)
    verify_sparse(raw, sparse)

    info = [
        "cpio_sha256={}".format(sha256(cpio)),
        "cpio_entries={}".format(len(tree_manifest)),
        "raw_sha256={}".format(sha256(raw)),
        "raw_size={}".format(os.path.getsize(raw)),
        "sparse_sha256={}".format(sha256(sparse)),
        "sparse_size={}".format(os.path.getsize(sparse)),
        "sparse_blocks={}".format(total_blocks),
        "label={}".format(args.label),
    ]
    with open(sparse + ".info.txt", "w") as handle:
        handle.write("\n".join(info) + "\n")
    print("\n".join(info))
    print("done: {}".format(sparse))


if __name__ == "__main__":
    main()
