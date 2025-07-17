# Peare

*Plugin-Extendable Avdanced Resource Editor*

<img width="472" height="442" alt="Peare icon" src="https://github.com/user-attachments/assets/bab6314a-0ae0-4c13-a3a4-d46ee26b5ab8" />

Inspired by Resource Hacker and BCC Workshop, wrote in C#.

The goal is to open, edit, save the resources inside an executable or an application package, and expand the resources type support via plugins made for this software. 
Current focus is on Windows 16/32/64bit and OS/2 16/32bit.

In future I would also like to support other application packages, like jar, apk, appx, xap, msi, msix... Everything containing something deserves to be opened and understood.

What is currently (at least partially) implemented right now:

- Open file and list types and resources
	- NE New Executable (Windows 16bit)
	- NE New Executable (OS/2 16bit)
	- PE Portable Executable (Windows 32/64bit)
	- LX Linear Executable Extended (OS/2 32bit)

- Open resources from the list:

	- NE (OS/2)
		- RT_POINTER (BA, BM, IC, CI, CP, PT)
		- RT_MESSAGE
		- RT_BITMAP (BA, BM, IC, CI, CP, PT)
		- RT_STRING
		- RT_DISPLAYINFO
		- RT_MENU

	- LX (OS/2)
		- RT_POINTER (BA, BM, IC, CI, CP, PT)
		- RT_MESSAGE
		- RT_BITMAP (BA, BM, IC, CI, CP, PT)
		- RT_STRING
		- RT_DISPLAYINFO (theoretically, I can't find any OS/2 LX contining a RT_DISPLAYINFO resource)
		- RT_MENU

	- NE (Windows)
		- RT_MESSAGETABLE (theoretically, I can't find any Windows NE contining a RT_MESSAGETABLE resource)
		- RT_FONTDIR
		- RT_FONT
		- RT_GROUP_ICON
		- RT_ICON
		- RT_BITMAP
		- RT_STRING
		- RT_VERSION
		- RT_GROUP_CURSOR
		- RT_CURSOR
		- RT_MENU

	- PE (Windows)
		- RT_MESSAGETABLE
		- RT_STRING
		- RT_BITMAP
		- RT_GROUP_ICON
		- RT_ICON
		- RT_VERSION
		- RT_GROUP_CURSOR
		- RT_CURSOR
		- RT_MENU
		- RT_ACCELERATOR

	- Fallback to ASCII text and raw bytes (All the formats)



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


## Screenshots

Some examples of the software working at current status:

### LX RT_POINTER
![LX RT_POINTER](https://github.com/RaulMerelli/Peare/raw/main/Screenshots/LX%20RT_POINTER.png)

### NE RT_BITMAP
![NE RT_BITMAP](https://github.com/RaulMerelli/Peare/raw/main/Screenshots/NE%20RT_BITMAP.png)

### NE RT_FONT
![NE RT_FONT](https://github.com/RaulMerelli/Peare/raw/main/Screenshots/NE%20RT_FONT.png)

### PE RT_BITMAP
![PE RT_BITMAP](https://github.com/RaulMerelli/Peare/raw/main/Screenshots/PE%20RT_BITMAP.png)

### PE RT_MENU
![PE RT_MENU](https://github.com/RaulMerelli/Peare/raw/main/Screenshots/PE%20RT_MENU.png)


