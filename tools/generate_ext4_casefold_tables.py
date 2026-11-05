import argparse
import zipfile
from pathlib import Path


def parse_ucd(archive):
    with zipfile.ZipFile(archive) as source:
        unicode_data = source.read("UnicodeData.txt").decode("ascii")
        case_folding = source.read("CaseFolding.txt").decode("utf-8")
    decompositions = {}
    combining_classes = {}
    for line in unicode_data.splitlines():
        fields = line.split(";")
        codepoint = int(fields[0], 16)
        combining = int(fields[3])
        decomposition = fields[5]
        if combining:
            combining_classes[codepoint] = combining
        if decomposition and not decomposition.startswith("<"):
            decompositions[codepoint] = [int(value, 16)
                                         for value in decomposition.split()]
    folds = {}
    for line in case_folding.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        codepoint, status, mapping = [field.strip() for field in line.split(";")[:3]]
        if status in ("C", "F"):
            current = folds.get(int(codepoint, 16))
            if current is None or status == "F":
                folds[int(codepoint, 16)] = [int(value, 16)
                                             for value in mapping.split()]
    return decompositions, combining_classes, folds


def emit_mapping(name, mapping, output):
    values = []
    rows = []
    for codepoint, sequence in sorted(mapping.items()):
        rows.append((codepoint, len(values), len(sequence)))
        values.extend(sequence)
    output.append(f"static const uint32_t {name}_values[] = {{")
    output.extend(",".join(f"0x{value:x}" for value in values[i:i + 12]) + ","
                  for i in range(0, len(values), 12))
    output.append("};")
    output.append(f"static const struct ext4_unicode_mapping {name}[] = {{")
    output.extend(f"{{0x{codepoint:x}, {offset}, {length}}},"
                  for codepoint, offset, length in rows)
    output.append("};")
    output.append(f"#define {name.upper()}_COUNT {len(rows)}")


def emit(archive, target):
    decompositions, combining_classes, folds = parse_ucd(archive)
    lines = [
        "/* Generated from Unicode Character Database 12.1.0. */",
        "/* See src/writer/third_party/lwext4/UNICODE-LICENSE.txt. */",
        "#ifndef EXT4_CASEFOLD_DATA_H_",
        "#define EXT4_CASEFOLD_DATA_H_",
        "",
        "struct ext4_unicode_mapping { uint32_t codepoint; uint32_t offset; uint8_t length; };",
        "struct ext4_unicode_ccc { uint32_t codepoint; uint8_t value; };",
    ]
    emit_mapping("ext4_unicode_decomposition", decompositions, lines)
    emit_mapping("ext4_unicode_casefold", folds, lines)
    lines.append("static const struct ext4_unicode_ccc ext4_unicode_ccc_table[] = {")
    lines.extend(f"{{0x{codepoint:x}, {value}}},"
                 for codepoint, value in sorted(combining_classes.items()))
    lines.extend([
        "};",
        f"#define EXT4_UNICODE_CCC_COUNT {len(combining_classes)}",
        "#endif",
    ])
    Path(target).write_text("\n".join(lines) + "\n", encoding="ascii")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    emit(args.archive, args.output)


if __name__ == "__main__":
    main()
