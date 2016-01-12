/**
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2011 Gregory Montoir
 * Copyright (C) 2016 Romulo Fernandes (systemstub_3ds.cpp)
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

#include "scaler.h"
#include "systemstub.h"

// 3DS Specific
#include <3ds.h>
#include <citro3d.h>

#include <iostream>
#include <sstream>
#include <fstream>
#include <new>

#define PAL_MAX_SIZE 257
// #define _DEBUG
#ifdef _DEBUG
#  define vAssert(X, Y) \
    do { \
      if ((X) == false) {\
        std::stringstream str;\
        str << Y;\
        printf("%s\n", str.str().c_str());\
        while (aptMainLoop()) {\
          hidScanInput();\
          const u32 kDown = hidKeysDown();\
          if (kDown & KEY_SELECT)\
            break;\
          else if (kDown & KEY_START)\
            return;\
        gfxFlushBuffers();\
        gfxSwapBuffers();\
        gspWaitForVBlank();\
        }\
        gfxExit();\
        exit(0);\
      }\
    } while( false );
#else
#  define vAssert(X,Y)
#endif
  
struct SystemStub_THREEDS : SystemStub {
	enum {
		MAX_BLIT_RECTS = 200,
		SOUND_SAMPLE_RATE = 22050,
		JOYSTICK_COMMIT_VALUE = 3200
	};

  enum keyBindingTarget {
    KBT_NONE = 0,
    KBT_BACKSPACE,
    KBT_ENTER,
    KBT_SHIFT,
    KBT_SPACE, 

    KBT_MAX_TARGETS
  };

  // MUST match keyBindingTarget order.
  char const* m_commandNames[KBT_MAX_TARGETS];

  enum keyIndices {
    KI_KEY_A = 0,
    KI_KEY_B,
    KI_KEY_SELECT,
    KI_KEY_START,
    KI_KEY_DRIGHT,
    KI_KEY_DLEFT,
    KI_KEY_DUP,
    KI_KEY_DDOWN,
    KI_KEY_R,
    KI_KEY_L,
    KI_KEY_X,
    KI_KEY_Y,
    KI_KEY_ZL,
    KI_KEY_ZR,

    KI_KEY_MAX_KEYS
  };

  struct AudioCore {
    bool quitSoundThread;
    bool playing;

    Handle mutex;

    AudioCallback callback;
    void* callbackParam;
    
    Thread threadHandle;
    u8* buffer;
  };

  // Is game paused?
  bool m_paused;

  // Key bindings
  u16 m_keyBindings[KI_KEY_MAX_KEYS];

  // Register when game started
  u64 m_startTick;

  // Palette emulation.
  u8* m_palette;

  // Indexed colors on framebuffer.
  u8* m_screenBufferPtr;

  // Overscan color used
  u8 m_overScanColor;

  // Lookup table for full screen. Index in Framebuffer => Index in original FB
  size_t* m_fullscreenLUT;

  // Full screen or centered?
  bool m_fullScreen;

  // Initialized screen size
  unsigned int m_screenWidth;
  unsigned int m_screenHeight;

  // Audio core 
  AudioCore m_audioCore;

	virtual ~SystemStub_THREEDS() {}
	virtual void init(const char *title, int w, int h);
	virtual void destroy();
	virtual void setPalette(const uint8 *pal, int n);
	virtual void setPaletteEntry(int i, const Color *c);
	virtual void getPaletteEntry(int i, Color *c);
	virtual void setOverscanColor(int i);
	virtual void copyRect(int x, int y, int w, int h, const uint8 *buf, int pitch);
	virtual void fadeScreen();
	virtual void updateScreen(int shakeOffset);
	virtual void processEvents();
	virtual void sleep(int duration);
	virtual uint32 getTimeStamp();
	virtual void startAudio(AudioCallback callback, void *param);
	virtual void stopAudio();
	virtual uint32 getOutputSampleRate();
	virtual void lockAudio();
	virtual void unlockAudio();

  // Render options menu.
  void loadOptions();
  void saveOptions();
  void renderOptionsText(int selectedIndex);
  void renderOptions();
  void selectOption(int selectedIndex);
};

SystemStub *SystemStub_THREEDS_Create() {
	return new (std::nothrow) SystemStub_THREEDS();
}

void SystemStub_THREEDS::init(const char *title, int width, int height) {
  m_startTick = svcGetSystemTick();
  m_screenWidth = width;
  m_screenHeight = height;
  m_fullScreen = false;
  m_paused = false;

  // Initialize top screen as double buffer / RGBA8
  gfxSetDoubleBuffering(GFX_TOP, true);
  gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);

  // Cleanup palette to black
  m_palette = new (std::nothrow) u8[PAL_MAX_SIZE * 3];
  for (int i = 0; i < PAL_MAX_SIZE * 3; ++i)
    m_palette[i] = 0;

  // Configure overscan color
  m_overScanColor = 0;

  // Configure command names
  for (int i = 0; i < KBT_MAX_TARGETS; ++i) {
    switch (i) {
    case KBT_NONE:
      m_commandNames[i] = "No binding";
      break;
    case KBT_SPACE:
      m_commandNames[i] = "Space";
      break;
    case KBT_BACKSPACE:
      m_commandNames[i] = "Backspace";
      break;
    case KBT_SHIFT:
      m_commandNames[i] = "Shift";
      break;
    case KBT_ENTER:
      m_commandNames[i] = "Enter";
      break;
    }
  }

  // Cleanup audio core
  memset(&m_audioCore, 0, sizeof(AudioCore));

  // Initialize player input
	memset(&_pi, 0, sizeof(_pi));

  // Default config
  memset(m_keyBindings, 0, sizeof(u16) * KI_KEY_MAX_KEYS);
  m_keyBindings[KI_KEY_Y] = KBT_BACKSPACE;
  m_keyBindings[KI_KEY_B] = KBT_ENTER;
  m_keyBindings[KI_KEY_A] = KBT_SHIFT;
  m_keyBindings[KI_KEY_X] = KBT_SPACE;

  // Allocate screen buffer.
  u16 fbWidth, fbHeight;
  u8* framebufferPtr = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbWidth, &fbHeight);
    
  vAssert(fbWidth > 0, "fbWidth > 0");
  vAssert(fbHeight > 0, "fbHeight > 0");

  if (framebufferPtr != 0) {
    vAssert(m_screenWidth < fbHeight, "m_screenWidth >= fbHeight");
    vAssert(m_screenHeight < fbWidth, "m_screenHeight >= fbWidth");

    m_screenBufferPtr = new (std::nothrow) u8[m_screenWidth * m_screenHeight];
    memset(m_screenBufferPtr, 0, m_screenWidth * m_screenHeight * sizeof(u8));

    // Calculate fullscreen LUT
    m_fullscreenLUT = new (std::nothrow) size_t[fbWidth * fbHeight];
    memset(m_fullscreenLUT, 0, fbWidth * fbHeight * sizeof(size_t));

    for (float j = 0; j < fbWidth; ++j) {
      for (float i = 0; i < fbHeight; ++i) {
        size_t y = (j / (float)fbWidth) * m_screenHeight;
        if (y >= m_screenHeight)
          y = m_screenHeight - 1;

        size_t x = (i / (float)fbHeight) * m_screenWidth;
        if (x >= m_screenWidth)
          x = m_screenWidth - 1;

        const int deltaY = fbWidth - m_screenHeight;
        const size_t lutAddr = (m_screenHeight - j + deltaY) + i * fbWidth;

        const size_t bufferAddr = x + y * m_screenWidth;
        m_fullscreenLUT[lutAddr] = bufferAddr;
      }
    }

  } else {
    vAssert(false, "Failed to get framebuffer pointer on ::init");
  }

  // Load user configs if possible
  loadOptions();
}

void SystemStub_THREEDS::destroy() {
  if (m_screenBufferPtr != 0)
    delete [] m_screenBufferPtr;
    
  if (m_fullscreenLUT != 0)
    delete [] m_fullscreenLUT;

  if (m_palette != 0)
    delete [] m_palette;

  // Wait for sound thread
  m_audioCore.quitSoundThread = true; 
  threadJoin(m_audioCore.threadHandle, U64_MAX);

  svcCloseHandle(m_audioCore.mutex);
  gfxExit();
}

void SystemStub_THREEDS::setPalette(const uint8* pal, int n) {
	vAssert(n <= PAL_MAX_SIZE, "Invalid palette index: " << n);
  memcpy(m_palette, pal, sizeof(uint8) * n * 3);
}

void SystemStub_THREEDS::setPaletteEntry(int i, const Color* c) {
	vAssert(i < PAL_MAX_SIZE, "Invalid palette index: " << i);
  vAssert(c != 0, "Invalid setPaletteEntry color pointer");

  m_palette[i * 3 + 0] = (c->r << 2) | (c->r & 3);
  m_palette[i * 3 + 1] = (c->g << 2) | (c->g & 3);
  m_palette[i * 3 + 2] = (c->b << 2) | (c->b & 3);
}

void SystemStub_THREEDS::getPaletteEntry(int i, Color* c) {
	vAssert(i < PAL_MAX_SIZE, "Invalid palette index: " << i);
  vAssert(c != 0, "Invalid setPaletteEntry color pointer");

  memcpy(c, &m_palette[i * 3], sizeof(u8) * 3);
}

void SystemStub_THREEDS::setOverscanColor(int i) {
  m_overScanColor = i;
}

void SystemStub_THREEDS::copyRect(int x, int y, int w, int h, const uint8* buf, int pitch) {
  vAssert(m_screenBufferPtr != 0, "Invalid m_screenBufferPtr pointer! in ::copyRect");

  if (x >= (int)m_screenWidth || y >= (int)m_screenHeight)
    return;

  if (x < 0)
    x = 0;

  if (y < 0)
    y = 0;

  if (x + w > (int)m_screenWidth)
    w = m_screenWidth - x;

  if (y + h > (int)m_screenHeight)
    h = m_screenHeight - y;

  for (int j = 0; j < h; ++j) {
    const size_t baseAddr0 = (y + j) * pitch;

    for (int i = 0; i < w; ++i) {
      const size_t baseAddr = x + i + baseAddr0; 
      vAssert(baseAddr < m_screenWidth * m_screenHeight, 
        "Base address is bigger than buffer: " << baseAddr);

      m_screenBufferPtr[baseAddr] = buf[baseAddr];
    }
  }
}

void SystemStub_THREEDS::fadeScreen() {
  // TODO
}

void SystemStub_THREEDS::updateScreen(int shakeOffset) {
  vAssert(m_screenBufferPtr != 0, "Invalid screen buffer pointer m_screenBufferPtr");

  // Copy colors from buffer to screen.
  u16 fbWidth = 0, fbHeight = 0;
  u16* framebufferPtr = (u16*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbWidth, &fbHeight);

  vAssert(fbWidth > 0, "Invalid framebuffer width " << fbWidth);
  vAssert(fbHeight > 0, "Invalid framebuffer height " << fbHeight);

  // Centered or Scaled?
  if (!m_fullScreen) {
    // Figure out proper X and Y to start rendering at
    const int startX = (fbHeight / 2) - (m_screenWidth / 2);
    const int startY = (fbWidth / 2) - (m_screenHeight / 2);

    for (size_t j = 0; j < m_screenHeight; ++j) {
      for (size_t i = 0; i < m_screenWidth; ++i) {
        
        const size_t index = m_screenBufferPtr[i + j * m_screenWidth];
        vAssert(index < PAL_MAX_SIZE, "Invalid palette index " << index);
        
        // 3DS screen is 90' rotated.
        const u8* color = &m_palette[index * 3];
        const size_t baseAddr = ((m_screenHeight - j + startY) + 
          (i + startX) * fbWidth);

        framebufferPtr[baseAddr] = RGB8_to_565(color[0], color[1], color[2]);
      }
    }
  } else {
    const size_t fbSize = fbWidth * fbHeight;
    for (size_t i = 0; i < fbSize; ++i) {
      const size_t index = m_screenBufferPtr[m_fullscreenLUT[i]];
      vAssert(index < PAL_MAX_SIZE, "Invalid palette index " << index);
        
      const u8* color = &m_palette[index * 3];
      framebufferPtr[i] = RGB8_to_565(color[0], color[1], color[2]);
    }
  }

  // Flush and swap framebuffers
  gfxFlushBuffers();
  gfxSwapBuffers();
}

static void clearFramebuffers() {
  u16 fbWidth = 0; 
  u16 fbHeight = 0;
  u16* framebufferPtr = (u16*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, 
    &fbWidth, &fbHeight);

  for (int i = 0; i < 2; ++i) {
    const size_t fbSize = fbWidth * fbHeight;
    memset(framebufferPtr, 0, fbSize * sizeof(u16));

    gfxFlushBuffers();
    gfxSwapBuffers();

    framebufferPtr = (u16*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, 
      &fbWidth, &fbHeight);
  }
}

void SystemStub_THREEDS::processEvents() {
	if (!aptMainLoop()) {
    _pi.quit = true;

    return;
  }

  gspWaitForVBlank();
  hidScanInput();

  // Handle player input
  const u32 kDown = hidKeysDown();
  const u32 kUp = hidKeysUp();
  const u32 kHeld = hidKeysHeld();

  if (kDown & KEY_SELECT) {
    m_paused = true;
    m_audioCore.playing = false;

    renderOptions();
    return;
  }

  // If any of the directional movement key is pressed, clear dirMask and 
  // recalculate
  _pi.dirMask = 0;
  if (kHeld & KEY_UP 
    || kHeld & KEY_DOWN 
    || kHeld & KEY_LEFT 
    || kHeld & KEY_RIGHT) {
  
    if (kHeld & KEY_UP)
      _pi.dirMask |= PlayerInput::DIR_UP;
    if (kHeld & KEY_LEFT)
      _pi.dirMask |= PlayerInput::DIR_LEFT;
    if (kHeld & KEY_RIGHT)
      _pi.dirMask |= PlayerInput::DIR_RIGHT;
    if (kHeld & KEY_DOWN)
      _pi.dirMask |= PlayerInput::DIR_DOWN;
  }

  // Find bindings, exclude directional keys, select and start.
  for (int i = 0; i < KI_KEY_MAX_KEYS; ++i) {
    if (i == KI_KEY_DRIGHT 
      || i == KI_KEY_DLEFT
      || i == KI_KEY_DUP
      || i == KI_KEY_DDOWN
      || i == KI_KEY_SELECT
      || i == KI_KEY_START) {

      continue;
    }

    const auto kCommand = m_keyBindings[i];
    if (kCommand == 0)
      continue;

    const u16 kFlag = BIT(i);
    if (kDown & kFlag) {

      if (kCommand == KBT_BACKSPACE)  _pi.backspace = true;
      else if (kCommand == KBT_ENTER) _pi.enter = true;
      else if (kCommand == KBT_SHIFT) _pi.shift = true;
      else if (kCommand == KBT_SPACE) _pi.space = true;

    } else if (kUp & kFlag) {

      if (kCommand == KBT_BACKSPACE)  _pi.backspace = false;
      else if (kCommand == KBT_ENTER) _pi.enter = false;
      else if (kCommand == KBT_SHIFT) _pi.shift = false;
      else if (kCommand == KBT_SPACE) _pi.space = false;

    }
  }

  if (kDown & KEY_START)
    _pi.escape = true;
  else if (kUp & KEY_START)
    _pi.escape = false;
}

void SystemStub_THREEDS::sleep(int duration) {
  // Thanks to WinterMute for this
  svcSleepThread(duration * 1e6);
}

// From smea
#define TICKS_PER_SEC (268123480)
#define TICKS_PER_MSEC (268123)

uint32 SystemStub_THREEDS::getTimeStamp() {
  const u64 deltaTime = (svcGetSystemTick() - m_startTick) / TICKS_PER_MSEC;
  const u32 time = deltaTime & 0xFFFFFFFF;

  return time;
}

// Audio callback used by Flashback
// typedef void (*AudioCallback)(void *param, uint8 *stream, int len);
#define AUDIO_BUFFER_LENGTH 8192
#define NUM_SOUND_BUFFER 4
#define NUM_WAVEBUFS 128

void playSound(SystemStub_THREEDS::AudioCore& audioCore, 
  ndspWaveBuf* waveBuffer, u8* buffer) {

  // Clean wavebuffer.
  memset(waveBuffer, 0, sizeof(ndspWaveBuf));

  // Wait for sound mutex
  svcWaitSynchronization(audioCore.mutex, U64_MAX);
  
  // Callback should fill the entire buffer, even with silence.
  audioCore.callback(audioCore.callbackParam, buffer, AUDIO_BUFFER_LENGTH);
	
  // Release and play
  svcReleaseMutex(audioCore.mutex);
  
  // Upload the buffer
  DSP_FlushDataCache(buffer, AUDIO_BUFFER_LENGTH);
  
  ndspChnSetRate(0, SystemStub_THREEDS::SOUND_SAMPLE_RATE);
  waveBuffer->data_vaddr = buffer;
  waveBuffer->nsamples = AUDIO_BUFFER_LENGTH;
  waveBuffer->looping = false;
  waveBuffer->status = NDSP_WBUF_FREE;
  DSP_FlushDataCache(waveBuffer, sizeof(ndspWaveBuf));
  
  float mix[12] = {
    1.0f, 1.0f,
    1.0f, 1.0f,
    0.0f, 0.0f,
    0.0f, 0.0f,
    0.0f, 0.0f,
    0.0f, 0.0f
  };
  
  ndspChnSetMix(0, mix);
  ndspChnWaveBufAdd(0, waveBuffer);
}

void soundThreadHandler(void* arg) {
  ndspInit();
  ndspChnSetInterp(0, NDSP_INTERP_NONE);
  ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM8);
  
  size_t waveBuffIndex = 0;
  ndspWaveBuf* waveBuffer[NUM_WAVEBUFS];
  for (int i = 0; i < NUM_WAVEBUFS; ++i)
    waveBuffer[i] = (ndspWaveBuf*) calloc(1, sizeof(ndspWaveBuf));

  SystemStub_THREEDS* system = (SystemStub_THREEDS*)arg;
  SystemStub_THREEDS::AudioCore& audioCore = system->m_audioCore;

  bool initialized = false;
  size_t buffIndex = 0;

  u8* buffer[NUM_SOUND_BUFFER];
  for (int i = 0; i < NUM_SOUND_BUFFER; ++i)
    buffer[i] = (u8*)linearAlloc(AUDIO_BUFFER_LENGTH);

  while (!audioCore.quitSoundThread) {
    if (!audioCore.playing) {
      svcSleepThread(1e4);
      continue;
    }

    if (!initialized) {
      ndspChnWaveBufClear(0);
      initialized = true;
    }

    playSound(audioCore, waveBuffer[waveBuffIndex], buffer[buffIndex]);
    if (++buffIndex >= NUM_SOUND_BUFFER)
      buffIndex = 0;

    if (++waveBuffIndex >= NUM_WAVEBUFS)
      waveBuffIndex = 0;

    // Wait for sound to start
    while (!ndspChnIsPlaying(0))
      svcSleepThread(1e4);

    // Wait for next buffer
    while (ndspChnIsPlaying(0))
      svcSleepThread(1e4);
  }

  for (int i = 0; i < NUM_WAVEBUFS; ++i)
    free(waveBuffer[i]);
	
  for (int i = 0; i < NUM_SOUND_BUFFER; ++i)
    linearFree(buffer[i]);

  ndspExit();
  svcExitThread();
}

void SystemStub_THREEDS::startAudio(AudioCallback callback, void* param) {
  s32 currPriority = 0;
	svcGetThreadPriority(&currPriority, CUR_THREAD_HANDLE);

  m_audioCore.quitSoundThread = false;
  m_audioCore.playing = true;
  m_audioCore.callback = callback;
  m_audioCore.callbackParam = param;
  m_audioCore.threadHandle = threadCreate(soundThreadHandler, this, 4 * 1024, 
    currPriority - 1, -2, false);

  svcCreateMutex(&m_audioCore.mutex, false);
}

void SystemStub_THREEDS::stopAudio() {
  m_audioCore.playing = false;
}

uint32 SystemStub_THREEDS::getOutputSampleRate() {
	return SOUND_SAMPLE_RATE;
}

void SystemStub_THREEDS::lockAudio() {
  svcWaitSynchronization(m_audioCore.mutex, U64_MAX);
}

void SystemStub_THREEDS::unlockAudio() {
  svcReleaseMutex(m_audioCore.mutex);
}

static const char* selectedOption(int index, int desired) {
  return (index == desired ? "\x1b[32m  " : "  ");
}

static const char* clearColor() {
  return "\x1b[0m";
}

static const char* bindingName(int binding) {
  switch (binding) {
  default:
  case SystemStub_THREEDS::KBT_NONE:
    return "No Binding";
    break;
  case SystemStub_THREEDS::KBT_BACKSPACE:
    return "Backspace";
    break;
  case SystemStub_THREEDS::KBT_ENTER:
    return "Enter";
    break;
  case SystemStub_THREEDS::KBT_SHIFT:
    return "Shift";
    break;
  case SystemStub_THREEDS::KBT_SPACE:
    return "Space";
    break;
  }
}

static void drawPickCommands(SystemStub_THREEDS* systemPtr, int selectedIndex) {
  consoleClear();
  std::cout << std::endl << std::endl << " Select command for key:" << 
    std::endl << std::endl << std::endl;
  
  for (int i = 0 ; i < SystemStub_THREEDS::KBT_MAX_TARGETS; ++i) {
    std::cout << selectedOption(selectedIndex, i) << "\t\t\t\t" <<
      systemPtr->m_commandNames[i] << clearColor() << 
      std::endl << std::endl;
  }
}

static u16 pickCommand(SystemStub_THREEDS* systemPtr) {
  const int maxOptions = SystemStub_THREEDS::KBT_MAX_TARGETS - 1;
  int selectedIndex = 0;

  drawPickCommands(systemPtr, selectedIndex);
  while (aptMainLoop()) {
    hidScanInput();
    const u32 kUp = hidKeysUp();

    if (kUp & KEY_UP) {
      selectedIndex--;
      if (selectedIndex < 0)
        selectedIndex = maxOptions;

      drawPickCommands(systemPtr, selectedIndex);
    } else if (kUp & KEY_DOWN) {
      selectedIndex++;
      if (selectedIndex > maxOptions)
        selectedIndex = 0;

      drawPickCommands(systemPtr, selectedIndex);
    }

    // Select
    if (kUp & KEY_A)
      return SystemStub_THREEDS::KBT_NONE + selectedIndex;

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }

  vAssert(false, "OOPS!");
  return 0;
}
  
void SystemStub_THREEDS::selectOption(int selectedIndex) {
  switch (selectedIndex) {
  case 0: // Full screen
    m_fullScreen = !m_fullScreen;
    if (!m_fullScreen) 
      clearFramebuffers();
    break;
  case 1: // L
    m_keyBindings[KI_KEY_L] = pickCommand(this);
    break;
  case 2: // R
    m_keyBindings[KI_KEY_R] = pickCommand(this);
    break;
  case 3: // A
    m_keyBindings[KI_KEY_A] = pickCommand(this);
    break;
  case 4: // B
    m_keyBindings[KI_KEY_B] = pickCommand(this);
    break;
  case 5: // X
    m_keyBindings[KI_KEY_X] = pickCommand(this);
    break;
  case 6: // Y
    m_keyBindings[KI_KEY_Y] = pickCommand(this);
    break;
  case 7: // PAUSE
    m_paused = false;
    m_audioCore.playing = true;
    break;
  case 8: // QUIT
    _pi.quit = true;
    break;
  }
}

void SystemStub_THREEDS::renderOptionsText(int selectedIndex) {
  consoleClear();
  std::cout << std::endl << " Video:" << std::endl << std::endl;
  std::cout << selectedOption(selectedIndex, 0) << 
    (m_fullScreen ? " Display scaled (Unstable)" : " Normal size") <<
    clearColor() << std::endl << std::endl << std::endl;
  
  std::cout << std::endl << " Controls:" << std::endl << std::endl;
  std::cout << selectedOption(selectedIndex, 1) << " Shoulder L (" << 
    bindingName(m_keyBindings[KI_KEY_L]) << ")" << clearColor() << 
    std::endl << std::endl;
  
  std::cout << selectedOption(selectedIndex, 2) << " Shoulder R (" << 
    bindingName(m_keyBindings[KI_KEY_R]) << ")" << clearColor() << 
    std::endl << std::endl;
  
  std::cout << selectedOption(selectedIndex, 3) << " A (" << 
    bindingName(m_keyBindings[KI_KEY_A]) << ")" << clearColor() << 
    std::endl << std::endl;
  
  std::cout << selectedOption(selectedIndex, 4) << " B (" << 
    bindingName(m_keyBindings[KI_KEY_B]) << ")" << clearColor() << 
    std::endl << std::endl;
  
  std::cout << selectedOption(selectedIndex, 5) << " X (" << 
    bindingName(m_keyBindings[KI_KEY_X]) << ")" << clearColor() << 
    std::endl << std::endl;
  
  std::cout << selectedOption(selectedIndex, 6) << " Y (" << 
    bindingName(m_keyBindings[KI_KEY_Y]) << ")" << clearColor() << 
    std::endl << std::endl;
  
  std::cout << std::endl << std::endl << "\t\t\t" << 
    selectedOption(selectedIndex, 7) << " Return to game" << 
    clearColor() << std::endl;
  
  std::cout << std::endl << "\t\t\t" << 
    selectedOption(selectedIndex, 8) << " Exit Game" << 
    clearColor() << std::endl << std::endl;
}
      
void SystemStub_THREEDS::renderOptions() {
  const int maxOptions = 8;
  int selectedIndex = 0;

  renderOptionsText(selectedIndex);
  while (aptMainLoop()) {
    hidScanInput();
    const u32 kUp = hidKeysUp();

    if (kUp & KEY_UP) {
      selectedIndex--;
      if (selectedIndex < 0)
        selectedIndex = maxOptions;

      renderOptionsText(selectedIndex);
    } else if (kUp & KEY_DOWN) {
      selectedIndex++;
      if (selectedIndex > maxOptions)
        selectedIndex = 0;

      renderOptionsText(selectedIndex);
    }

    // Select
    if (kUp & KEY_A) {
      selectOption(selectedIndex);
      renderOptionsText(selectedIndex);
    }

    if (!m_paused || _pi.quit) {
      consoleClear();
      saveOptions();

      return;
    }

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
}

void SystemStub_THREEDS::loadOptions() {
  std::ifstream optionsFile("./options.cfg", std::ios_base::in);
  if (!optionsFile.is_open())
    return;

  optionsFile >> m_fullScreen;
  for (int i = 0; i < KI_KEY_MAX_KEYS; ++i) {
    u16 key;
    optionsFile >> key;
    
    // Ignore directionals
    if (i == KI_KEY_DRIGHT 
      || i == KI_KEY_DLEFT
      || i == KI_KEY_DUP
      || i == KI_KEY_DDOWN
      || i == KI_KEY_SELECT
      || i == KI_KEY_START) {
      
      continue;
    } else
      m_keyBindings[i] = key;
  }
}

void SystemStub_THREEDS::saveOptions() {
  std::ofstream optionsFile("./options.cfg", std::ios_base::out);
  if (!optionsFile.is_open())
    return;

  optionsFile << m_fullScreen << std::endl;
  for (int i = 0; i < KI_KEY_MAX_KEYS; ++i)
    optionsFile << m_keyBindings[i] << std::endl;
}

