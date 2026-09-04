// Copyright 2023 Jakub Księżniak
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//#define VERBOSE_VLOG

#include "uac_context.h"
#include "uac_device.h"

#include <utility>
#include "uac_parser.h"
#include "uac_streaming.h"
#include "logging.h"
#include "uac_exceptions.h"

namespace uac {

    namespace {
        // Locate the Feature Unit on a route.  UAC topologies normally have
        // the feature unit directly below the output terminal, but mixers and
        // selector units may insert additional nodes, so do not assume
        // sources[0] is the volume unit.
        const uac_feature_unit *find_feature_unit(const uac_topology_entity *root) {
            if (!root) return nullptr;
            std::vector<const uac_topology_entity *> pending{root};
            while (!pending.empty()) {
                const auto *entity = pending.back();
                pending.pop_back();
                if (entity->unit && entity->unit->unitType == UAC_AC_FEATURE_UNIT)
                    return static_cast<const uac_feature_unit *>(entity->unit.get());
                for (const auto *source : entity->sources)
                    pending.push_back(source);
            }
            return nullptr;
        }

        int16_t read_le16(const uint8_t *data) {
            const uint16_t value = static_cast<uint16_t>(data[0]) |
                                   (static_cast<uint16_t>(data[1]) << 8);
            return static_cast<int16_t>(value);
        }

        bool feature_has_volume(const uac_feature_unit *feature) {
            return feature && feature->bControlSize != 0 &&
                   (feature->masterControls & (1u << VOLUME_CONTROL)) != 0;
        }
    }

    uac_device_impl::uac_device_impl(std::shared_ptr<uac_context> context, libusb_device *usb_device) : context(std::move(context)), usb_device(usb_device) {
        libusb_device_descriptor desc{};
        libusb_get_device_descriptor(usb_device, &desc);

        LOG_DEBUG("try to scan device: %04x:%04x", desc.idVendor, desc.idProduct);
        audiocontrol = uac_scan_device(usb_device);
        fix_device_quirks(desc);
        libusb_ref_device(usb_device);
    }

    uac_device_impl::~uac_device_impl() {
        LOG_VERBOSE("destructor");
        libusb_unref_device(usb_device);
        usb_device = nullptr;
    }

    void uac_device_impl::fix_device_quirks(libusb_device_descriptor &desc) {
        if (desc.idVendor == 0x534d && (desc.idProduct == 0x2109 || desc.idProduct == 0x0021)) {
            LOG_DEBUG("Apply device quirks!!");
            auto& setting = audiocontrol->streams.back();
            auto format = (uac_format_type_1*)(setting.altsettings[0].formatTypeDesc.get());
            format->bNrChannels = 2;
            format->tSamFreq[0] = 48000;
            quirk_swap_channels = true;
        }
    }

    bool uac_device_impl::hasQuirkSwapChannels() const {
        return quirk_swap_channels;
    }

    uint16_t uac_device_impl::get_vid() const {
        libusb_device_descriptor desc{};
        libusb_get_device_descriptor(usb_device, &desc);
        return desc.idVendor;
    }

    uint16_t uac_device_impl::get_pid() const {
        libusb_device_descriptor desc{};
        libusb_get_device_descriptor(usb_device, &desc);
        return desc.idProduct;
    }

    std::shared_ptr<uac_device_handle> uac_device_impl::open() {
        libusb_device_handle *hDev;
        int errval = libusb_open(usb_device, &hDev);
        if (errval != LIBUSB_SUCCESS) {
            throw usb_exception_impl("libusb_open()", (libusb_error)errval);
        }
        return wrapHandle(hDev);
    }

    std::shared_ptr<uac_device_handle> uac_device_impl::wrapHandle(libusb_device_handle *h_dev) {
        int errval = libusb_set_auto_detach_kernel_driver(h_dev, true);
        if (errval != LIBUSB_SUCCESS) {
            throw usb_exception_impl("wrapHandle()", (libusb_error)errval);
        }
        return std::make_shared<uac_device_handle_impl>(shared_from_this(), h_dev);
    }

    std::vector<ref_uac_audio_route> uac_device_impl::query_audio_routes(uac_terminal_type termIn, uac_terminal_type termOut) const {
        auto& routes = audiocontrol->audio_routes();
        std::vector<ref_uac_audio_route> elems;
        for (auto&& aft : routes) {
            if (aft.contains_terminal_out(termOut) && aft.contains_terminal_in(termIn)) {
                elems.emplace_back(aft);
            }
        }
        return elems;
    }

    const uac_stream_if& uac_device_impl::get_stream_interface(const uac_audio_route& route) const {
        auto route_impl = static_cast<const uac_audio_route_impl&>(route);
        auto route_has_terminal_id = [](const uac_topology_entity *entry, uint8_t terminal_id) {
            std::vector<const uac_topology_entity *> pending{entry};
            while (!pending.empty()) {
                const auto *entity = pending.back();
                pending.pop_back();
                if ((entity->inTerminal && entity->inTerminal->bTerminalID == terminal_id) ||
                    (entity->outTerminal && entity->outTerminal->bTerminalID == terminal_id)) {
                    return true;
                }
                for (const auto *source : entity->sources) pending.push_back(source);
            }
            return false;
        };
        for (auto &stream : audiocontrol->streams) {
            for (auto& alt : stream.altsettings) {
                // Capture interfaces link to the USB streaming output terminal,
                // while playback interfaces link to the USB streaming input
                // terminal. Both terminal IDs belong to the same route.
                if (route_has_terminal_id(route_impl.entry.get(), alt.general.bTerminalLink)) {
                    return stream;
                }
            }
        }
        throw std::out_of_range("missing stream interface for a given route");
    }


    uac_device_handle_impl::uac_device_handle_impl(std::shared_ptr<uac_device_impl> device, libusb_device_handle *usb_handle) : device(std::move(device)), usb_handle(usb_handle) {
        LOG_VERBOSE("constructor");
    }

    uac_device_handle_impl::~uac_device_handle_impl() {
        LOG_VERBOSE("destructor");
        close();
    }

    void uac_device_handle_impl::close() {
        LOG_ENTER();
        if (usb_handle != nullptr) {
            detach();
            LOG_VERBOSE("close %p", usb_handle);
            libusb_close(usb_handle);
            usb_handle = nullptr;
        }
    }

    void uac_device_handle_impl::detach() {
        LOG_ENTER();
        if (usb_handle != nullptr) {
            int bInterfaceNumber = device->audiocontrol->bInterfaceNumber;
            LOG_DEBUG("release AC intf(%d)", bInterfaceNumber);
            libusb_release_interface(usb_handle, bInterfaceNumber);
        }
    }

    std::shared_ptr<uac_stream_handle> uac_device_handle_impl::start_streaming(const uac_stream_if& streamIf, const uac_audio_config_uncompressed& config, stream_cb_func cb_func) {
        return start_streaming(streamIf, config, cb_func, 1);
    }

    std::shared_ptr<uac_stream_handle> uac_device_handle_impl::start_streaming(const uac_stream_if& streamIf, const uac_audio_config_uncompressed& config, stream_cb_func cb_func, int burst) {
        auto* streamIfImpl = static_cast<const uac_stream_if_impl*>(&streamIf);
        
        if (burst < 1) throw std::invalid_argument("invalid burst value");

        auto result = std::find_if(streamIfImpl->altsettings.begin(), streamIfImpl->altsettings.end(), [config](const uac_altsetting& alt) {
            return config.bAlternateSetting == alt.bAlternateSetting;
        });
        if (result == streamIfImpl->altsettings.end()) throw std::invalid_argument("invalid format");
        const auto& altsetting = *result;

        LOG_DEBUG("claim AC intf(%d)", device->audiocontrol->bInterfaceNumber);
        int errval = libusb_claim_interface(usb_handle, device->audiocontrol->bInterfaceNumber);
        if (errval != LIBUSB_SUCCESS) {
            throw usb_exception_impl("libusb_claim_interface()", (libusb_error)errval);
        }

        uint8_t clockSourceId = 0;
        bool clockReadable = false;
        bool clockWritable = false;
        if (device->audiocontrol->uac2) {
            for (const auto &terminal : device->audiocontrol->inputTerminals) {
                if (terminal->bTerminalID == altsetting.general.bTerminalLink) {
                    clockSourceId = terminal->bCSourceID;
                    break;
                }
            }
            if (clockSourceId == 0) {
                for (const auto &terminal : device->audiocontrol->outputTerminals) {
                    if (terminal->bTerminalID == altsetting.general.bTerminalLink) {
                        clockSourceId = terminal->bCSourceID;
                        break;
                    }
                }
            }
            for (const auto &clock : device->audiocontrol->clockSources) {
                if (clock.bClockID == clockSourceId) {
                    clockReadable = clock.frequencyReadable();
                    clockWritable = clock.frequencyWritable();
                    break;
                }
            }
            LOG_DEBUG("UAC2 stream clock source=%u readable=%d writable=%d",
                      clockSourceId, clockReadable, clockWritable);
        }

        auto streamHandle = std::make_shared<uac_stream_handle_impl>(
                shared_from_this(), streamIfImpl->bInterfaceNr, altsetting,
                clockSourceId, clockReadable, clockWritable);
        streamHandle->set_sampling_rate(config.tSampleRate);
        streamHandle->start(cb_func, burst);
        return streamHandle;
    }

    bool uac_device_handle_impl::is_master_muted(const uac_audio_route &route) {
        auto route_impl = static_cast<const uac_audio_route_impl&>(route);
        const int cs = MUTE_CONTROL;
        const int cn = 0;
        const int unit = route_impl.entry->sources[0]->unit->bUnitID;
        uint8_t data = 0;

        int errval = libusb_control_transfer(
            usb_handle,
            REQ_TYPE_IF_GET,
            device->audiocontrol->uac2 ? REQ_CUR : REQ_GET_CUR,
            cs << 8 | cn,
            unit << 8 | device->audiocontrol->bInterfaceNumber,
            &data,
            sizeof(data),
            1000 /* timeout ms */);

        if (errval < 0)
            throw usb_exception_impl("is_master_muted()", (libusb_error)errval);
        if (errval != sizeof(data))
            throw usb_exception_impl("is_master_muted()", LIBUSB_ERROR_IO);
        return data != 0;
    }

    int32_t uac_device_handle_impl::get_feature_master_volume(const uac_audio_route &route) {
        auto route_impl = static_cast<const uac_audio_route_impl&>(route);
        const int cs = VOLUME_CONTROL;
        const int cn = 0;
        const uac_feature_unit *feature = find_feature_unit(route_impl.entry.get());
        if (!feature_has_volume(feature))
            throw usb_exception_impl("get_feature_master_volume", LIBUSB_ERROR_NOT_SUPPORTED);
        const int unit = feature->bUnitID;
        const bool uac2 = device->audiocontrol->uac2;
        uint8_t data[2] = {0, 0};

        int errval = libusb_control_transfer(
            usb_handle,
            REQ_TYPE_IF_GET,
            uac2 ? REQ_CUR : REQ_GET_CUR,
            cs << 8 | cn,
            unit << 8 | device->audiocontrol->bInterfaceNumber,
            data,
            sizeof(data),
            1000 /* timeout ms */);

        if (errval < 0)
            throw usb_exception_impl("get_feature_master_volume", (libusb_error)errval);
        if (errval != sizeof(data))
            throw usb_exception_impl("get_feature_master_volume", LIBUSB_ERROR_IO);
        return read_le16(data);
    }

    bool uac_device_handle_impl::set_feature_master_volume(const uac_audio_route &route, int32_t volume) {
        auto route_impl = static_cast<const uac_audio_route_impl&>(route);
        const uac_feature_unit *feature = find_feature_unit(route_impl.entry.get());
        if (!feature_has_volume(feature)) return false;
        const bool uac2 = device->audiocontrol->uac2;
        uint8_t data[4] = {0, 0, 0, 0};
        const int width = 2;
        const int16_t native = static_cast<int16_t>(std::max(-32768, std::min(32767, volume)));
        data[0] = static_cast<uint8_t>(native);
        data[1] = static_cast<uint8_t>(native >> 8);
        const int errval = libusb_control_transfer(
            usb_handle, REQ_TYPE_IF_SET, uac2 ? REQ_CUR : REQ_SET_CUR,
            VOLUME_CONTROL << 8, feature->bUnitID << 8 | device->audiocontrol->bInterfaceNumber,
            data, width, 1000);
        return errval == width;
    }

    bool uac_device_handle_impl::get_feature_master_volume_range(const uac_audio_route &route,
                                                                  int32_t *min, int32_t *max,
                                                                  int32_t *res) {
        if (!min || !max || !res) return false;
        auto route_impl = static_cast<const uac_audio_route_impl&>(route);
        const uac_feature_unit *feature = find_feature_unit(route_impl.entry.get());
        if (!feature_has_volume(feature)) return false;
        const bool uac2 = device->audiocontrol->uac2;
        const uint16_t value = VOLUME_CONTROL << 8;
        const uint16_t index = feature->bUnitID << 8 | device->audiocontrol->bInterfaceNumber;
        if (uac2) {
            // UAC2 Feature Unit RANGE returns wNumSubRanges followed by signed
            // 8.8 dB min/max/res values (2 bytes each). Clock Source sample
            // frequency RANGE is the separate UAC2 control that uses 4-byte
            // values.
            // min/max/res values for each subrange.
            uint8_t data[8] = {0};
            const int n = libusb_control_transfer(usb_handle, REQ_TYPE_IF_GET, REQ_RANGE,
                                                   value, index, data, sizeof(data), 1000);
            if (n < 8 || (data[0] == 0 && data[1] == 0)) return false;
            auto read16 = [](const uint8_t *p) -> int32_t {
                return static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                                            (static_cast<uint16_t>(p[1]) << 8));
            };
            *min = read16(data + 2); *max = read16(data + 4); *res = read16(data + 6);
            return *max >= *min;
        }
        auto read16 = [&](uint8_t request, int32_t *out) -> bool {
            uint8_t data[2] = {0};
            const int n = libusb_control_transfer(usb_handle, REQ_TYPE_IF_GET, request,
                                                   value, index, data, sizeof(data), 1000);
            if (n != 2) return false;
            *out = static_cast<int16_t>(static_cast<uint16_t>(data[0]) |
                                        (static_cast<uint16_t>(data[1]) << 8));
            return true;
        };
        return read16(REQ_GET_MIN, min) && read16(REQ_GET_MAX, max) && read16(REQ_GET_RES, res) && *max >= *min;
    }

    std::string uac_device_handle_impl::getString(uint8_t index) const {
        std::string name;
        if (index > 0) {
            name.reserve(256);
            int result = libusb_get_string_descriptor_ascii(usb_handle, index, (unsigned char *) name.data(), 256);
            if (result < 0) {
                LOG_WARN("Failed to read string descriptor");
            }
        }
        return name;
    }

    std::string uac_device_handle_impl::get_name() const {
        return getString(device->audiocontrol->iInterface);
    }

    static void dump_format(FILE *f, uac_format_type_desc *format);
    void uac_device_handle_impl::dump(FILE *f) const {
        if (f == nullptr) f = stderr;

        fprintf(f, "--- USB AUDIO DEVICE CONFIGURATION ---\n");

        fprintf(f, "Audio Control:\n");
        fprintf(f, "bcdADC: 0x%04x\n", device->audiocontrol->bcdADC);
        fprintf(f, "bInterfaceNumber: %d\n", device->audiocontrol->bInterfaceNumber);

        if (device->audiocontrol->iInterface == 0) {
            fprintf(f, "iInterface: 0\n");
        } else {
            fprintf(f, "iInterface: %s\n", get_name().c_str());
        }

        fprintf(f, "Input Terminals:\n");
        for (auto&& terminal : device->audiocontrol->inputTerminals) {
            fprintf(f, "- bTerminalID: %d\n", terminal->bTerminalID);
            fprintf(f, "\twTerminalType: 0x%04x\n", terminal->wTerminalType);
            fprintf(f, "\tbAssocTerminal: %d\n", terminal->bAssocTerminal);
            fprintf(f, "\tbNrChannels: %d\n", terminal->bNrChannels);
            fprintf(f, "\twChannelConfig: 0x%04x\n", terminal->wChannelConfig);
            fprintf(f, "\tiTerminal: %d\n", terminal->iTerminal);
        }
        fprintf(f, "Units:\n");
        for (auto&& unit : device->audiocontrol->units) {
            fprintf(f, "- bUnitID: %d\n", unit->bUnitID);
            fprintf(f, "\tunitType: 0x%02x\n", unit->unitType);
            // todo print unit types
        }
        fprintf(f, "Output Terminals:\n");
        for (auto&& terminal : device->audiocontrol->outputTerminals) {
            fprintf(f, "- bTerminalID: %d\n", terminal->bTerminalID);
            fprintf(f, "\twTerminalType: 0x%04x\n", terminal->wTerminalType);
            fprintf(f, "\tbAssocTerminal: %d\n", terminal->bAssocTerminal);
            fprintf(f, "\tbSourceID: %d\n", terminal->bSourceID);
            fprintf(f, "\tiTerminal: %d\n", terminal->iTerminal);
        }

        fprintf(f, "Audio Streams:\n");
        for (auto&& as : device->audiocontrol->streams) {
            fprintf(f, "- bInterfaceNr: %d\n", as.bInterfaceNr);
            for (int i = 0; i < as.altsettings.size(); ++i) {
                auto && altsetting = as.altsettings[i];
                fprintf(f, "\tbAlternateSetting: %d\n", altsetting.bAlternateSetting);
                fprintf(f, "\t  bTerminalLink: %d\n", altsetting.general.bTerminalLink);
                fprintf(f, "\t  wFormatTag: 0x%04x\n", altsetting.general.wFormatTag);
                fprintf(f, "\t  bDelay: %d\n", altsetting.general.bDelay);
                dump_format(f, altsetting.formatTypeDesc.get());
                fprintf(f, "\t  wMaxPacketSize: %d\n", altsetting.endpoint.wMaxPacketSize);
            }
        }
    }

    static void dump_format(FILE *f, uac_format_type_desc *format) {
        fprintf(f, "\t  bFormatType: 0x%02x\n", format->bFormatType);
        uac_format_type_1 *format1;
        switch (format->bFormatType) {
        case UAC_FORMAT_TYPE_I:
        case UAC_FORMAT_TYPE_III:
            format1 = (uac_format_type_1*)format;
            fprintf(f, "\t  bNrChannels: %d\n", format1->bNrChannels);
            fprintf(f, "\t  bSubframeSize: %d\n", format1->bSubframeSize);
            fprintf(f, "\t  bBitResolution: %d\n", format1->bBitResolution);
            fprintf(f, "\t  bSamFreqType: %d\n", format1->bSamFreqType);
            if (format1->bSamFreqType > 0) {
                for (int i = 0; i < format1->bSamFreqType; ++i) {
                    fprintf(f, "\t  tSamFreq[%d]: %d\n", i, format1->tSamFreq[i]);
                }
            } else {
                fprintf(f, "\t  tLowerSamFreq: %d\n", format1->tLowerSamFreq);
                fprintf(f, "\t  tUpperSamFreq: %d\n", format1->tUpperSamFreq);
            }
            break;
        
        default:
            break;
        }
    }
}
