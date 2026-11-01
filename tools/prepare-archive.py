#!/usr/bin/env python3
"""Build checksum-pinned static ZIP/7z dependencies in ignored build storage."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / "build/deps"
SOURCES = (
    ("zlib-1.3.2", "https://zlib.net/fossils/zlib-1.3.2.tar.xz",
     "d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3"),
    ("xz-5.8.4", "https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.xz",
     "4ce24038fd4221e0d13bc1a2de7a4db56e90b92b3bf75321f6c14be73f65de4b"),
    ("libarchive-3.8.10", "https://www.libarchive.org/downloads/libarchive-3.8.10.tar.xz",
     "56cad765459d5ee25a3b761f90c47d99379d28297fbfb72d908880d6b875fb96"),
)


def source(name, url, digest):
    download = DEPS / "downloads" / (name + ".tar.xz")
    download.parent.mkdir(parents=True, exist_ok=True)
    if not download.is_file():
        with urllib.request.urlopen(url, timeout=60) as response:
            content = response.read(32 * 1024 * 1024 + 1)
        if hashlib.sha256(content).hexdigest() != digest:
            raise SystemExit(f"Checksum mismatch: {name}")
        download.write_bytes(content)
    if hashlib.sha256(download.read_bytes()).hexdigest() != digest:
        raise SystemExit(f"Checksum mismatch: {download}")
    directory = DEPS / name
    # Restore patched files from the verified tarball for every rebuild.
    with tarfile.open(download) as package:
        if not directory.is_dir():
            package.extractall(DEPS, filter="data")
        elif name.startswith("libarchive-"):
            for relative in ("libarchive/archive_read_support_format_7zip.c",
                             "libarchive/archive_read_support_format_zip.c"):
                package.extract(name + "/" + relative, DEPS, filter="data")
    return directory


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise SystemExit("The pinned libarchive decoder-limits patch no longer applies.")
    return text.replace(old, new, 1)


def limit_decoders(directory):
    # Caps before allocations/decompression inside next_header(), where the
    # application's streaming budget cannot act. See docs/archive-support.md.
    path = directory / "libarchive/archive_read_support_format_7zip.c"
    text = path.read_text()
    text = replace_once(text, "ARCHIVE_LITERAL_LL(100000000)", "ARCHIVE_LITERAL_LL(4096)")
    text = replace_once(text,
        "\t\tr = lzma_raw_decoder(&(zip->lzstream), filters);",
        "\t\t/* Kega Refusion: bound dictionaries before decoder allocation. */\n"
        "\t\tif (lzma_raw_decoder_memusage(filters) > (128ULL << 20)) {\n"
        "\t\t\tfree(ff->options);\n"
        "\t\t\tarchive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,\n"
        '\t\t\t    "7-Zip dictionary exceeds 128 MiB limit");\n'
        "\t\t\treturn (ARCHIVE_FAILED);\n\t\t}\n"
        "\t\tr = lzma_raw_decoder(&(zip->lzstream), filters);")
    text = replace_once(text,
        "\t\t\tif (parse_7zip_int64(a, &(folder->unPackSize[j])) < 0)\n"
        "\t\t\t\tgoto failed;",
        "\t\t\tif (parse_7zip_int64(a, &(folder->unPackSize[j])) < 0)\n"
        "\t\t\t\tgoto failed;\n"
        "\t\t\tif (folder->unPackSize[j] < 0 ||\n"
        "\t\t\t    folder->unPackSize[j] > (128LL << 20)) {\n"
        "\t\t\t\tarchive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,\n"
        '\t\t\t\t    "7-Zip block exceeds 128 MiB limit");\n'
        "\t\t\t\tgoto failed;\n\t\t\t}")
    text = replace_once(text, "\tif (next_header_size < 0) {",
                        "\tif (next_header_size < 0 || next_header_size > (8LL << 20)) {")
    text = replace_once(text,
        "\t\t\t\tzip->header_bytes_remaining =\n"
        "\t\t\t\t\tzip->folder_outbytes_remaining;",
        "\t\t\t\tif (zip->folder_outbytes_remaining > (8LL << 20)) {\n"
        "\t\t\t\t\tarchive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,\n"
        '\t\t\t\t\t    "7-Zip header exceeds 8 MiB limit");\n'
        "\t\t\t\t\tfree_StreamsInfo(&(zip->si));\n"
        "\t\t\t\t\treturn (ARCHIVE_FATAL);\n\t\t\t\t}\n"
        "\t\t\t\tzip->header_bytes_remaining =\n"
        "\t\t\t\t\tzip->folder_outbytes_remaining;")
    text = replace_once(text, "msize > PPMD7_MAX_MEM_SIZE",
                        "msize > (128U << 20)")
    path.write_text(text)
    path = directory / "libarchive/archive_read_support_format_zip.c"
    text = path.read_text()
    text = replace_once(text,
        "lzma_stream_decoder(&zip->zipx_lzma_stream, UINT64_MAX, 0)",
        "lzma_stream_decoder(&zip->zipx_lzma_stream, 128ULL << 20, 0)")
    text = replace_once(text,
        "lzma_alone_decoder(&zip->zipx_lzma_stream, 576 * ((uint64_t)1 << 20))",
        "lzma_alone_decoder(&zip->zipx_lzma_stream, 128ULL << 20)")
    text = replace_once(text, "if(order < 2 || restore_method > 2)",
                        "if(order < 2 || restore_method > 2 || mem > 128)")
    text = replace_once(text, "\tunsigned found;\n\tint64_t correction;",
        "\tunsigned found;\n\tsize_t kega_entries = 0, kega_metadata = 0;\n\tint64_t correction;")
    text = replace_once(text, "\t\tzip_entry = calloc(1, sizeof(struct zip_entry));",
        "\t\tif (++kega_entries > 4096) {\n"
        "\t\t\tarchive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,\n"
        '\t\t\t    "ZIP exceeds 4096 entry limit");\n'
        "\t\t\treturn ARCHIVE_FATAL;\n\t\t}\n"
        "\t\tzip_entry = calloc(1, sizeof(struct zip_entry));")
    text = replace_once(text,
        "\t\tfilename_length = archive_le16dec(p + 28);\n"
        "\t\textra_length = archive_le16dec(p + 30);",
        "\t\tfilename_length = archive_le16dec(p + 28);\n"
        "\t\textra_length = archive_le16dec(p + 30);\n"
        "\t\tkega_metadata += 46 + filename_length + extra_length + archive_le16dec(p + 32);\n"
        "\t\tif (kega_metadata > (8U << 20)) {\n"
        "\t\t\tarchive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,\n"
        '\t\t\t    "ZIP directory exceeds 8 MiB limit");\n'
        "\t\t\treturn ARCHIVE_FATAL;\n\t\t}")
    path.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    args = parser.parse_args()
    prefix = DEPS / "archive" / args.platform
    stamp = prefix / "build-id"
    zlib_name = "libzs.a" if args.platform == "windows" else "libz.a"
    libraries = ("libarchive.a", "liblzma.a", zlib_name)
    identity = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    if stamp.is_file() and stamp.read_text() == identity and all(
            (prefix / "lib" / name).is_file() for name in libraries):
        print(prefix)
        return
    directories = {name: source(name, url, digest) for name, url, digest in SOURCES}
    limit_decoders(directories["libarchive-3.8.10"])
    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
              "-DCMAKE_INSTALL_PREFIX=" + str(prefix), "-DCMAKE_INSTALL_LIBDIR=lib",
              "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTING=OFF"]
    if args.platform == "windows":
        common += ["-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "cmake/mingw-w64-x86_64.cmake")]
    def build(name, options):
        output = DEPS / "archive-build" / args.platform / name
        subprocess.run(["cmake", "-S", str(directories[name]), "-B", str(output)]
                       + common + options, check=True)
        subprocess.run(["cmake", "--build", str(output), "-j", str(min(os.cpu_count() or 2, 12))],
                       check=True)
        subprocess.run(["cmake", "--install", str(output)], check=True)
    build("zlib-1.3.2", ["-DZLIB_BUILD_SHARED=OFF", "-DZLIB_BUILD_STATIC=ON",
                       "-DZLIB_BUILD_TESTING=OFF"])
    build("xz-5.8.4", ["-DXZ_NLS=OFF", "-DXZ_THREADS=no",
        "-DXZ_TOOL_XZ=OFF", "-DXZ_TOOL_XZDEC=OFF", "-DXZ_TOOL_LZMADEC=OFF",
        "-DXZ_TOOL_LZMAINFO=OFF", "-DXZ_DOC=OFF"])
    disabled = ("OPENSSL", "MBEDTLS", "NETTLE", "LIBB2", "LZ4", "LZO", "ZSTD",
                "BZip2", "LIBXML2", "BSDXML", "EXPAT", "WIN32_XMLLITE",
                "PCREPOSIX", "PCRE2POSIX", "CNG", "TAR", "CPIO", "CAT", "UNZIP",
                "XATTR", "ACL", "ICONV", "TEST")
    build("libarchive-3.8.10", ["-DENABLE_" + name + "=OFF" for name in disabled] + [
        "-DZLIB_INCLUDE_DIR=" + str(prefix / "include"),
        "-DZLIB_LIBRARY=" + str(prefix / "lib" / zlib_name),
        "-DLIBLZMA_INCLUDE_DIR=" + str(prefix / "include"),
        "-DLIBLZMA_LIBRARY=" + str(prefix / "lib/liblzma.a")])
    for name in libraries:
        if not (prefix / "lib" / name).is_file():
            raise SystemExit(f"Missing dependency build product: {name}")
    stamp.write_text(identity)
    print(prefix)


if __name__ == "__main__":
    main()
