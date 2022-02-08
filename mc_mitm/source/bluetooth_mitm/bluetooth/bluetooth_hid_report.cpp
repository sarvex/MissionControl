/*
 * Copyright (c) 2020-2021 ndeadly
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "bluetooth_hid_report.hpp"
#include "bluetooth_circular_buffer.hpp"
#include "../btdrv_shim.h"
#include "../btdrv_mitm_flags.hpp"
#include "../../utils.hpp"
#include "../../controllers/controller_management.hpp"
#include <mutex>
#include <cstring>

namespace ams::bluetooth::hid::report {

    namespace {

        constexpr size_t bluetooth_sharedmem_size = 0x3000;

        const s32 ThreadPriority = utils::ConvertToUserPriority(17);
        const size_t ThreadStackSize = 0x1000;
        alignas(os::ThreadStackAlignment) uint8_t g_thread_stack[ThreadStackSize];
        os::ThreadType g_thread;

        // This is only required  on fw < 7.0.0
        bluetooth::HidReportEventInfo g_event_info;
        bluetooth::HidEventType g_current_event_type;

        os::SystemEvent g_system_event;
        os::SystemEvent g_system_event_fwd(os::EventClearMode_AutoClear, true);
        os::SystemEvent g_system_event_user_fwd(os::EventClearMode_AutoClear, true);

        os::Event g_init_event(os::EventClearMode_ManualClear);
        os::Event g_report_read_event(os::EventClearMode_AutoClear);

        os::SharedMemory g_real_bt_shmem;
        os::SharedMemory g_fake_bt_shmem(bluetooth_sharedmem_size, os::MemoryPermission_ReadWrite, os::MemoryPermission_ReadWrite);

        bluetooth::CircularBuffer *g_real_buffer;
        bluetooth::CircularBuffer *g_fake_buffer;

        bluetooth::HidReportEventInfo g_fake_report_event_info;

        void EventThreadFunc(void *) {
            WaitInitialized();

            while (true) {
                g_system_event.Wait();
                HandleEvent();
            }
        }

    }

    bool IsInitialized() {
        return g_init_event.TryWait();
    }

    void WaitInitialized(void) {
        g_init_event.Wait();
    }

    void SignalInitialized(void) {
        g_init_event.Signal();
    }

    void SignalReportRead(void) {
        g_report_read_event.Signal();
    }

    os::SharedMemory *GetRealSharedMemory(void) {
        if (hos::GetVersion() < hos::Version_7_0_0)
            return nullptr;

        return &g_real_bt_shmem;
    }

    os::SharedMemory *GetFakeSharedMemory(void) {
        return &g_fake_bt_shmem;
    }

    os::SystemEvent *GetSystemEvent(void) {
        return &g_system_event;
    }

    os::SystemEvent *GetForwardEvent(void) {
        return &g_system_event_fwd;
    }

    os::SystemEvent *GetUserForwardEvent(void) {
        return &g_system_event_user_fwd;
    }

    Result Initialize(void) {
        R_TRY(os::CreateThread(&g_thread,
            EventThreadFunc,
            nullptr,
            g_thread_stack,
            ThreadStackSize,
            ThreadPriority
        ));

        os::StartThread(&g_thread);

        return ams::ResultSuccess();
    }

    void Finalize(void) {
        os::DestroyThread(&g_thread);
    }

    Result MapRemoteSharedMemory(os::NativeHandle handle) {
        g_real_bt_shmem.Attach(bluetooth_sharedmem_size, handle, true);
        g_real_bt_shmem.Map(os::MemoryPermission_ReadWrite);
        g_real_buffer = reinterpret_cast<bluetooth::CircularBuffer *>(g_real_bt_shmem.GetMappedAddress());

        return ams::ResultSuccess();
    }

    Result InitializeReportBuffer(void) {
        g_fake_bt_shmem.Map(os::MemoryPermission_ReadWrite);
        g_fake_buffer = reinterpret_cast<bluetooth::CircularBuffer *>(g_fake_bt_shmem.GetMappedAddress());
        g_fake_buffer->Initialize("HID Report");
        g_fake_buffer->type = bluetooth::CircularBufferType_HidReport;
        g_fake_buffer->_unk3 = 1;

        return ams::ResultSuccess();
    }

    Result WriteHidReportBuffer(const bluetooth::Address *address, const bluetooth::HidReport *report) {
        if (hos::GetVersion() < hos::Version_9_0_0) {
            // Todo: check this may still be necessary
            //g_fake_report_event_info.data_report.v7.size = g_fake_report_event_info.data_report.v7.report.size + 0x11;
            g_fake_report_event_info.data_report.v7.addr = *address;
            std::memcpy(&g_fake_report_event_info.data_report.v7.report, report, report->size + sizeof(report->size));
        }
        else {
            g_fake_report_event_info.data_report.v9.addr = *address;
            std::memcpy(&g_fake_report_event_info.data_report.v9.report, report, report->size + sizeof(report->size));
        }

        if (hos::GetVersion() >= hos::Version_12_0_0)
            g_fake_buffer->Write(BtdrvHidEventType_Data, &g_fake_report_event_info, report->size + 0x11);
        else
            g_fake_buffer->Write(BtdrvHidEventTypeOld_Data, &g_fake_report_event_info, report->size + 0x11);

        g_system_event_fwd.Signal();

        return ams::ResultSuccess();
    }

    Result SendHidReport(const bluetooth::Address *address, const bluetooth::HidReport *report) {
        return btdrvWriteHidData(*address, report);
    }

    /* Only used for < 7.0.0. Newer firmwares read straight from shared memory */
    Result GetEventInfo(bluetooth::HidEventType *type, void *buffer, size_t size) {
        AMS_UNUSED(size);

        while (true) {
            auto packet = g_fake_buffer->Read();
            if (!packet)
                return -1;

            g_fake_buffer->Free();

            if (packet->header.type == 0xff) {
                continue;
            }
            else {
                auto event_info = reinterpret_cast<bluetooth::HidReportEventInfo *>(buffer);

                *type = static_cast<bluetooth::HidEventType>(packet->header.type);
                event_info->data_report.v1.hdr.addr = packet->data.data_report.v7.addr;
                event_info->data_report.v1.hdr.res = 0;
                event_info->data_report.v1.hdr.size = packet->header.size;
                event_info->data_report.v1.addr = packet->data.data_report.v7.addr;
                std::memcpy(&event_info->data_report.v1.report, &packet->data.data_report.v7.report, packet->header.size);
                break;
            }
        }

        return ams::ResultSuccess();
    }

    inline void HandleHidReportEventV1(void) {
        R_ABORT_UNLESS(btdrvGetHidReportEventInfo(&g_event_info, sizeof(bluetooth::HidReportEventInfo), &g_current_event_type));

        switch (g_current_event_type) {
            case BtdrvHidEventTypeOld_Data:
                {
                    auto device = controller::LocateHandler(&g_event_info.data_report.v1.addr);
                    if (!device)
                        return;

                    device->HandleIncomingReport(reinterpret_cast<bluetooth::HidReport *>(&g_event_info.data_report.v1.report));
                }
                break;
            default:
                g_fake_buffer->Write(g_current_event_type, &g_event_info.data_report.v1.report.data, g_event_info.data_report.v1.report.size);
                break;
        }
    }

    inline void HandleHidReportEventV7(void) {
        while (true) {
            auto real_packet = g_real_buffer->Read();
            if (!real_packet)
                break;

            g_real_buffer->Free();

            switch (real_packet->header.type) {
                case 0xff:
                    continue;
                case BtdrvHidEventTypeOld_Data:
                    {
                        auto device = controller::LocateHandler(hos::GetVersion() < hos::Version_9_0_0 ? &real_packet->data.data_report.v7.addr : &real_packet->data.data_report.v9.addr);
                        if (!device)
                            continue;

                        auto report = hos::GetVersion() < hos::Version_9_0_0 ? reinterpret_cast<bluetooth::HidReport *>(&real_packet->data.data_report.v7.report) : &real_packet->data.data_report.v9.report;
                        device->HandleIncomingReport(report);
                    }
                    break;
                default:
                    g_fake_buffer->Write(real_packet->header.type, &real_packet->data, real_packet->header.size);
                    break;
            }
        }
    }

    inline void HandleHidReportEventV12(void) {
        while (true) {
            auto real_packet = g_real_buffer->Read();
            if (!real_packet)
                break;

            g_real_buffer->Free();

            switch (real_packet->header.type) {
                case 0xff:
                    continue;
                case BtdrvHidEventType_Data:
                    {
                        auto device = controller::LocateHandler(&real_packet->data.data_report.v9.addr);
                        if (!device)
                            continue;

                        device->HandleIncomingReport(&real_packet->data.data_report.v9.report);
                    }
                    break;
                default:
                    g_fake_buffer->Write(real_packet->header.type, &real_packet->data, real_packet->header.size);
                    break;
            }
        }
    }

    void HandleEvent(void) {
        if (g_redirect_hid_report_events) {
            g_system_event_user_fwd.Signal();
            g_report_read_event.Wait();
        }

        if (hos::GetVersion() >= hos::Version_12_0_0)
            HandleHidReportEventV12();
        else if (hos::GetVersion() >= hos::Version_7_0_0)
            HandleHidReportEventV7();
        else
            HandleHidReportEventV1();
    }

}
