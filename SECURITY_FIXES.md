# Security and Compatibility Fixes for Quake Codebase

This document summarizes the security vulnerabilities and compatibility issues that were identified and fixed in the Quake codebase.

## Issues Fixed

### 1. OpenGL Extensions Buffer Overflow (CRITICAL)
**Location:** `WinQuake/gl_vidlinuxglx.c`, `QW/client/gl_vidlinux_x11.c`

**Problem:** Modern NVIDIA and AMD GPUs return very long GL_EXTENSIONS strings (>10KB) that overflow the 4096-byte MAXPRINTMSG buffer in `Con_Printf()`, causing crashes on startup.

**Fix:** 
- Modified `GL_Init()` to print GL_EXTENSIONS in 1024-byte chunks
- Increased MAXPRINTMSG from 4096 to 16384 bytes
- Replaced `vsprintf()` with `vsnprintf()` with proper size checks

### 2. X11 Hardcoded Resolution (COMPATIBILITY)
**Location:** `WinQuake/gl_vidlinuxglx.c`, `QW/client/gl_vidlinux_x11.c`

**Problem:** Resolution was hardcoded to 640x480, causing issues on Full HD (1920x1080) and higher resolution displays.

**Fix:**
- Added automatic screen resolution detection using `DisplayWidth()` and `DisplayHeight()`
- Cap resolution at 1920x1080 for performance
- Still respects `-width` and `-height` command line parameters

### 3. Buffer Overflow in vsprintf Calls (HIGH)
**Location:** `WinQuake/console.c`, `QW/client/console.c`

**Problem:** Multiple uses of `vsprintf()` without bounds checking could cause buffer overflows with malformed input.

**Fix:**
- Replaced all `vsprintf()` calls with `vsnprintf()` with proper size limits
- Added explicit null termination after all vsnprintf calls
- Fixed in `Con_Printf()`, `Con_DPrintf()`, `Con_SafePrintf()`, and `Con_DebugLog()`

### 4. Buffer Overflow in sprintf Calls (MEDIUM)
**Location:** Various files

**Problem:** Several `sprintf()` calls could overflow fixed-size buffers.

**Fix:**
- Replaced `sprintf()` with `snprintf()` where appropriate
- Fixed directory path construction in `VID_SetPalette()` and `VID_Init()`

### 5. String Function Buffer Overflow (MEDIUM)
**Location:** `WinQuake/common.c`, `QW/client/common.c`

**Problem:** `Q_strncpy()` did not always null-terminate strings when the buffer was exactly filled.

**Fix:**
- Modified `Q_strncpy()` to always null-terminate the destination buffer
- Added bounds checking to prevent writing past buffer end

### 6. Integer Overflow in Memory Allocations (MEDIUM)
**Location:** `WinQuake/vid_win.c`, `QW/client/vid_win.c`

**Problem:** `width * height * sizeof(*d_pzbuffer)` calculation could overflow for very large resolutions.

**Fix:**
- Added integer overflow checks in `VID_CheckAdequateMem()` and `VID_AllocBuffers()`
- Return false if overflow would occur, preventing crash

### 7. Integer Overflow in Vertex Allocations (MEDIUM)
**Location:** `WinQuake/gl_rsurf.c`, `WinQuake/gl_warp.c`, `QW/client/gl_rsurf.c`, `QW/client/gl_warp.c`

**Problem:** `(numverts-4) * VERTEXSIZE * sizeof(float)` calculation could overflow with malformed BSP files.

**Fix:**
- Added overflow checks before `Hunk_Alloc()` calls
- Call `Sys_Error()` if overflow detected to prevent exploits

### 8. Array Bounds Overflow in Command Line Parsing (HIGH)
**Location:** `WinQuake/gl_vidlinuxglx.c`, `QW/client/gl_vidlinux_x11.c`

**Problem:** `com_argv[COM_CheckParm(param)+1]` accessed without verifying index+1 is within bounds.

**Fix:**
- Added bounds checking: `if (i != 0 && i < com_argc - 1)`
- Prevents reading past end of argv array
- Fixed for `-width`, `-height`, `-conwidth`, `-conheight`, and `-gamma` parameters

## Security Improvements Summary

### Buffer Overflow Prevention
- ✅ Fixed all `vsprintf()` calls (replaced with `vsnprintf()`)
- ✅ Fixed critical `sprintf()` calls (replaced with `snprintf()`)
- ✅ Fixed `Q_strncpy()` to always null-terminate
- ✅ Increased console buffer size for modern GPU extension strings

### Integer Overflow Prevention
- ✅ Added checks for `width * height` calculations
- ✅ Added checks for `numverts * VERTEXSIZE` calculations
- ✅ Protected all critical memory allocations

### Array Bounds Checking
- ✅ Added bounds checking for command-line argument access
- ✅ Protected against out-of-bounds array access in argument parsing

### Input Validation
- ✅ Added validation for resolution parameters
- ✅ Capped maximum resolution at 1920x1080
- ✅ Added checks before memory allocation

## Compatibility Improvements

### Display Resolution
- ✅ Auto-detection of native screen resolution on Linux/X11
- ✅ Proper support for Full HD (1920x1080) and higher displays
- ✅ Maintains backward compatibility with command-line parameters

### OpenGL Support
- ✅ Fixed crashes with modern GPUs (NVIDIA, AMD)
- ✅ Proper handling of long extension strings
- ✅ Chunked output prevents console buffer overflow

## Testing Recommendations

1. Test on modern NVIDIA/AMD GPUs to verify GL_EXTENSIONS fix
2. Test on Full HD (1920x1080) and 4K (3840x2160) displays
3. Test with malformed command-line parameters
4. Test with very large BSP files
5. Fuzz test console input
6. Test with `-width`, `-height`, `-gamma` parameters

## Notes for Developers

- Always use `snprintf()`/`vsnprintf()` instead of `sprintf()`/`vsprintf()`
- Always null-terminate strings after `strncpy()` operations
- Check for integer overflow before multiplications in size calculations
- Validate array indices before access
- Cap user-provided values (resolutions, buffer sizes, etc.)

## Known Remaining Issues

1. DirectInput handling is Windows-specific (Linux version already uses X11)
2. Some `strcpy()` calls in non-critical paths remain unchanged
3. Some older code uses fixed-size buffers that could be dynamic
4. Network code may have additional vulnerabilities (not reviewed)

## Files Modified

### WinQuake
- `console.c` - Buffer overflow fixes
- `common.c` - String function fixes
- `gl_vidlinuxglx.c` - Resolution and buffer overflow fixes
- `vid_win.c` - Integer overflow checks
- `gl_rsurf.c` - Integer overflow checks
- `gl_warp.c` - Integer overflow checks

### QW/client
- `console.c` - Buffer overflow fixes
- `common.c` - String function fixes
- `gl_vidlinux_x11.c` - Resolution and buffer overflow fixes
- `vid_win.c` - Integer overflow checks
- `gl_rsurf.c` - Integer overflow checks
- `gl_warp.c` - Integer overflow checks
