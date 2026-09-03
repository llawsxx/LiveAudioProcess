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

#pragma once

#include "libuac.h"
#include "uac_device.h"
#include "uac_parser.h"
#include <mutex>
#include <condition_variable>

namespace uac {

    class uac_stream_handle_impl : public uac_stream_handle {
    public:
        uac_stream_handle_impl(const std::shared_ptr<uac_device_handle_impl>& dev_handle,
                               uint8_t interfaceNr, const uac_altsetting& altsetting,
                               uint8_t clockSourceId = 0,
                               bool clockFrequencyReadable = false,
                               bool clockFrequencyWritable = false);
        ~uac_stream_handle_impl();

        void start(stream_cb_func stream_cb_func, int burst);
        void stop() override;

        void set_sampling_rate(const uint32_t samplingRate) override;

        error_code check_streaming_error() const override;
        uac_stream_stats get_streaming_stats() const override;

        bool is_active() const;

    protected:
        void set_sampling_freq(uint32_t sampling);
        uint32_t get_sampling_freq();

    private:
        static void cb(libusb_transfer *transfer);
        void prepare_output_transfer(libusb_transfer *transfer);
        const std::shared_ptr<uac_device_handle_impl> dev_handle;

        const uac_altsetting& altsetting;
        uint8_t bInterfaceNr;

        stream_cb_func cb_func;

        std::mutex mMutex;
        std::condition_variable mCv;
        int mActiveTransfers;

        uint stride;

        // quirks
        uint offset_stream;

        uint32_t target_sampling_rate;
        uint8_t clockSourceId = 0;
        bool clockFrequencyReadable = false;
        bool clockFrequencyWritable = false;

        std::atomic<bool> active = false;
        std::vector<libusb_transfer*> transfers;

        error_code usbTransferError = UAC_NO_ERROR;
        std::atomic<uint64_t> packetErrors{0};
        std::atomic<uint64_t> emptyPackets{0};
        std::atomic<uint64_t> transferErrors{0};
        std::atomic<uint64_t> outputPacketNumerator{0};
        uint64_t outputPacketStep = 0;
    };
}
