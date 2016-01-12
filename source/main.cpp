/* REminiscence - Flashback interpreter
 * Copyright (C) 2005-2011 Gregory Montoir
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "file.h"
#include "fs.h"
#include "game.h"
#include "systemstub.h"

// 3DS Specific
#include <3ds.h>
#include <citro3d.h>

#include <new>

/* Not necessary on 3ds
 static const char *USAGE =
 "REminiscence - Flashback Interpreter\n"
 "Usage: %s [OPTIONS]...\n"
 "  --datapath=PATH   Path to data files (default 'DATA')\n"
 "  --savepath=PATH   Path to save files (default '.')\n"
 "  --levelnum=NUM    Starting level (default '0')";
 
 static bool parseOption(const char *arg, const char *longCmd, const char **opt) {
 bool handled = false;
 if (arg[0] == '-' && arg[1] == '-') {
 if (strncmp(arg + 2, longCmd, strlen(longCmd)) == 0) {
 *opt = arg + 2 + strlen(longCmd);
 handled = true;
 }
 }
 return handled;
 }
 */

static __attribute__ ((noinline)) int detectVersion(FileSystem *fs) {
	static const struct {
		const char *filename;
		int type;
		const char *name;
	} table[] = {
		{ "LEVEL1.MAP", kResourceTypePC, "PC" },
		{ "LEVEL1.LEV", kResourceTypeAmiga, "Amiga" },
		{ 0, -1 }
	};
	for (int i = 0; table[i].filename; ++i) {
		File f;
		if (f.open(table[i].filename, "rb", fs)) {
			debug(DBG_INFO, "Detected %s version", table[i].name);
			return table[i].type;
		}
	}
	return -1;
}

static __attribute__ ((noinline)) Language detectLanguage(FileSystem *fs) {
	static const struct {
		const char *filename;
		Language language;
	} table[] = {
		// PC
		{ "ENGCINE.TXT", LANG_EN },
		{ "FR_CINE.TXT", LANG_FR },
		{ "GERCINE.TXT", LANG_DE },
		{ "SPACINE.TXT", LANG_SP },
		// Amiga
		{ "FRCINE.TXT", LANG_FR },
		{ 0, LANG_EN }
	};
	for (int i = 0; table[i].filename; ++i) {
		File f;
		if (f.open(table[i].filename, "rb", fs)) {
			return table[i].language;
		}
	}
	return LANG_EN;
}

static __attribute__ ((noinline)) void waitForSelectAndQuit(const char* msg) {
  printf("%s\nPress SELECT to quit, press START to continue\n", msg);
	while (aptMainLoop()) {
    hidScanInput();
    const u32 kDown = hidKeysDown();
    if (kDown & KEY_SELECT)
      break;
    else if (kDown & KEY_START)
      return;

		// Flush and swap framebuffers
		gfxFlushBuffers();
		gfxSwapBuffers();

		//Wait for VBlank
		gspWaitForVBlank();
  }

  gfxExit();
  exit(0);
}

#undef main
int main(int argc, char *argv[]) {
	const char *dataPath = "DATA";
	const char *savePath = ".";
	// const char *levelNum = "0";
  
  /* Ignore option parsing for 3DS */
  /*
   for (int i = 1; i < argc; ++i) {
   bool opt = false;
   
   if (strlen(argv[i]) >= 2) {
   opt |= parseOption(argv[i], "datapath=", &dataPath);
   opt |= parseOption(argv[i], "savepath=", &savePath);
   opt |= parseOption(argv[i], "levelnum=", &levelNum);
   }
   
   if (!opt) {
   printf(USAGE, argv[0]);
   return 0;
   }
   }
   */

  // Initialize graphics because SystemStub is not created yet.
  gfxInitDefault();
  consoleInit(GFX_BOTTOM, NULL);
  
	g_debugMask = DBG_INFO; // DBG_CUT | DBG_VIDEO | DBG_RES | DBG_MENU | DBG_PGE | DBG_GAME | DBG_UNPACK | DBG_COL | DBG_MOD | DBG_SFX | DBG_FILE;
	FileSystem fs(dataPath);
  
	const int version = detectVersion(&fs);
	if (version == -1)
    waitForSelectAndQuit("Unable to find data files, check that all required files are present");
	
  Language language = detectLanguage(&fs);
	SystemStub* stub = SystemStub_THREEDS_Create();
  if (stub == 0)
    waitForSelectAndQuit("Failed to allocate SystemStub!");

  // Game used to fail here, because of a very small .text segment on Download 
  // Play. Fixed by using an additional XML.
	Game* game = new (std::nothrow) Game(stub, &fs, savePath, 0, (ResourceType)version, language);
  if (game == 0)
    waitForSelectAndQuit("Failed to allocate game!");

	game->run();
	delete game;
	delete stub;

  gfxExit();
	return 0;
}
