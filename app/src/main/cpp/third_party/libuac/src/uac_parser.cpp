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

#include "uac_parser.h"
#include "logging.h"
#include "uac_context.h"
#include "uac_exceptions.h"
#include <algorithm>
#include <list>
#include <sstream>
#include <utility>

namespace uac {

    class uac_config_desc {
        libusb_config_descriptor *config = nullptr;
    public:
        explicit uac_config_desc(libusb_device *udev) {
            int errval = libusb_get_active_config_descriptor(udev, &config);
            if (errval != LIBUSB_SUCCESS) {
                errval = libusb_get_config_descriptor(udev, 0, &config);
                if (errval != LIBUSB_SUCCESS) {
                    throw usb_exception_impl("libusb_get_config_descriptor()", (libusb_error) errval);
                }
            }
        }
        ~uac_config_desc() {
            libusb_free_config_descriptor(config);
        }
        libusb_config_descriptor* operator->() {
            return config;
        }
    };
    

    static std::unique_ptr<uac_audiocontrol> parse_audiocontrol(const libusb_interface_descriptor *ifdesc);
    
    static void scan_audiostreaming(uac_audiocontrol& ac, const libusb_interface *usbintf);
    static void parse_audiostreaming_intf(uac_stream_if_impl &stream_if, const libusb_interface_descriptor *altsettings, int num_altsetting, bool uac2);

    std::unique_ptr<uac_audiocontrol> uac_scan_device(libusb_device *udev) {
        uac_config_desc configDesc(udev);
        std::unique_ptr<uac_audiocontrol> audiocontrol;
        for (size_t i = 0; i < configDesc->bNumInterfaces; ++i) {
            auto intf_desc = configDesc->interface[i].altsetting;
            if (intf_desc->bInterfaceClass == LIBUSB_CLASS_AUDIO) {
                LOG_DEBUG("found AUDIO Class interface, subclass=0x%x, protocol=%d", intf_desc->bInterfaceSubClass, intf_desc->bInterfaceProtocol);
                switch (intf_desc->bInterfaceSubClass) {
                case uac_subclass_code::UAC_SUBCLASS_AUDIOCONTROL:
                    audiocontrol = parse_audiocontrol(intf_desc);
                    break;
                case uac_subclass_code::UAC_SUBCLASS_AUDIOSTREAMING:
                    if (!audiocontrol) {
                        // we expect the AudioControl interface before any AudioStreaming interfaces
                        throw invalid_device_exception();
                    }
                    scan_audiostreaming(*audiocontrol, &configDesc->interface[i]);
                    break;
                default:
                    break;
                }
            }
        }
        if (!audiocontrol) {
            // this is not a valid USB Audio Class device
            throw invalid_device_exception();
        }
        audiocontrol->highSpeed = libusb_get_device_speed(udev) >= LIBUSB_SPEED_HIGH;
        for (auto &stream : audiocontrol->streams) stream.highSpeed = audiocontrol->highSpeed;
        return audiocontrol;
    }

    std::unique_ptr<uac_audiocontrol> parse_audiocontrol(const libusb_interface_descriptor *ifdesc) {
        auto data = ifdesc->extra;
        int remaining = ifdesc->extra_length;

        if (data == nullptr || remaining < 3) {
            LOG_ERROR("no extra data available for a given interface: bInterfaceNumber=%d", ifdesc->bInterfaceNumber);
            return nullptr;
        }
        int descSize = data[0];
        int descriptorType = data[1];
        int subtype = data[2];
        if (descSize < 3 || descSize > remaining || subtype != UAC_AC_HEADER || descSize < 8) {
            LOG_ERROR("expected a HEADER first but got an invalid descriptor sizeof(%d) %d:%d", descSize, descriptorType, subtype);
            return nullptr;
        }
        if (TO_WORD(data + 3) >= 0x0200 && descSize < 9) {
            LOG_ERROR("UAC2 HEADER descriptor too short: %d", descSize);
            return nullptr;
        }
        LOG_DEBUG("got HEADER descriptor. sizeof(%d)", descSize);
        auto audiocontrol = std::make_unique<uac_audiocontrol>(ifdesc->bInterfaceNumber, ifdesc->iInterface);
        parse_ac_header(*audiocontrol, data, descSize);

        if (audiocontrol->wTotalLength != remaining) {
            LOG_WARN("wTotalLength mismatch with actual data available: %d != %d", audiocontrol->wTotalLength, remaining);
        }
        
        remaining -= descSize;
        data += descSize;

        // parse other descriptors
        while (remaining >= 3) {
            descSize = data[0];
            descriptorType = data[1];
            subtype = data[2];
            if (descSize < 3 || remaining < descSize) {
                LOG_WARN("Bad descriptor size, exceeds remaining bytes %d < %d", remaining, descSize);
                break;
            }
            LOG_DEBUG("got descriptor sizeof(%d) %d:%d", descSize, descriptorType, subtype);
            switch (subtype) {
            case UAC_AC_HEADER:
                LOG_DEBUG("got another HEADER descriptor. A bug or buggy device?");
                break;
            case UAC_AC_INPUT_TERMINAL:
                if (descSize >= (audiocontrol->uac2 ? 17 : 12))
                    audiocontrol->inputTerminals.push_back(parse_input_terminal(data, descSize, audiocontrol->uac2));
                else
                    LOG_WARN("Input terminal descriptor too short: %d", descSize);
                break;
            case UAC_AC_OUTPUT_TERMINAL:
                if (descSize >= (audiocontrol->uac2 ? 12 : 9))
                    audiocontrol->outputTerminals.push_back(parse_output_terminal(data, descSize, audiocontrol->uac2));
                else
                    LOG_WARN("Output terminal descriptor too short: %d", descSize);
                break;
            case UAC_AC_MIXER_UNIT:
                audiocontrol->units.push_back(parse_mixer_unit(data, descSize));
                break;
            case UAC_AC_FEATURE_UNIT:
                if (descSize >= (audiocontrol->uac2 ? 10 : 7) &&
                    (audiocontrol->uac2 || data[5] != 0))
                    audiocontrol->units.push_back(parse_feature_unit(data, descSize, audiocontrol->uac2));
                else
                    LOG_WARN("Feature unit descriptor too short: %d", descSize);
                break;
            case 0x0A: // UAC2 CLOCK_SOURCE
                if (audiocontrol->uac2 && descSize >= 8) {
                    uac_clock_source clock;
                    clock.bClockID = data[3];
                    clock.bmAttributes = data[4];
                    clock.bmControls = data[5];
                    audiocontrol->clockSources.push_back(clock);
                }
                break;
            
            default:
                LOG_DEBUG("Unsupported AC descriptor: %d, size=%d", subtype, descSize);
                break;
            }
            remaining -= descSize;
            data += descSize;
        }

        audiocontrol->configure_audio_function();
        return audiocontrol;
    }

    void scan_audiostreaming(uac_audiocontrol& ac, const libusb_interface *usbintf) {
        for (auto &&stream : ac.streams) {
            auto ifdesc = usbintf->altsetting;
            if (stream.bInterfaceNr == ifdesc->bInterfaceNumber) {
                LOG_DEBUG("parse AS interface %d", ifdesc->bInterfaceNumber);
                parse_audiostreaming_intf(stream, usbintf->altsetting, usbintf->num_altsetting, ac.uac2);
                return;
            }
        }
        if (ac.uac2) {
            auto ifdesc = usbintf->altsetting;
            LOG_DEBUG("parse unlisted AudioStreaming interface %d", ifdesc->bInterfaceNumber);
            auto found = std::find_if(ac.streams.begin(), ac.streams.end(),
                                      [ifdesc](const uac_stream_if_impl& stream) {
                                          return stream.bInterfaceNr == ifdesc->bInterfaceNumber;
                                      });
            if (found == ac.streams.end()) {
                ac.streams.emplace_back(ifdesc->bInterfaceNumber);
                parse_audiostreaming_intf(ac.streams.back(), usbintf->altsetting, usbintf->num_altsetting, ac.uac2);
            }
            return;
        }
        LOG_DEBUG("This AudioStreaming interface is not part of current AudioControl.");
    }

    void uac_audiocontrol::configure_audio_function() {
        for (auto &&terminal : outputTerminals) {
            auto route = build_audio_topology(terminal);
            audioFunctionTopology.push_back(route);
        }
    }

    uac_audio_route_impl uac_audiocontrol::build_audio_topology(std::shared_ptr<uac_output_terminal> outputTerminal) {
        std::stringstream logStream;
        auto outputEntity = std::make_shared<uac_topology_entity>(outputTerminal);

        std::list<uac_topology_entity*> entities;
        std::string verbose;
        entities.push_back(outputEntity.get());
        logStream << "out " << (int) outputTerminal->bTerminalID;
        while (!entities.empty()) {
            auto entity = entities.front();
            entities.pop_front();
            for (auto sourceId : entity->source_ids()) {
                // check unit first
                auto unit = find_unit(sourceId);
                if (unit != nullptr) {
                    logStream << " < unit " << (int) unit->bUnitID;
                    entity = entity->link_source(unit);
                    entities.push_back(entity);
                } else {
                    // then maybe it's an input terminal
                    auto inTerminal = find_input_terminal(sourceId);
                    if (inTerminal != nullptr) {
                        logStream << " < in " << (int) inTerminal->bTerminalID;
                        entity = entity->link_source(inTerminal);
                    } else {
                        logStream << " <- This topology looks invalid, not ending with the Terminal.";
                    }
                }
            }
        }
        LOG_DEBUG("audio route chain : %s", logStream.str().c_str());
        return {outputEntity};
    }

    std::shared_ptr<uac_unit> uac_audiocontrol::find_unit(int id) {
        for (auto &&unit : units) {
            if (unit->bUnitID == id) return unit;
        }
        return {}; // empty
    }

    std::shared_ptr<uac_input_terminal> uac_audiocontrol::find_input_terminal(int id) {
        for (auto &&terminal : inputTerminals) {
            if (terminal->bTerminalID == id) return terminal;
        }
        return {}; // empty
    }

    uac_topology_entity::uac_topology_entity(std::shared_ptr<uac_unit> unit) : unit(std::move(unit)) {}

    uac_topology_entity::uac_topology_entity(std::shared_ptr<uac_input_terminal> inTerminal) : inTerminal(std::move(inTerminal)) {}

    uac_topology_entity::uac_topology_entity(std::shared_ptr<uac_output_terminal> outTerminal) : outTerminal(std::move(outTerminal)) {}

    uac_topology_entity::~uac_topology_entity() {
        for (auto &&i : sources) {
            delete i;
        }
    }

    std::vector<int> uac_topology_entity::source_ids() const {
        auto ids = std::vector<int>();
        if (outTerminal != nullptr) {
            ids.push_back(outTerminal->bSourceID);
        } else if (unit != nullptr) {
            if (unit->unitType == UAC_AC_FEATURE_UNIT) {
                uac_feature_unit *ftunit = static_cast<uac_feature_unit*>(unit.get());
                ids.push_back(ftunit->bSourceId);
            }
        }
        return ids;
    }

    uac_topology_entity* uac_topology_entity::link_source(std::shared_ptr<uac_unit> srcUnit) {
        auto entity = new uac_topology_entity(std::move(srcUnit));
        entity->sink = this;
        sources.push_back(entity);
        return entity;
    }

    uac_topology_entity* uac_topology_entity::link_source(std::shared_ptr<uac_input_terminal> terminal) {
        auto entity = new uac_topology_entity(std::move(terminal));
        entity->sink = this;
        sources.push_back(entity);
        return entity;
    }

    void parse_ac_header(uac_audiocontrol& ac, const uint8_t *data, int size) {
        ac.bcdADC = TO_WORD(data+3);
        /* UAC1: bcdADC, wTotalLength, bInCollection start at offsets 3/5/7.
         * UAC2 inserts bCategory before wTotalLength, shifting the latter
         * fields by one byte. */
        const bool uac2 = ac.bcdADC >= 0x0200;
        ac.uac2 = uac2;
        ac.wTotalLength = TO_WORD(data + (uac2 ? 6 : 5));
        if (uac2) return;
        uint8_t bInCollection = data[7];
        for (size_t i = 0; i < bInCollection; ++i) {
            ac.streams.emplace_back(data[8 + i]);
            LOG_DEBUG("\t got Audio Streaming interface at: %d", ac.streams.back().bInterfaceNr);
        }
    }

    std::shared_ptr<uac_input_terminal> parse_input_terminal(const uint8_t *data, int size, bool uac2) {
        auto terminal = std::make_shared<uac_input_terminal>();
        terminal->bTerminalID = data[3];
        terminal->wTerminalType = TO_WORD(data+4);
        terminal->bAssocTerminal = data[6];
        if (uac2) {
            // UAC2 adds a clock-source ID and widens channel count/configuration.
            terminal->bCSourceID = size > 7 ? data[7] : 0;
            terminal->bNrChannels = data[8];
            // The public UAC1-compatible structure retains the low 16 bits.
            terminal->wChannelConfig = TO_WORD(data + 9);
            terminal->iChannelNames = data[13];
            terminal->iTerminal = data[16];
        } else {
            terminal->bNrChannels = data[7];
            terminal->wChannelConfig = TO_WORD(data+8);
            terminal->iChannelNames = data[10];
            terminal->iTerminal = data[11];
        }
        LOG_DEBUG("\t got INPUT_TERMINAL %d: type=0x%x", terminal->bTerminalID, terminal->wTerminalType);
        return terminal;
    }

    std::shared_ptr<uac_output_terminal> parse_output_terminal(const uint8_t *data, int size, bool uac2) {
        auto terminal = std::make_shared<uac_output_terminal>();
        terminal->bTerminalID = data[3];
        terminal->wTerminalType = TO_WORD(data+4);
        terminal->bAssocTerminal = data[6];
        terminal->bSourceID = data[7];
        if (uac2) {
            terminal->bCSourceID = size > 8 ? data[8] : 0;
            terminal->iTerminal = size > 11 ? data[11] : 0;
        } else {
            terminal->iTerminal = data[8];
        }
        LOG_DEBUG("\t got OUTPUT_TERMINAL %d: type=0x%x", terminal->bTerminalID, terminal->wTerminalType);
        return terminal;
    }

    std::shared_ptr<uac_mixer_unit> parse_mixer_unit(const uint8_t *data, int size) {
        auto unit = std::make_shared<uac_mixer_unit>();
        unit->unitType = (uac_ac_descriptor_subtype) data[2];
        unit->bUnitID = data[3];
        return unit;
    }

    std::shared_ptr<uac_feature_unit> parse_feature_unit(const uint8_t *data, int size, bool uac2) {
        auto unit = std::make_unique<uac_feature_unit>();
        unit->unitType = (uac_ac_descriptor_subtype) data[2];
        unit->bUnitID = data[3];
        unit->bSourceId = data[4];
        // UAC2 stores four bytes per control; UAC1 carries bControlSize here.
        unit->bControlSize = uac2 ? 4 : data[5];
        if (unit->bControlSize != 0 && size > 5) {
            const int controlsOffset = 6;
            const int available = std::max(0, size - controlsOffset);
            const int width = std::min<int>(unit->bControlSize, 4);
            if (available >= width) {
                for (int i = 0; i < width; ++i)
                    unit->masterControls |= static_cast<uint32_t>(data[controlsOffset + i]) << (8 * i);
                const int stride = unit->bControlSize;
                const int count = uac2
                        ? ((size >= 6 && stride > 0) ? (size - 6) / stride - 1 : 0)
                        : ((size >= 7 && stride > 0) ? (size - 7) / stride - 1 : 0);
                unit->channelCount = static_cast<uint8_t>(std::max(0, count));
            }
        }
        LOG_DEBUG("\t got FEATURE_UNIT %d: bSourceId=0x%x", unit->bUnitID, unit->bSourceId);
        return unit;
    }

    uac_format_type_1* parse_as_format_type_1_3(const uint8_t *data, int size, bool uac2) {
        // UAC2 Type I has a fixed six-byte descriptor; channels live in
        // AS_GENERAL and rates are supplied by the linked clock source.
        const bool truncated = size < 8;
        uint8_t bSamFreqType = (uac2 || truncated) ? 0 : data[7];
        uac_format_type_1 *desc = (uac_format_type_1*) malloc(
                sizeof(uac_format_type_1) + sizeof(uint32_t) * bSamFreqType);
        desc->bFormatType = (uac_format_type) data[3];
        if (uac2) {
            desc->bNrChannels = 0; // filled from the UAC2 AS_GENERAL descriptor
            desc->bSubframeSize = size > 4 ? data[4] : 0;
            desc->bBitResolution = data[5];
        } else {
            desc->bNrChannels = size > 4 ? data[4] : 0;
            desc->bSubframeSize = size > 5 ? data[5] : 0;
            desc->bBitResolution = size > 6 ? data[6] : 0;
        }
        if (truncated && !uac2) {
            // This non-compliant device uses the six-byte descriptor form
            // [format, channels, bit-resolution]; derive byte subframe size.
            desc->bBitResolution = desc->bSubframeSize;
            desc->bSubframeSize = (uint8_t)std::max(1, (int)desc->bBitResolution / 8);
        }
        desc->bSamFreqType = bSamFreqType;
        LOG_DEBUG("AS FORMAT I: len=%d ch=%u subframe=%u bits=%u freqType=%u", size,
                  desc->bNrChannels, desc->bSubframeSize, desc->bBitResolution, bSamFreqType);
        if (desc->bSamFreqType == 0) {
            if (uac2) {
                // The exact UAC2 range is a runtime Clock Source GET_RANGE
                // property, not part of this descriptor.
                desc->tLowerSamFreq = 8000;
                desc->tUpperSamFreq = 384000;
            } else {
                if (size < 14) {
                    desc->tLowerSamFreq = 8000;
                    desc->tUpperSamFreq = 384000;
                } else {
                    desc->tLowerSamFreq = TO_DWORD24(data + 8);
                    desc->tUpperSamFreq = TO_DWORD24(data + 11);
                }
            }
            LOG_DEBUG("AS FORMAT I continuous rates: %u..%u", desc->tLowerSamFreq, desc->tUpperSamFreq);
        } else {
            desc->tLowerSamFreq = 0;
            desc->tUpperSamFreq = 0;
            for (size_t i = 0; i < desc->bSamFreqType; ++i) {
                desc->tSamFreq[i] = uac2 ? TO_DWORD(data + 8 + i*4) : TO_DWORD24(data + 8 + i*3);
                LOG_DEBUG("supported freq %d", desc->tSamFreq[i]);
            }
            
        }
        return desc;
    }

    void parse_as_general(uac_as_general &generalDesc, const uint8_t *data, int size, bool uac2) {
        generalDesc.bTerminalLink = data[3];
        if (uac2) {
            // UAC2 AS_GENERAL: bFormatType is byte 5, bmFormats is bytes 6..9,
            // and the channel cluster follows it. For Type I, bmFormats bit 0
            // denotes PCM.
            generalDesc.bDelay = 0;
            const uint32_t formats = size >= 10 ? (uint32_t)TO_DWORD(data + 6) : 0;
            generalDesc.wFormatTag = (formats & 0x01u) != 0
                    ? UAC_FORMAT_DATA_PCM
                    : UAC_FORMAT_DATA_TYPE_I_UNDEFINED;
            generalDesc.bNrChannels = size >= 11 ? data[10] : 0;
            generalDesc.bmChannelConfig = size >= 15 ? (uint32_t)TO_DWORD(data + 11) : 0;
        } else {
            generalDesc.bDelay = data[4];
            generalDesc.wFormatTag = (uac_audio_data_format_type) TO_WORD(data+5);
        }
    }

    std::unique_ptr<uac_format_type_desc> parse_as_format_type(const uint8_t *data, int size, bool uac2) {
        std::unique_ptr<uac_format_type_desc> format;
        uint8_t bFormatType = data[3];
        switch (bFormatType) {
        case UAC_FORMAT_TYPE_I:
        case UAC_FORMAT_TYPE_III:
            format = std::unique_ptr<uac_format_type_desc>(parse_as_format_type_1_3(data, size, uac2));
            break;
        
        default:
            format = std::unique_ptr<uac_format_type_desc>((uac_format_type_desc*)malloc(sizeof(uac_format_type_desc)));
            format->bFormatType = static_cast<uac_format_type>(bFormatType);
            break;
        }
        return format;
    }

    void parse_iso_ep(iso_endpoint_desc& desc, const uint8_t *data, int size) {
        int remaining = size;
        while (remaining >= 3) {
            int length = data[0];
            if (length < 3 || length > remaining) {
                LOG_WARN("Invalid class-specific endpoint descriptor length=%d remaining=%d", length, remaining);
                break;
            }
            if (data[2] == EP_GENERAL && length >= 7) {
                desc.bmAttributes = data[3];
                desc.bLockDelayUnits = data[4];
                desc.wLockDelay = TO_WORD(data + 5);
            }
            data += length;
            remaining -= length;
        }
        
    }

    void parse_audiostreaming_intf(uac_stream_if_impl &stream_if, const libusb_interface_descriptor *altsettings, int num_altsetting, bool uac2) {
        stream_if.uac2 = uac2;
        for (size_t i = 0; i < num_altsetting; ++i) {
            auto ifdesc = &altsettings[i];
            // The zero-bandwidth alternate setting has no data endpoint.
            if (ifdesc->bAlternateSetting == 0 && ifdesc->bNumEndpoints == 0)
                continue;
            LOG_DEBUG("parsing altsetting=%d descriptor...", ifdesc->bAlternateSetting);
            auto& altsetting = stream_if.altsettings.emplace_back();
            auto data = ifdesc->extra;
            int remaining = ifdesc->extra_length;

            altsetting.bAlternateSetting = ifdesc->bAlternateSetting;
            altsetting.uac2 = uac2;

            bool hasGeneralDescriptor = false;
            bool hasFormatDescriptor = false;
            while (remaining >= 3) {
                int descSize = data[0];
                if (descSize < 3 || descSize > remaining) {
                    LOG_WARN("Bad AS descriptor size %d (remaining %d)", descSize, remaining);
                    break;
                }
                auto subtype = data[2];
                switch (subtype) {
                case UAC_AS_GENERAL:
                    LOG_DEBUG("got AS_GENERAL descriptor");
                    parse_as_general(altsetting.general, data, descSize, uac2);
                    hasGeneralDescriptor = true;
                    break;
                case UAC_AS_FORMAT_TYPE:
                    if (descSize >= 6) {
                        LOG_DEBUG("got AS_FORMAT_TYPE descriptor");
                        altsetting.formatTypeDesc = parse_as_format_type(data, descSize, uac2);
                        hasFormatDescriptor = true;
                    } else {
                        LOG_WARN("AS_FORMAT_TYPE descriptor too short: %d", descSize);
                    }
                    break;
                case UAC_AS_FORMAT_SPECIFIC:
                    LOG_DEBUG("got AS_FORMAT_SPECIFIC descriptor");
                    break;
                default:
                    break;
                }
                remaining -= descSize;
                data += descSize;
            }

            if (uac2 && hasGeneralDescriptor && hasFormatDescriptor) {
                auto *format = const_cast<uac_format_type_1 *>(altsetting.getFormatType1());
                if (format != nullptr && altsetting.general.bNrChannels != 0)
                    format->bNrChannels = altsetting.general.bNrChannels;
            }

            if (!hasGeneralDescriptor || !hasFormatDescriptor || ifdesc->bNumEndpoints == 0) {
                stream_if.altsettings.pop_back();
                continue;
            }
            
            const libusb_endpoint_descriptor *dataEndpoint = nullptr;
            for (uint8_t epIndex = 0; epIndex < ifdesc->bNumEndpoints; ++epIndex) {
                const auto *candidate = &ifdesc->endpoint[epIndex];
                const bool isIso = (candidate->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK)
                                   == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
                // Bits 5..4 are the isochronous usage type.  Explicit feedback
                // endpoints (01b) are not audio data endpoints.
                const bool isFeedback = (candidate->bmAttributes & 0x30u) == 0x10u;
                if (isIso && !isFeedback) {
                    dataEndpoint = candidate;
                    break;
                }
            }
            if (dataEndpoint == nullptr) {
                stream_if.altsettings.pop_back();
                LOG_ERROR("No isochronous data endpoint in interface(%d), endpoints=%d",
                          i, ifdesc->bNumEndpoints);
            } else {
                LOG_DEBUG("altsetting endpointAddress=%x, wMaxPacketSize=%d, endpoints=%d",
                          dataEndpoint->bEndpointAddress, dataEndpoint->wMaxPacketSize,
                          ifdesc->bNumEndpoints);
                auto& epDesc = altsetting.endpoint;
                epDesc.bEndpointAddress = dataEndpoint->bEndpointAddress;
                epDesc.bInterval = dataEndpoint->bInterval;
                const uint16_t rawMaxPacket = dataEndpoint->wMaxPacketSize;
                epDesc.wMaxPacketSize = (rawMaxPacket & 0x07ffu) *
                        (uint16_t)(1u + ((rawMaxPacket >> 11) & 0x03u));
                parse_iso_ep(epDesc.iso_desc, dataEndpoint->extra, dataEndpoint->extra_length);
            }
        }
        
    }

    static bool matches_terminals(uint16_t terminalType, uac_terminal_type expected) {
        if (expected == UAC_TERMINAL_ANY) {
            return true;
        } else if ((expected & 0xFF) == 0) {
            return (terminalType & 0xFF00) == expected;
        } else {
            return terminalType == expected;
        }
    }

    uac_audio_route_impl::uac_audio_route_impl(std::shared_ptr<uac_topology_entity> entry) : entry(entry) {
        LOG_DEBUG("construct uac_audio_route_impl %p", this);
    }
    
    uac_topology_entity* uac_audio_route_impl::findInputTerminalByType(uac_topology_entity *entity, uac_terminal_type terminalType) {
        if (entity->inTerminal != nullptr && matches_terminals(entity->inTerminal->wTerminalType, terminalType)) {
            return entity;
        } else {
            for (auto &&e : entity->sources) {
                auto other = findInputTerminalByType(e, terminalType);
                if (other != nullptr) {
                    return other;
                }
            }
            return nullptr;
        }
    }

    bool uac_audio_route_impl::contains_terminal_out(uac_terminal_type terminalType) const {
        return matches_terminals(entry->outTerminal->wTerminalType, terminalType);
    }

    bool uac_audio_route_impl::contains_terminal_in(uac_terminal_type terminalType) const {
        return findInputTerminalByType(entry.get(), terminalType) != nullptr;
    }

    bool uac_audio_route_impl::contains_terminal(uac_terminal_type terminalType) const {
        if (matches_terminals(entry->outTerminal->wTerminalType, terminalType)) {
            return true;
        } else {
            return findInputTerminalByType(entry.get(), terminalType) != nullptr;
        }
    }

    bool uac_altsetting::supportsSampleRate(uint32_t sampleRate) const {
        bool result = false;
        const uac_format_type_1 *format1;
        switch (formatTypeDesc->bFormatType) {
            case UAC_FORMAT_TYPE_I:
            case UAC_FORMAT_TYPE_III:
                format1 = getFormatType1();
                if (format1->bSamFreqType == 0) {
                    result = format1->tLowerSamFreq <= sampleRate &&
                    sampleRate <= format1->tUpperSamFreq;
                } else {
                    for (int i = 0; i < format1->bSamFreqType; ++i) {
                        if (format1->tSamFreq[i] == sampleRate) {
                            result = true;
                            break;
                        }
                    }
                }
                break;
            default:
                break;
        }
        return result;
    }

    bool uac_altsetting::supportsChannelsCount(uint8_t channelsCount) const {
        const uac_format_type_1 *format1;
        switch (formatTypeDesc->bFormatType) {
            case UAC_FORMAT_TYPE_I:
            case UAC_FORMAT_TYPE_III:
                format1 = getFormatType1();
                return format1->bNrChannels == channelsCount;
            default:
                break;
        }
        return false;
    }

    const uac_format_type_1* uac_altsetting::getFormatType1() const {
        if (formatTypeDesc->bFormatType == UAC_FORMAT_TYPE_I
        || formatTypeDesc->bFormatType == UAC_FORMAT_TYPE_III) {
            return reinterpret_cast<uac_format_type_1 *>(formatTypeDesc.get());
        } else {
            return nullptr;
        }
    }
}
