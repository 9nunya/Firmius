#pragma once

#include <string>
#include <optional>

class Clipboard {
public:
    static bool setText(const std::string& text);
    static bool hasImage();
    
    static std::optional<std::string> getImage(std::string& mimeType);
    
    static bool isWayland();
    
    static bool isX11();
};
