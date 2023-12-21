#include "submodules/getprocaddress/getprocaddress.c"
#define _LEANLOADER_DEBUG 0
/*
    leanloader.c
    a simple and efficient single-file image loader library
    (C) 2023, MIT License, https://github.com/martona/leanloader
    12/21/2023, 0.1.0, initial release

    A lightweight single-file library to load images. Internally it uses GDI+, but
    it does not depend on any linked libraries, including the CRT. It does #include
    getprocaddress.c from the submodules directory, which is a no-dependency library
    used internally, to make the dependency-free claim possible.

    Tested on Windows 10 with gcc 13.2; ymmv. Not inherently multi-thread-safe.

    Include this file in your project. It should be pretty friction-free. 
    
    Instantiate the leanloader_image_info struct, set the name field to the name
    of the image file you want to load (16-bit unicode string) and call leanloader_load.

    Upon return with a nonzero value, the fields of the struct will describe the image.

    Call leanloader_dispose when done with the image to free associated resources.
*/

// a few structs used internally

typedef struct {
    u32 w;              // image width
    u32 h;              // image height
    i32 stride;         // image stride (in bytes, always width * 4)
    u32 PixelFormat;    // pixel format, always 0x0026200a for 32-bit ARGB
    ptr ptr;            // pointer to the first pixel of the image
    ptr Reserved;       // reserved
} BitmapData;

typedef struct {
    wchar* name;        // name of the image file to load
    BitmapData bd;      // bitmap data, filled in by leanloader_load
    ptr gpbitmap;       // GDI+ bitmap handle, used internally
} leanloader_image_info;

typedef struct {
    u32 GdiplusVersion;             // Must be 1
    ptr DebugEventCallback;         // blah
    u32 SuppressBackgroundThread;   // blah
    u32 SuppressExternalCodecs;     // blah
} GdiplusStartupInput;

typedef struct {
    i32 x;
    i32 y;
    i32 w;
    i32 h;
} Rect;

// definitions for the function types we use
// kernel:
#ifndef _BASIC_KERNEL_DEFINED
#define _BASIC_KERNEL_DEFINED
typedef ptr (*LoadLibraryA_t)(char *name);
typedef void (*FreeLibrary_t)(ptr modulehandle);
typedef ptr (*GlobalAlloc_t)(u32 flags, u32 size);
typedef u32 (*GlobalFree_t)(ptr ptr);
#endif
// gdiplus:
typedef u32 (*GdipStartup_t)(ptr* token, GdiplusStartupInput* input, ptr output);
typedef u32 (*GdipShutdown_t)(ptr token);
typedef u32 (*GdipCreateBitmapFromFile_t)(wchar* filename, ptr* bitmap);
typedef u32 (*GdipDisposeImage_t)(ptr image);
typedef u32 (*GdipGetImageWidth_t)(ptr image, u32* width);
typedef u32 (*GdipGetImageHeight_t)(ptr image, u32* height);
typedef u32 (*GdipBitmapLockBits_t)(ptr bitmap, ptr rect, u32 flags, u32 format, ptr lockedbitmapdata);
typedef u32 (*GdipBitmapUnlockBits_t)(ptr bitmap, ptr lockedbitmapdata);

// a single-instance global global struct that holds module handles and function pointers
typedef struct {
    ptr token;
    u32 refcnt;
    ptr kernel32;
    ptr gdiplus;
    GetProcAddress_t            GetProcAddress;
    LoadLibraryA_t              LoadLibraryA;
    FreeLibrary_t               FreeLibrary;
    GlobalAlloc_t               GlobalAlloc;
    GlobalFree_t                GlobalFree;
    GdipStartup_t               GdipStartup;
    GdipShutdown_t              GdipShutdown;
    GdipCreateBitmapFromFile_t  GdipCreateBitmapFromFile;
    GdipDisposeImage_t          GdipDisposeImage;
    GdipGetImageWidth_t         GdipGetImageWidth;
    GdipGetImageHeight_t        GdipGetImageHeight;
    GdipBitmapLockBits_t        GdipBitmapLockBits;
    GdipBitmapUnlockBits_t      GdipBitmapUnlockBits;
} env_t;

static env_t env = {0};

// an internal function to initialize the "runtime environment"
static i32 leanloader_env_init() {
    if (env.refcnt == 0) {
        env.kernel32       = get_kernel32_modulehandle();
        env.GetProcAddress = get_getprocaddress(env.kernel32);
        env.LoadLibraryA   = env.GetProcAddress(env.kernel32, "LoadLibraryA");
        env.FreeLibrary    = env.GetProcAddress(env.kernel32, "FreeLibrary");
        env.GlobalAlloc    = env.GetProcAddress(env.kernel32, "GlobalAlloc");
        env.GlobalFree     = env.GetProcAddress(env.kernel32, "GlobalFree");
        env.gdiplus        = env.LoadLibraryA("gdiplus.dll");
        if (env.gdiplus) {
            env.GdipStartup                = env.GetProcAddress(env.gdiplus, "GdiplusStartup");
            env.GdipShutdown               = env.GetProcAddress(env.gdiplus, "GdiplusShutdown");
            env.GdipCreateBitmapFromFile   = env.GetProcAddress(env.gdiplus, "GdipCreateBitmapFromFile");
            env.GdipDisposeImage           = env.GetProcAddress(env.gdiplus, "GdipDisposeImage");
            env.GdipGetImageWidth          = env.GetProcAddress(env.gdiplus, "GdipGetImageWidth");
            env.GdipGetImageHeight         = env.GetProcAddress(env.gdiplus, "GdipGetImageHeight");
            env.GdipBitmapLockBits         = env.GetProcAddress(env.gdiplus, "GdipBitmapLockBits");
            env.GdipBitmapUnlockBits       = env.GetProcAddress(env.gdiplus, "GdipBitmapUnlockBits");
            GdiplusStartupInput input = {1, 0, 0, 0};
            u32 status = env.GdipStartup(&env.token, &input, 0);
            if (status == 0) {
                env.refcnt++;
            } else {
                env.FreeLibrary(env.gdiplus);
            }
        }
    } else {
        env.refcnt++;
    }
    return env.refcnt;
}

// an internal function to deinitialize the "runtime environment"
static i32 leanloader_env_deinit() {
    if (env.refcnt > 0) {
        env.refcnt--;
        if (env.refcnt == 0) {
            env.GdipShutdown(env.token);
            env.FreeLibrary(env.gdiplus);
        }
    }
    return env.refcnt;
}

// one of the two main functions, this one loads the image specified in the info struct
i32 leanloader_load(leanloader_image_info* info) {
    info->gpbitmap  = 0;
    if (leanloader_env_init()) {
        u32 status = env.GdipCreateBitmapFromFile(info->name, &info->gpbitmap);
        if (status == 0) {
            status = env.GdipGetImageWidth(info->gpbitmap, &info->bd.w);
            if (status == 0) {
                status = env.GdipGetImageHeight(info->gpbitmap, &info->bd.h);
                if (status == 0) {
                    u32 GPTR = 0x0040;
                    // ensure that the bitmap data allocation size is a multiple of 64 bytes
                    // this comes in handy when working with SIMD instructions up to AVX512 (specifically
                    // the loop cleanup code is easier because we can wander off the end of the row)
                    u32 allocSize = (info->bd.w * info->bd.h * 4) + 63 & ~63;
                    info->bd.ptr = env.GlobalAlloc(GPTR, allocSize);
                    if (info->bd.ptr) {
                        u32 ImageLockModeRead           = 0x0001;
                        u32 ImageLockModeWrite          = 0x0002;
                        u32 ImageLockModeUserInputBuf   = 0x0004;
                        u32 PixelFormat32bppARGB        = 0x0026200a;
                        u32 flags = ImageLockModeRead | ImageLockModeWrite | ImageLockModeUserInputBuf;
                        info->bd.PixelFormat = PixelFormat32bppARGB;
                        info->bd.stride = info->bd.w * 4;
                        Rect rect = {0, 0, info->bd.w, info->bd.h};
                        status = env.GdipBitmapLockBits(info->gpbitmap, &rect, flags, PixelFormat32bppARGB, &info->bd);
                        if (status == 0) {
                            return 1;
                        } else {
                            env.GlobalFree(info->bd.ptr);
                        }
                    }
                }
            }
            env.GdipDisposeImage(info->gpbitmap);
        }
    }
    return 0;
}

// the other main function, this one frees the resources allocated by leanloader_load
i32 leanloader_dispose(leanloader_image_info* info) {
    if (info->gpbitmap) {
        if (info->bd.ptr) {
            env.GdipBitmapUnlockBits(info->gpbitmap, &info->bd);
            env.GlobalFree(info->bd.ptr);
            info->bd.ptr = 0;
        }
        env.GdipDisposeImage(info->gpbitmap);
        info->gpbitmap = 0;
    }
    leanloader_env_deinit();
    return 0;
}

#if _LEANLOADER_DEBUG
#include <stdio.h>
int main(int argc, char *argv[]) {
    leanloader_image_info info;
    info.name = u"leanloader.png";
    leanloader_load(&info);
    leanloader_dispose(&info);
}
#endif
