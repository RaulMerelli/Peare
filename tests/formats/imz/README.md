# WinImage IMZ samples

IMZ uses a ZIP container with one raw floppy image. WinImage documents IMZ as
a ZIP-compatible compressed image format.

The local validation sample uses the real FreeDOS 1.4 image from
`tests/formats/floppy/fixtures/FD14-Floppy.zip`. 7-Zip 19.00 extracted that
image and created the ZIP-compatible `.imz` container. No disk-image bytes
were authored or changed.

Set `PEARE_IMZ_SAMPLE` to the resulting `.imz` file and
`PEARE_IMZ_RAW_SAMPLE` to the extracted `.ima` file to run the ABI test.
The sample files stay outside the repository.

Source: https://www.freedos.org/
Format reference: https://www.winimage.com/htm/winimage.htm
