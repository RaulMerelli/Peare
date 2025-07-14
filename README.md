# Peare

*Plugin-Extendable Avdanced Resource Editor*

Inspired by Resource Hacker and BCC Workshop, wrote in C#.

The goal is to open, edit, save the resources inside an executable or an application package, and expand the resources type support via plugins made for this software. 
Current focus is on Windows 16/32/64bit and OS/2 16/32bit.

In future I would also like to support other application packages, like jar, apk, appx, xap, msi, msix... Everything containing something deserves to be opened and understood.

What is currently (at least partially) implemented right now:

- Open file and list types and resources
	- NE New Executable (Windows 16bit)
	- NE New Executable(OS/2 16bit)
	- PE Portable Executable (Windows 32/64bit)
	- LX Linear Executable Extended (OS/2 32bit)

- Open resources from the list:

	- NE (OS/2)
		- RT_POINTER (BA, BM, IC, CI)
		- RT_MESSAGE
		- RT_BITMAP (BA, BM, IC, CI)
		- RT_STRING
		- RT_DISPLAYINFO

	- LX (OS/2)
		- RT_POINTER (BA, BM, IC, CI)
		- RT_MESSAGE
		- RT_STRING
		- RT_BITMAP (BA, BM, IC, CI)
		- RT_DISPLAYINFO (theoretically, I can't find any Windows LX contining a RT_DISPLAYINFO resource)

	- NE (Windows)
		- RT_MESSAGETABLE (theoretically, I can't find any Windows NE contining a RT_MESSAGETABLE resource)
		- RT_FONTDIR
		- RT_FONT
		- RT_GROUP_ICON
		- RT_ICON
		- RT_BITMAP
		- RT_STRING
		- RT_VERSION

	- PE (Windows)
		- RT_BITMAP

	- Fallback to ASCII text and raw bytes (NE for Windows and OS/2, LX)



RT_FONT support for NE Windows is handcrafted, this is what is supported:

- Fnt ver. 1
	- monospace raster fonts
	- variable width raster fonts
	- vectors fonts
- Fnt ver. 2
	- monospace raster fonts
	- Fvariable width raster fonts
- Fnt ver. 3
	- monospace raster fonts
