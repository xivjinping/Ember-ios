// Title detection for the game library. Unlike a filename scan this opens
// the image: an .iso is only listed as an Xbox game with a name and title ID
// if it really contains an XDVDFS volume with a default.xbe. Files that only
// match by extension are still listed, but flagged as unverified.
#include "ember/core_api.h"

#include "xbe.h"
#include "xiso.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace ember;

namespace {

std::string lowerExtension(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e;
}

void fillFromXbe(const XbeImage& xbe, em_title_info* out) {
    out->verified = true;
    out->title_id = xbe.titleId;
    out->region   = xbe.region;
    if (!xbe.titleName.empty()) std::snprintf(out->name, sizeof(out->name), "%s", xbe.titleName.c_str());
    std::snprintf(out->title_code, sizeof(out->title_code), "%s", titleCode(xbe.titleId).c_str());
}

uint64_t directorySize(const fs::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (it->is_regular_file(fe)) total += it->file_size(fe);
    }
    return total;
}

// Extracted discs on case-sensitive volumes may spell it Default.xbe.
bool findDefaultXbe(const fs::path& dir, fs::path& found) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::string name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        if (name == "default.xbe") {
            found = entry.path();
            return true;
        }
    }
    return false;
}

} // namespace

bool em_probe_title(const char* path, em_title_info* out) {
    if (!path || !out) return false;
    std::memset(out, 0, sizeof(*out));
    const fs::path p(path);
    std::error_code ec;
    std::string error;

    if (fs::is_directory(p, ec)) {
        fs::path xbePath;
        if (!findDefaultXbe(p, xbePath)) return false;
        out->format     = EM_TITLE_FOLDER;
        out->size_bytes = directorySize(p);
        std::snprintf(out->name, sizeof(out->name), "%s", p.filename().string().c_str());
        FileSource file;
        XbeImage xbe;
        if (file.open(xbePath.string()) && parseXbe(file, xbe, error)) fillFromXbe(xbe, out);
        return true;
    }
    if (!fs::is_regular_file(p, ec)) return false;

    out->size_bytes = fs::file_size(p, ec);
    std::snprintf(out->name, sizeof(out->name), "%s", p.stem().string().c_str());

    FileSource file;
    if (file.open(p.string())) {
        XbeImage xbe;
        if (parseXbe(file, xbe, error)) {
            out->format = EM_TITLE_XBE;
            fillFromXbe(xbe, out);
            return true;
        }
        XisoReader disc;
        if (disc.open(file)) {
            out->format   = EM_TITLE_XISO;
            out->verified = true; // a real XDVDFS volume, even if the executable cannot be read
            XisoEntry entry;
            if (disc.find("default.xbe", entry)) {
                SliceSource inner(file, disc.fileOffset(entry), entry.size);
                if (parseXbe(inner, xbe, error)) fillFromXbe(xbe, out);
            }
            return true;
        }
    }

    const std::string ext = lowerExtension(p);
    if (ext == ".xbe") out->format = EM_TITLE_XBE;
    else if (ext == ".iso" || ext == ".xiso") out->format = EM_TITLE_XISO;
    return out->format != EM_TITLE_UNKNOWN; // verified stays false
}
