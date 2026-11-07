# Intel HEX Fixtures

GNU Binutils `objcopy` 2.46.0.20260210 created the samples from the project
`README.md`. The commands created address-zero, `0x10000` segment-address, and
`0x100000` linear-address samples, and an address-near-limit sample:

```text
objcopy -I binary -O ihex -B i386 README.md README-low-address.hex
objcopy -I binary -O ihex -B i386 --change-addresses=0x10000 README.md README-extended-segment.hex
objcopy -I binary -O ihex -B i386 --change-addresses=0x100000 README.md README-extended-linear.hex
objcopy -I binary -O ihex -B i386 --change-addresses=0xFFFF0000 README.md README-top-address.hex
```

The sparse sample has two `README.md` segments, at address zero and `0x100000`.
Binutils built its ELF input from the same README, added the second section, and
set that section's load flags before it wrote Intel HEX:

```text
objcopy -I binary -O elf64-x86-64 -B i386:x86-64 README.md sparse.o
objcopy --add-section .high=README.md --change-section-vma .high=0x100000 sparse.o sparse.elf
objcopy --set-section-flags .high=alloc,load,contents,data sparse.elf sparse-load.elf
objcopy -O ihex sparse-load.elf README-sparse-segments.hex
objcopy -I ihex -O binary README-sparse-segments.hex sparse.bin
```

Binutils rebuilt each sample. It filled the gap in the sparse binary output
with zeros.
