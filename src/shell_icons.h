#pragma once
#include <windows.h>
#include <shellapi.h>
#include <GL/gl.h>
#include <imgui.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

// Windows Explorer's file association icons, owned by the current GL context.
// Resolve a path only once; reuse textures for identical system icon indices.
class ShellIcons {
    std::map<std::wstring, int> paths;
    std::map<int, GLuint> textures;
public:
    GLuint Get(const std::wstring& path, bool directory = false) {
        auto cached = paths.find(path);
        if (cached != paths.end()) return textures[cached->second];
        SHFILEINFOW info{};
        UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
        if (directory) flags |= SHGFI_USEFILEATTRIBUTES;
        if (!SHGetFileInfoW(path.c_str(), directory ? FILE_ATTRIBUTE_DIRECTORY : 0,
                            &info, sizeof(info), flags)) {
            if (!SHGetFileInfoW(path.c_str(),directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                                &info,sizeof(info),flags | SHGFI_USEFILEATTRIBUTES)) return 0;
        }
        paths[path] = info.iIcon;
        auto existing = textures.find(info.iIcon);
        if (existing != textures.end()) { DestroyIcon(info.hIcon); return existing->second; }
        // Render on black and white to recover straight alpha for both modern
        // alpha icons and older AND-mask icons, including their antialiased edges.
        constexpr int size = 20;
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = size; bitmap.bmiHeader.biHeight = -size;
        bitmap.bmiHeader.biPlanes = 1; bitmap.bmiHeader.biBitCount = 32;
        HDC dc = CreateCompatibleDC(nullptr);
        void* pixels = nullptr;
        HBITMAP dib = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
        GLuint texture = 0;
        if (dc && dib && pixels) {
            HGDIOBJ old = SelectObject(dc, dib);
            auto* bytes = static_cast<unsigned char*>(pixels);
            std::fill(bytes, bytes + size*size*4, 0);
            bool drawn = DrawIconEx(dc,0,0,info.hIcon,size,size,0,nullptr,DI_NORMAL) != FALSE;
            GdiFlush();
            std::vector<unsigned char> black(bytes, bytes + size*size*4), rgba(size*size*4);
            std::fill(bytes, bytes + size*size*4, 255);
            drawn = DrawIconEx(dc,0,0,info.hIcon,size,size,0,nullptr,DI_NORMAL) != FALSE && drawn;
            GdiFlush();
            for (int i=0; i<size*size; ++i) {
                int alpha = std::clamp(255 - (int(bytes[i*4]) - int(black[i*4])), 0, 255);
                for (int c=0;c<3;++c)
                    rgba[i*4+c] = alpha ? std::min(255, int(black[i*4+2-c])*255/alpha) : 0;
                rgba[i*4+3] = alpha;
            }
            SelectObject(dc, old);
            if (drawn) {
                GLint previous = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
                glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,size,size,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
                glBindTexture(GL_TEXTURE_2D, previous);
            }
        }
        if (dib) DeleteObject(dib);
        if (dc) DeleteDC(dc);
        DestroyIcon(info.hIcon);
        textures[info.iIcon] = texture;
        return texture;
    }
    void Clear() {
        for (auto& icon : textures) if (icon.second) glDeleteTextures(1, &icon.second);
        textures.clear(); paths.clear();
    }
    size_t TextureCount() const { return textures.size(); }
};
