/*
 * Copyright (C) 2015  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of MultiBootPatcher
 *
 * MultiBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "utilities.h"

#include <algorithm>

#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>

#include <libmbp/patcherconfig.h>

#include "external/minizip/zip.h"
#include "external/minizip/ioandroid.h"

#include "actions.h"
#include "multiboot.h"
#include "romconfig.h"
#include "roms.h"
#include "util/delete.h"
#include "util/file.h"
#include "util/finally.h"
#include "util/fts.h"
#include "util/logging.h"
#include "util/path.h"
#include "util/properties.h"
#include "util/string.h"

namespace mb
{

static bool wipe_directory_print(const std::string &mountpoint, bool wipe_media)
{
    LOGV("Wiping %s%s", mountpoint.c_str(),
         wipe_media ? "" : " (excluding media directory)");
    bool ret = wipe_directory(mountpoint, wipe_media);
    LOGV("-> %s", ret ? "Succeeded" : "Failed");
    return ret;
}

static bool delete_recursive_print(const std::string &path)
{
    LOGV("Recursively deleting %s", path.c_str());
    bool ret = util::delete_recursive(path);
    LOGV("-> %s", ret ? "Succeeded" : "Failed");
    return ret;
}

static bool switch_rom(const std::string &rom_id)
{
    mbp::PatcherConfig pc;

    std::string prop_product_device;
    std::string prop_build_product;

    util::get_property("ro.product.device", &prop_product_device, "");
    util::get_property("ro.build.product", &prop_build_product, "");

    LOGD("ro.product.device = %s", prop_product_device.c_str());
    LOGD("ro.build.product = %s", prop_build_product.c_str());

    const mbp::Device *device = nullptr;

    for (const mbp::Device *d : pc.devices()) {
        auto codenames = d->codenames();
        auto it = std::find_if(codenames.begin(), codenames.end(),
                               [&](const std::string &codename) {
            return prop_product_device == codename
                    || prop_build_product == codename;
        });
        if (it != codenames.end()) {
            device = d;
            break;
        }
    }

    if (!device) {
        LOGE("Unknown device: %s", prop_product_device.c_str());
        return false;
    }

    auto block_devs = device->bootBlockDevs();
    if (block_devs.empty()) {
        LOGE("No boot partitions defined");
        return false;
    }

    return action_choose_rom(rom_id, block_devs[0], device->blockDevBaseDirs());
}

static bool wipe_system(const std::string &rom_id)
{
    Roms roms;
    roms.add_builtin();
    roms.add_data_roms();

    auto rom = roms.find_by_id(rom_id);
    if (!rom) {
        return false;
    }

    bool ret = wipe_directory_print(rom->system_path, true);
    // Try removing ROM's /system if it's empty
    remove(rom->system_path.c_str());
    return ret;
}

static bool wipe_cache(const std::string &rom_id)
{
    Roms roms;
    roms.add_builtin();
    roms.add_data_roms();

    auto rom = roms.find_by_id(rom_id);
    if (!rom) {
        return false;
    }

    bool ret = wipe_directory_print(rom->cache_path, true);
    // Try removing ROM's /cache if it's empty
    remove(rom->cache_path.c_str());
    return ret;
}

static bool wipe_data(const std::string &rom_id)
{
    Roms roms;
    roms.add_builtin();
    roms.add_data_roms();

    auto rom = roms.find_by_id(rom_id);
    if (!rom) {
        return false;
    }

    bool ret = wipe_directory_print(rom->data_path, false);
    // Try removing ROM's /data/media and /data if they're empty
    remove((rom->data_path + "/media").c_str());
    remove(rom->data_path.c_str());
    return ret;
}

static bool wipe_dalvik_cache(const std::string &rom_id)
{
    Roms roms;
    roms.add_builtin();
    roms.add_data_roms();

    auto rom = roms.find_by_id(rom_id);
    if (!rom) {
        return false;
    }

    // Most ROMs use /data/dalvik-cache, but some use
    // /cache/dalvik-cache, like the jflte CyanogenMod builds
    std::string data_path(rom->data_path);
    std::string cache_path(rom->cache_path);
    data_path += "/dalvik-cache";
    cache_path += "/dalvik-cache";
    // util::delete_recursive() returns true if the path does not
    // exist (ie. returns false only on errors), which is exactly
    // what we want
    return delete_recursive_print(data_path)
            && delete_recursive_print(cache_path);
}

static bool wipe_multiboot(const std::string &rom_id)
{
    Roms roms;
    roms.add_builtin();
    roms.add_data_roms();

    auto rom = roms.find_by_id(rom_id);
    if (!rom) {
        return false;
    }

    // Delete /data/media/0/MultiBoot/[ROM ID]
    std::string multiboot_path("/data/media/0/MultiBoot/");
    multiboot_path += rom->id;
    return delete_recursive_print(multiboot_path);
}

static void generate_aroma_config(std::vector<unsigned char> *data)
{
    std::string str_data(data->begin(), data->end());

    std::string rom_menu_items;
    std::string rom_selection_items;

    Roms roms;
    roms.add_installed();

    for (std::size_t i = 0; i < roms.roms.size(); ++i) {
        const std::shared_ptr<Rom> &rom = roms.roms[i];

        std::string config_path = util::format(
                "/data/media/0/MultiBoot/%s/config.json", rom->id.c_str());
        std::string name = rom->id;

        RomConfig config;
        if (config.load_file(config_path)) {
            name = config.name;
        }

        rom_menu_items += util::format(
                "\"%s\", \"\", \"@default\",\n", name.c_str());

        rom_selection_items += util::format(
                "if prop(\"operations.prop\", \"selected\") == \"%zu\" then\n"
                "    setvar(\"romid\", \"%s\");\n"
                "    setvar(\"romname\", \"%s\");\n"
                "endif;\n",
                i + 2 + 1, rom->id.c_str(), name.c_str());
    }

    util::replace_all(&str_data, "\t", "\\t");
    util::replace_all(&str_data, "@MBTOOL_VERSION@", MBP_VERSION);
    util::replace_all(&str_data, "@ROM_MENU_ITEMS@", rom_menu_items);
    util::replace_all(&str_data, "@ROM_SELECTION_ITEMS@", rom_selection_items);
    util::replace_all(&str_data, "@FIRST_INDEX@", util::format("%d", 2 + 1));
    util::replace_all(&str_data, "@LAST_INDEX@",
                      util::format("%zu", 2 + roms.roms.size()));

    data->assign(str_data.begin(), str_data.end());
}

class AromaGenerator : public util::FTSWrapper {
public:
    AromaGenerator(std::string path, std::string zippath)
        : FTSWrapper(std::move(path), FTS_GroupSpecialFiles),
        _zippath(std::move(zippath))
    {
    }

    virtual bool on_pre_execute() override
    {
        zlib_filefunc64_def zFunc;
        memset(&zFunc, 0, sizeof(zFunc));
        fill_android_filefunc64(&zFunc);
        _zf = zipOpen2_64(_zippath.c_str(), 0, nullptr, &zFunc);
        if (!_zf) {
            LOGE("%s: Failed to open for writing", _zippath.c_str());
            return false;
        }

        return true;
    }

    virtual bool on_post_execute(bool success) override
    {
        (void) success;
        return zipClose(_zf, nullptr) == ZIP_OK;
    }

    virtual int on_reached_file() override
    {
        std::string name = std::string(_curr->fts_path).substr(_path.size() + 1);
        LOGD("%s -> %s", _curr->fts_path, name.c_str());

        if (name == "META-INF/com/google/android/aroma-config.in") {
            std::vector<unsigned char> data;
            if (!util::file_read_all(_curr->fts_accpath, &data)) {
                LOGE("Failed to read: %s", _curr->fts_path);
                return false;
            }

            generate_aroma_config(&data);

            name = "META-INF/com/google/android/aroma-config";
            bool ret = add_file(name, data);
            return ret ? Action::FTS_OK : Action::FTS_Fail;
        } else {
            bool ret = add_file(name, _curr->fts_accpath);
            return ret ? Action::FTS_OK : Action::FTS_Fail;
        }
    }

    virtual int on_reached_symlink() override
    {
        LOGW("Ignoring symlink when creating zip: %s", _curr->fts_path);
        return Action::FTS_OK;
    }

    virtual int on_reached_special_file() override
    {
        LOGW("Ignoring special file when creating zip: %s", _curr->fts_path);
        return Action::FTS_OK;
    }

private:
    zipFile _zf;
    std::string _zippath;

    bool add_file(const std::string &name,
                  const std::vector<unsigned char> &contents)
    {
        // Obviously never true, but we'll keep it here just in case
        bool zip64 = (uint64_t) contents.size() >= ((1ull << 32) - 1);

        zip_fileinfo zi;
        memset(&zi, 0, sizeof(zi));

        int ret = zipOpenNewFileInZip2_64(
            _zf,                    // file
            name.c_str(),           // filename
            &zi,                    // zip_fileinfo
            nullptr,                // extrafield_local
            0,                      // size_extrafield_local
            nullptr,                // extrafield_global
            0,                      // size_extrafield_global
            nullptr,                // comment
            Z_DEFLATED,             // method
            Z_DEFAULT_COMPRESSION,  // level
            0,                      // raw
            zip64                   // zip64
        );

        if (ret != ZIP_OK) {
            LOGW("minizip: Failed to add file (error code: %d): [memory]", ret);

            return false;
        }

        // Write data to file
        ret = zipWriteInFileInZip(_zf, contents.data(), contents.size());
        if (ret != ZIP_OK) {
            LOGW("minizip: Failed to write data (error code: %d): [memory]", ret);
            zipCloseFileInZip(_zf);

            return false;
        }

        zipCloseFileInZip(_zf);

        return true;
    }

    bool add_file(const std::string &name, const std::string &path)
    {
        // Copy file into archive
        int fd = open64(path.c_str(), O_RDONLY);
        if (fd < 0) {
            LOGE("%s: Failed to open for reading: %s",
                 path.c_str(), strerror(errno));
            return false;
        }

        struct stat sb;
        if (fstat(fd, &sb) < 0) {
            LOGE("%s: Failed to stat: %s",
                 path.c_str(), strerror(errno));
            return false;
        }

        uint64_t size;
        lseek64(fd, 0, SEEK_END);
        size = lseek64(fd, 0, SEEK_CUR);
        lseek64(fd, 0, SEEK_SET);

        bool zip64 = size >= ((1ull << 32) - 1);

        zip_fileinfo zi;
        memset(&zi, 0, sizeof(zi));
        zi.external_fa = (sb.st_mode & 0777) << 16;

        int ret = zipOpenNewFileInZip2_64(
            _zf,                    // file
            name.c_str(),           // filename
            &zi,                    // zip_fileinfo
            nullptr,                // extrafield_local
            0,                      // size_extrafield_local
            nullptr,                // extrafield_global
            0,                      // size_extrafield_global
            nullptr,                // comment
            Z_DEFLATED,             // method
            Z_DEFAULT_COMPRESSION,  // level
            0,                      // raw
            zip64                   // zip64
        );

        if (ret != ZIP_OK) {
            LOGW("minizip: Failed to add file (error code: %d): %s",
                 ret, path.c_str());
            return false;
        }

        // Write data to file
        char buf[32768];
        ssize_t n;

        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            ret = zipWriteInFileInZip(_zf, buf, n);
            if (ret != ZIP_OK) {
                LOGW("minizip: Failed to write data (error code: %d): %s",
                     ret, path.c_str());
                zipCloseFileInZip(_zf);

                return false;
            }
        }

        if (n < 0) {
            zipCloseFileInZip(_zf);

            LOGE("%s: Failed to read file: %s",
                 path.c_str(), strerror(errno));
            return false;
        }

        zipCloseFileInZip(_zf);

        return true;
    }
};

static void utilities_usage(bool error)
{
    FILE *stream = error ? stderr : stdout;

    fprintf(stream,
            "Usage: utilities generate [template dir] [output file]\n"
            "   OR: utilities switch [ROM ID]\n"
            "   OR: utilities wipe-system [ROM ID]\n"
            "   OR: utilities wipe-cache [ROM ID]\n"
            "   OR: utilities wipe-data [ROM ID]\n"
            "   OR: utilities wipe-dalvik-cache [ROM ID]\n"
            "   OR: utilities wipe-multiboot [ROM ID]\n");
}

int utilities_main(int argc, char *argv[])
{
    // Make stdout unbuffered
    setvbuf(stdout, nullptr, _IONBF, 0);

    util::log_set_logger(std::make_shared<util::StdioLogger>(stdout, false));

    int opt;

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int long_index = 0;

    while ((opt = getopt_long(argc, argv, "h", long_options, &long_index)) != -1) {
        switch (opt) {
        case 'h':
            utilities_usage(false);
            return EXIT_SUCCESS;

        default:
            utilities_usage(true);
            return EXIT_FAILURE;
        }
    }

    if (argc - optind == 0) {
        utilities_usage(true);
        return EXIT_FAILURE;
    }

    const std::string action = argv[optind];
    if ((action == "generate" && argc - optind != 3)
            || (action != "generate" && argc - optind != 2)) {
        utilities_usage(true);
        return EXIT_FAILURE;
    }

    bool ret = false;

    if (action == "generate") {
        AromaGenerator gen(argv[optind + 1], argv[optind + 2]);
        ret = gen.run();
    } else if (action == "switch") {
        ret = switch_rom(argv[optind + 1]);
    } else if (action == "wipe-system") {
        ret = wipe_system(argv[optind + 1]);
    } else if (action == "wipe-cache") {
        ret = wipe_cache(argv[optind + 1]);
    } else if (action == "wipe-data") {
        ret = wipe_data(argv[optind + 1]);
    } else if (action == "wipe-dalvik-cache") {
        ret = wipe_dalvik_cache(argv[optind + 1]);
    } else if (action == "wipe-multiboot") {
        ret = wipe_multiboot(argv[optind + 1]);
    } else {
        LOGE("Unknown action: %s", action.c_str());
    }

    return ret ? EXIT_SUCCESS : EXIT_FAILURE;
}

}