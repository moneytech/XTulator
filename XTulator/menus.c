/*
  XTulator: A portable, open-source 80186 PC emulator.
  Copyright (C)2020 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifdef _WIN32

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "machine.h"
#include <Windows.h>
#include <SDL/SDL.h>
#include "config.h"
#include "modules/disk/biosdisk.h"
#include "utility.h"
#include "menus.h"

WNDPROC menus_oldProc;
MACHINE_t* menus_useMachine = NULL;

MENU_t menu_file[] = {
	{ TEXT("Soft &reset (Ctrl-Alt-Del)"), MENUS_ENABLED, MENUS_FUNCTION, (void*)menus_reset },
	{ TEXT(""), MENUS_ENABLED, MENUS_SEPARATOR, NULL },
	{ TEXT("E&xit"), MENUS_ENABLED, MENUS_FUNCTION, (void*)menus_exit },
	{ NULL }
};

MENU_t menu_disk[] = {
	{ TEXT("Change floppy 0..."), MENUS_ENABLED, MENUS_FUNCTION, (void*)menus_changeFloppy0 },
	{ TEXT("Change floppy 1..."), MENUS_ENABLED, MENUS_FUNCTION, (void*)menus_changeFloppy1 },
	{ TEXT(""), MENUS_ENABLED, MENUS_SEPARATOR, NULL },
	{ TEXT("Eject floppy 0"), MENUS_ENABLED, MENUS_FUNCTION, (void*)menus_ejectFloppy0 },
	{ TEXT("Eject floppy 1"), MENUS_ENABLED, MENUS_FUNCTION, (void*)menus_ejectFloppy1 },
	{ NULL }
};


LRESULT CALLBACK menus_wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	void (*func)();

	switch (msg) {
	case WM_COMMAND:
		func = (void*)wParam;
		if (func != NULL) {
			(*func)();
		}
		return(TRUE);
	}

	return(CallWindowProc(menus_oldProc, hwnd, msg, wParam, lParam));
}

int menus_buildMenu(HMENU* hmenuBar, wchar_t* title, MENU_t* menu) {
	HMENU hmenu;
	UINT i;

	hmenu = CreateMenu();
	i = 0;
	while (menu[i].title != NULL) {
		if (menu[i].type == MENUS_FUNCTION) {
			AppendMenuW(hmenu, MF_STRING, (UINT)menu[i].function, menu[i].title);
		}
		else if (menu[i].type == MENUS_SEPARATOR) {
			AppendMenuW(hmenu, MF_SEPARATOR, 0, NULL);
		}
		i++;
	}
	AppendMenuW(*hmenuBar, MF_POPUP, (UINT_PTR)hmenu, title);

	return 0;
}

int menus_init(HWND hwnd) {
	HMENU hmenuBar;

	hmenuBar = CreateMenu();

	if (menus_buildMenu(&hmenuBar, TEXT("File"), menu_file) < 0) {
		return -1;
	}
	if (menus_buildMenu(&hmenuBar, TEXT("Disk"), menu_disk) < 0) {
		return -1;
	}

	SetMenu(hwnd, hmenuBar);
	DrawMenuBar(hwnd);

	menus_oldProc = (WNDPROC)SetWindowLong(hwnd, GWL_WNDPROC, (LONG_PTR)menus_wndProc);

	return 0;
}

void menus_setMachine(MACHINE_t* machine) {
	menus_useMachine = machine;
}

//Below is all code for each menu item

void menus_exit() {
	running = 0;
}

void menus_openFloppyFile(uint8_t disk) {
	OPENFILENAME of_dlg;
	wchar_t filename[MAX_PATH + 1] = { 0 };

	memset(&of_dlg, 0, sizeof(of_dlg));
	of_dlg.lStructSize = sizeof(of_dlg);
	of_dlg.lpstrTitle = TEXT("Open disk image");
	of_dlg.hInstance = NULL;
	of_dlg.lpstrFile = filename;
	of_dlg.lpstrFilter = TEXT("Floppy disk images (*.img)\0*.img\0All files (*.*)\0*.*\0\0");
	of_dlg.nMaxFile = MAX_PATH;
	of_dlg.Flags = OFN_FILEMUSTEXIST | OFN_LONGNAMES;
	if (GetOpenFileName(&of_dlg)) {
		size_t size;
		char* filembs;
		size = wcstombs(NULL, of_dlg.lpstrFile, 0);
		filembs = (char*)malloc(size + 1);
		if (filembs == NULL) {
			return;
		}
		wcstombs(filembs, of_dlg.lpstrFile, size + 1);
		biosdisk_insert(&menus_useMachine->CPU, disk, filembs);
		free(filembs);
	}
}

void menus_changeFloppy0() {
	menus_openFloppyFile(0);
}

void menus_changeFloppy1() {
	menus_openFloppyFile(1);
}

void menus_ejectFloppy0() {
	biosdisk_eject(&menus_useMachine->CPU, 0);
}

void menus_ejectFloppy1() {
	biosdisk_eject(&menus_useMachine->CPU, 1);
}

void menus_reset() {
	menus_useMachine->KeyState.scancode = 0x1D; //ctrl
	menus_useMachine->KeyState.isNew = 1;
	i8259_doirq(&menus_useMachine->i8259, 1);
	utility_sleep(50);

	menus_useMachine->KeyState.scancode = 0x38; //alt
	menus_useMachine->KeyState.isNew = 1;
	i8259_doirq(&menus_useMachine->i8259, 1);
	utility_sleep(50);

	menus_useMachine->KeyState.scancode = 0x53; //delete
	menus_useMachine->KeyState.isNew = 1;
	i8259_doirq(&menus_useMachine->i8259, 1);
}

#endif
