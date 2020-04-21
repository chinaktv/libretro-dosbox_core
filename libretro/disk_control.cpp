// This is copyrighted software. More information is at the end of this file.
#include "disk_control.h"

#include "control.h"
#include "dos/cdrom.h"
#include "dos/drives.h"
#include "dos_inc.h"
#include "dosbox.h"
#include "libretro_dosbox.h"
#include "util.h"
#include <memory>
#include <vector>

static auto unmount(const std::filesystem::path& path) -> bool;

namespace state {

static std::vector<std::filesystem::path> images;
static unsigned int current_index = 0;
static bool is_ejected = false;

} // namespace state

namespace cb {

static RETRO_CALLCONV auto get_num_images() -> unsigned int
{
    return state::images.size();
}

static RETRO_CALLCONV auto get_eject_state() -> bool
{
    return state::is_ejected;
}

static RETRO_CALLCONV auto get_image_index() -> unsigned int
{
    return state::current_index;
}

static RETRO_CALLCONV auto set_eject_state(const bool ejected) -> bool
{
    state::is_ejected = ejected;
    log_cb(RETRO_LOG_INFO, "[dosbox] Tray %s\n", ejected ? "open" : "close");
    if (get_num_images() == 0) {
        return true;
    }
    if (ejected) {
        return unmount(state::images.at(get_image_index()));
    }
    return disk_control::mount(state::images.at(get_image_index()));
}

static RETRO_CALLCONV auto set_image_index(const unsigned int index) -> bool
{
    if (index >= get_num_images()) {
        return false;
    }

    state::current_index = index;
    log_cb(RETRO_LOG_INFO, "[dosbox] Disk index %u\n", index);
    return true;
}

static RETRO_CALLCONV auto add_image_index() -> bool
{
    state::images.emplace_back();
    log_cb(RETRO_LOG_INFO, "[dosbox] Disk count %u\n", get_num_images());
    return true;
}

static RETRO_CALLCONV auto replace_image_index(
    const unsigned int index, const retro_game_info* const info) -> bool
{
    if (index >= get_num_images()) {
        log_cb(
            RETRO_LOG_WARN, "[dosbox] Frontend tried to replace invalid disk index %u.\n", index);
        return false;
    }

    if (info == nullptr) {
        state::images.erase(state::images.begin() + index);
        if (get_image_index() >= get_num_images() && get_num_images() > 0) {
            set_image_index(get_num_images() - 1);
        }
        return true;
    }
    state::images.at(index) = info->path;
    return true;
}

static RETRO_CALLCONV auto get_image_label(
    const unsigned int index, char* const label, const size_t len) -> bool
{
    if (index > get_num_images()) {
        return false;
    }

    strncpy(label, state::images.at(index).filename().string().c_str(), len);
    label[len - 1] = '\0';
    return true;
}

} // namespace cb

void disk_control::init(const retro_environment_t env_cb)
{
    static retro_disk_control_callback disk_interface{
        cb::set_eject_state, cb::get_eject_state,     cb::get_image_index, cb::set_image_index,
        cb::get_num_images,  cb::replace_image_index, cb::add_image_index,
    };

    static retro_disk_control_ext_callback disk_interface_ext{
        cb::set_eject_state,
        cb::get_eject_state,
        cb::get_image_index,
        cb::set_image_index,
        cb::get_num_images,
        cb::replace_image_index,
        cb::add_image_index,
        nullptr,
        nullptr,
        cb::get_image_label,
    };

    if (!env_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disk_interface_ext)) {
        env_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_interface);
    }
}

static auto mount_floppy_image(const char drive_letter, const std::filesystem::path& path) -> bool
{
    constexpr Bit8u media_id = 0xF0;

    try {
        if (std::filesystem::file_size(path) > 2880 * 1024) {
            log_cb(RETRO_LOG_WARN, "[dosbox] Mounting HDD images is currently not supported.\n");
            return false;
        }
    }
    catch (const std::exception& e) {
        log_cb(RETRO_LOG_WARN, "[dosbox] Failed to detect image file size: %s.\n", e.what());
        return false;
    }

    // Geometry of floppy images is auto-detected so just pass zeros.
    auto floppy = std::make_unique<fatDrive>(path.string().c_str(), 0, 0, 0, 0, 0);
    if (!floppy.get()->created_successfully) {
        log_cb(
            RETRO_LOG_WARN, "[dosbox] Failed to mount drive %c as %s.\n", drive_letter,
            path.string().c_str());
        return false;
    }

    DriveManager::AppendDisk(drive_letter - 'A', floppy.release());
    DriveManager::InitializeDrive(drive_letter - 'A');
    // Set the correct media byte in the table.
    mem_writeb(Real2Phys(dos.tables.mediaid) + (drive_letter - 'A') * 9, media_id);
    // Command uses dta so set it to our internal dta.
    dos.dta(dos.tables.tempdta);
    log_cb(
        RETRO_LOG_INFO, "[dosbox] Drive %c is mounted as %s.\n", drive_letter,
        path.string().c_str());
    return true;
}

static auto mount_cd_image(const char drive_letter, const std::filesystem::path& path) -> bool
{
    constexpr Bit8u media_id = 0xF8;
    int error = -1;

    MSCDEX_SetCDInterface(CDROM_USE_SDL, -1);

    auto iso = std::make_unique<isoDrive>(drive_letter, path.string().c_str(), media_id, error);
    switch (error) {
    case 0:
        break;

    case 1:
        log_cb(RETRO_LOG_WARN, "[dosbox] %s\n", MSG_Get("MSCDEX_ERROR_MULTIPLE_CDROMS"));
        return false;

    case 2:
        log_cb(RETRO_LOG_WARN, "[dosbox] %s\n", MSG_Get("MSCDEX_ERROR_NOT_SUPPORTED"));
        return false;

    case 3:
        log_cb(RETRO_LOG_WARN, "[dosbox] %s\n", MSG_Get("MSCDEX_ERROR_OPEN"));
        return false;

    case 4:
        log_cb(RETRO_LOG_WARN, "[dosbox] %s\n", MSG_Get("MSCDEX_TOO_MANY_DRIVES"));
        return false;

    case 5:
        log_cb(RETRO_LOG_WARN, "[dosbox] %s\n", MSG_Get("MSCDEX_LIMITED_SUPPORT"));
        return false;

    case 6:
        log_cb(RETRO_LOG_WARN, "[dosbox] %s\n", MSG_Get("MSCDEX_INVALID_FILEFORMAT"));
        return false;

    default:
        log_cb(RETRO_LOG_WARN, "[dosbox] %s\n", MSG_Get("MSCDEX_UNKNOWN_ERROR"));
        return false;
    }

    DriveManager::AppendDisk(drive_letter - 'A', iso.release());
    DriveManager::InitializeDrive(drive_letter - 'A');
    // Set the correct media byte in the table.
    mem_writeb(Real2Phys(dos.tables.mediaid) + (drive_letter - 'A') * 9, media_id);
    return true;
}

auto disk_control::mount(std::filesystem::path image) -> bool
{
    if (control->SecureMode()) {
        log_cb(RETRO_LOG_WARN, "[dosbox] This operation is not permitted in secure mode\n");
        return false;
    }

    const auto extension = lower_case(image.extension().string());
    bool mounted_ok = false;
    char drive_letter;

    if (extension == ".img") {
        log_cb(RETRO_LOG_INFO, "[dosbox] Mounting disk as floppy %s\n", image.string().c_str());
        drive_letter = 'A';
        mounted_ok = mount_floppy_image(drive_letter, image);
    } else if (extension == ".iso" || extension == ".cue") {
        log_cb(RETRO_LOG_INFO, "[dosbox] Mounting disk as cdrom %s\n", image.string().c_str());
        drive_letter = 'D';
        mounted_ok = mount_cd_image(drive_letter, image);
    } else {
        log_cb(
            RETRO_LOG_WARN, "[dosbox] Unsupported disk image\n %s",
            image.extension().string().c_str());
        return false;
    }

    if (!mounted_ok) {
        return false;
    }

    for (auto* drive : Drives) {
        if (drive) {
            drive->EmptyCache();
        }
    }
    DriveManager::CycleDisks(drive_letter - 'A', true);
    log_cb(
        RETRO_LOG_INFO, "[dosbox] Drive %c is mounted as %s.\n", drive_letter,
        image.string().c_str());
    if (cb::get_num_images() == 0) {
        cb::add_image_index();
        state::images.at(0) = std::move(image);
    }
    return mounted_ok;
}

static auto unmount(const std::filesystem::path& path) -> bool
{
    char drive_letter;

    if (cb::get_num_images() == 0) {
        log_cb(RETRO_LOG_WARN, "[dosbox] No disks added to index.\n");
        return false;
    }

    if (const auto extension = lower_case(path.extension().string()); extension == ".img") {
        log_cb(RETRO_LOG_INFO, "[dosbox] Unmounting floppy %s.\n", path.string().c_str());
        drive_letter = 'A';
    } else if (extension == ".iso" || extension == ".cue") {
        log_cb(RETRO_LOG_INFO, "[dosbox] Umounting cdrom %s.\n", path.string().c_str());
        drive_letter = 'D';
    } else {
        log_cb(
            RETRO_LOG_WARN, "[dosbox] Unsupported disk image.\n %s",
            path.extension().string().c_str());
        return false;
    }

    if (Drives[drive_letter - 'A']) {
        DriveManager::UnmountDrive(drive_letter - 'A');
    }
    Drives[drive_letter - 'A'] = nullptr;
    DriveManager::CycleDisks(drive_letter - 'A', true);
    return true;
}

/*

Copyright (C) 2002-2019 The DOSBox Team
Copyright (C) 2015-2019 Andrés Suárez
Copyright (C) 2019-2020 Nikos Chantziaras <realnc@gmail.com>

This file is part of DOSBox-core.

DOSBox-core is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 2 of the License, or (at your option) any later
version.

DOSBox-core is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
DOSBox-core. If not, see <https://www.gnu.org/licenses/>.

*/
