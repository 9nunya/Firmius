#include "utils/Clipboard.hpp"
#include <cstdlib>
#include <vector>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <memory>
#include <array>
#include <sstream>

static const char base64_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static std::string base64_encode(const unsigned char* bytes_to_encode, size_t in_len) {
    std::string ret;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for(i = 0; (i <4) ; i++)
                ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for(j = i; j < 3; j++)
            char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        for (j = 0; (j < i + 1); j++)
            ret += base64_chars[char_array_4[j]];
        while((i++ < 3))
            ret += '=';
    }
    return ret;
}

#if defined(_WIN32)

#include <windows.h>

bool Clipboard::isWayland() { return false; }
bool Clipboard::isX11() { return false; }

bool Clipboard::setText(const std::string& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    const int size = static_cast<int>(text.size()) + 1;
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!handle) {
        CloseClipboard();
        return false;
    }
    void* data = GlobalLock(handle);
    if (!data) {
        GlobalFree(handle);
        CloseClipboard();
        return false;
    }
    std::memcpy(data, text.c_str(), size);
    GlobalUnlock(handle);
    if (!SetClipboardData(CF_TEXT, handle)) {
        GlobalFree(handle);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

bool Clipboard::hasImage() {
    bool hasImg = false;
    if (OpenClipboard(nullptr)) {
        hasImg = IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP);
        CloseClipboard();
    }
    return hasImg;
}

std::optional<std::string> Clipboard::getImage(std::string& mimeType) {
    if (!OpenClipboard(nullptr)) return std::nullopt;
    
    std::optional<std::string> result = std::nullopt;
    HANDLE hData = GetClipboardData(CF_DIB);
    if (hData != nullptr) {
        BITMAPINFO* pBmpInfo = static_cast<BITMAPINFO*>(GlobalLock(hData));
        if (pBmpInfo != nullptr) {
            int colors = pBmpInfo->bmiHeader.biClrUsed;
            if (colors == 0 && pBmpInfo->bmiHeader.biBitCount <= 8) {
                colors = 1 << pBmpInfo->bmiHeader.biBitCount;
            }
            int paletteSize = colors * sizeof(RGBQUAD);
            if (pBmpInfo->bmiHeader.biCompression == BI_BITFIELDS) {
                paletteSize += 3 * sizeof(DWORD);
            }
            
            DWORD headerSize = pBmpInfo->bmiHeader.biSize + paletteSize;
            DWORD imageSize = pBmpInfo->bmiHeader.biSizeImage;
            
            if (imageSize == 0) {
                int rowSize = ((pBmpInfo->bmiHeader.biWidth * pBmpInfo->bmiHeader.biBitCount + 31) / 32) * 4;
                imageSize = rowSize * abs(pBmpInfo->bmiHeader.biHeight);
            }
            
            DWORD fileSize = sizeof(BITMAPFILEHEADER) + headerSize + imageSize;
            std::vector<unsigned char> bmpData(fileSize);
            
            BITMAPFILEHEADER bfh = {0};
            bfh.bfType = 0x4D42;
            bfh.bfSize = fileSize;
            bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + headerSize;
            
            std::memcpy(bmpData.data(), &bfh, sizeof(BITMAPFILEHEADER));
            std::memcpy(bmpData.data() + sizeof(BITMAPFILEHEADER), pBmpInfo, headerSize + imageSize);
            
            GlobalUnlock(hData);
            
            mimeType = "image/bmp";
            result = base64_encode(bmpData.data(), bmpData.size());
        }
    }
    CloseClipboard();
    return result;
}

#elif defined(__APPLE__)

#import <AppKit/AppKit.h>

bool Clipboard::isWayland() { return false; }
bool Clipboard::isX11() { return false; }

bool Clipboard::setText(const std::string& text) {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        NSString *string = [NSString stringWithUTF8String:text.c_str()];
        return [pasteboard setString:string forType:NSPasteboardTypeString];
    }
}

bool Clipboard::hasImage() {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSArray *classes = @[ [NSImage class] ];
        NSDictionary *options = @{};
        return [pasteboard canReadObjectForClasses:classes options:options];
    }
}

std::optional<std::string> Clipboard::getImage(std::string& mimeType) {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSArray *classes = @[ [NSImage class] ];
        NSDictionary *options = @{};
        
        if ([pasteboard canReadObjectForClasses:classes options:options]) {
            NSArray *objects = [pasteboard readObjectsForClasses:classes options:options];
            if (objects && [objects count] > 0) {
                NSImage *image = [objects objectAtIndex:0];
                NSData *tiffData = [image TIFFRepresentation];
                if (tiffData) {
                    NSBitmapImageRep *imageRep = [NSBitmapImageRep imageRepWithData:tiffData];
                    NSData *pngData = [imageRep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                    if (pngData) {
                        mimeType = "image/png";
                        return base64_encode(static_cast<const unsigned char*>([pngData bytes]), [pngData length]);
                    }
                }
            }
        }
    }
    return std::nullopt;
}

#elif defined(__linux__)

#include <memory>
#include <array>

static std::vector<unsigned char> execCmdBinary(const char* cmd) {
    std::vector<unsigned char> result;
    std::shared_ptr<FILE> pipe(popen(cmd, "r"), pclose);
    if (!pipe) return result;
    
    std::array<unsigned char, 1024> buffer;
    while (!feof(pipe.get())) {
        size_t bytes = fread(buffer.data(), 1, buffer.size(), pipe.get());
        if (bytes > 0) {
            result.insert(result.end(), buffer.data(), buffer.data() + bytes);
        }
    }
    return result;
}

static std::string execCmdString(const char* cmd) {
    auto binary = execCmdBinary(cmd);
    return std::string(binary.begin(), binary.end());
}

static bool execCmdWrite(const char* cmd, const std::string& input) {
    FILE* pipe = popen(cmd, "w");
    if (!pipe) return false;
    const size_t written = fwrite(input.data(), 1, input.size(), pipe);
    const int status = pclose(pipe);
    return status == 0 && written == input.size();
}

static bool writeOsc52(const std::string& text) {
    const std::string encoded = base64_encode(
        reinterpret_cast<const unsigned char*>(text.data()), text.size());
    std::cout << "\033]52;c;" << encoded << "\a" << std::flush;
    return true;
}

bool Clipboard::isWayland() {
    return std::getenv("WAYLAND_DISPLAY") != nullptr;
}

bool Clipboard::isX11() {
    return std::getenv("DISPLAY") != nullptr;
}

bool Clipboard::setText(const std::string& text) {
    if (isWayland()) {
        if (execCmdWrite("wl-copy --type text/plain 2>/dev/null", text)) {
            return true;
        }
    } else if (isX11()) {
        if (execCmdWrite("xclip -selection clipboard -in 2>/dev/null", text)) {
            return true;
        }
        if (execCmdWrite("xsel --clipboard --input 2>/dev/null", text)) {
            return true;
        }
    }
    return writeOsc52(text);
}

bool Clipboard::hasImage() {
    if (isWayland()) {
        std::string output = execCmdString("wl-paste --list-types 2>/dev/null");
        return output.find("image/") != std::string::npos;
    } else if (isX11()) {
        std::string output = execCmdString("xclip -selection clipboard -t TARGETS -o 2>/dev/null");
        if (output.empty()) {
            output = execCmdString("xsel -b -t TARGETS -o 2>/dev/null");
        }
        return output.find("image/") != std::string::npos;
    }
    return false;
}

std::optional<std::string> Clipboard::getImage(std::string& mimeType) {
    if (!hasImage()) return std::nullopt;
    
    std::string cmd;
    if (isWayland()) {
        cmd = "wl-paste --type image/png 2>/dev/null";
        mimeType = "image/png";
    } else if (isX11()) {
        cmd = "xclip -selection clipboard -t image/png -o 2>/dev/null";
        mimeType = "image/png";
    } else {
        return std::nullopt;
    }
    
    std::vector<unsigned char> data = execCmdBinary(cmd.c_str());
    if (data.empty()) return std::nullopt;
    
    return base64_encode(data.data(), data.size());
}

#else

bool Clipboard::isWayland() { return false; }
bool Clipboard::isX11() { return false; }
bool Clipboard::setText(const std::string& text) {
    (void)text;
    return false;
}
bool Clipboard::hasImage() { return false; }
std::optional<std::string> Clipboard::getImage(std::string& mimeType) {
    (void)mimeType;
    return std::nullopt;
}

#endif
