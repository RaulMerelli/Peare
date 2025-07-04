# Peare

Plugin-Extendable Avdanced Resource Editor

Inspired by Resource Hacker and BCC Workshop, wrote in C#.

The the goal is to open, edit, save the resources inside an executable or an application package, and expand the resources type support via plugins made for this software. 
Current focus is on Windows 16/32/64bit.

What is currently (at least partially) implemented right now:
- Open NE files (Windows 16bit)
- Open PE files (Windows 32/64bit)
- Open Bitmap resources (both NE/PE)
- Open Icon resources (NE)
- Open Fnt resources (NE)
- Fallback to bytes and text (NE)

Fnt support is handcrafted, this is what is supported:
- Fnt ver. 1 vectors fonts
- Fnt ver. 2 monospace raster fonts
- Fnt ver. 2 variable width raster fonts
- Fnt ver. 3 monospace raster fonts
