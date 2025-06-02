/**
    @file ConnectionSTREAMEntry.cpp
    @author Lime Microsystems
    @brief Implementation of STREAM board connection.
*/

#include "ConnectionFX3.h"
#include "Logger.h"
#include "threadHelper.h"
#include <cstdlib>

using namespace lime;

#ifdef __unix__
void ConnectionFX3Entry::handle_libusb_events()
{
    if (!ctx) return;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 250000;
    while(mProcessUSBEvents.load() == true)
    {
        int r = libusb_handle_events_timeout_completed(ctx, &tv, NULL);
        if(r != 0) lime::error("error libusb_handle_events %s", libusb_strerror(libusb_error(r)));
    }
}

bool ConnectionFX3Entry::initialize_libusb()
{
    if (ctx) return true; // Already initialized

    // Check if we're in Termux USB FD mode
    const char* termux_fd = std::getenv("TERMUX_USB_FD");
    if (termux_fd) {
        // In Termux mode, disable device discovery
        int r = libusb_init(&ctx);
        if (r < 0) {
            lime::error("Init Error %i", r);
            return false;
        }
        libusb_set_option(ctx, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    } else {
        // Normal mode
        int r = libusb_init(&ctx);
        if (r < 0) {
            lime::error("Init Error %i", r);
            return false;
        }
#if LIBUSBX_API_VERSION < 0x01000106
        libusb_set_debug(ctx, 3);
#else
        libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, 3);
#endif
    }
    return true;
}

void ConnectionFX3Entry::cleanup_libusb()
{
    if (ctx) {
        mProcessUSBEvents.store(false);
        if (mUSBProcessingThread.joinable()) {
            mUSBProcessingThread.join();
        }
        libusb_exit(ctx);
        ctx = nullptr;
    }
}
#endif // __UNIX__

//! make a static-initialized entry in the registry
void __loadConnectionFX3Entry(void) //TODO fixme replace with LoadLibrary/dlopen
{
static ConnectionFX3Entry FX3Entry;
}

ConnectionFX3Entry::ConnectionFX3Entry(const char* connectionName):
    ConnectionRegistryEntry(connectionName)
#ifdef __unix__
    , ctx(nullptr)
#endif
{
#ifdef __unix__
    if (initialize_libusb()) {
        mProcessUSBEvents.store(true);
        mUSBProcessingThread = std::thread(&ConnectionFX3Entry::handle_libusb_events, this);
        SetOSThreadPriority(ThreadPriority::NORMAL, ThreadPolicy::REALTIME, &mUSBProcessingThread);
    }
#endif
}

ConnectionFX3Entry::ConnectionFX3Entry(void):
    ConnectionRegistryEntry("FX3")
#ifdef __unix__
    , ctx(nullptr)
#endif
{
#ifdef __unix__
    if (initialize_libusb()) {
        mProcessUSBEvents.store(true);
        mUSBProcessingThread = std::thread(&ConnectionFX3Entry::handle_libusb_events, this);
        SetOSThreadPriority(ThreadPriority::NORMAL, ThreadPolicy::REALTIME, &mUSBProcessingThread);
    }
#endif
}

ConnectionFX3Entry::~ConnectionFX3Entry(void)
{
#ifdef __unix__
    cleanup_libusb();
#endif
}

std::vector<ConnectionHandle> ConnectionFX3Entry::enumerate(const ConnectionHandle &hint)
{
    std::vector<ConnectionHandle> handles;

#ifndef __unix__
    CCyUSBDevice device;
    if (device.DeviceCount())
    {
        for (int i = 0; i<device.DeviceCount(); ++i)
        {
            if (hint.index >= 0 && hint.index != i)
                continue;
            if (device.IsOpen())
                device.Close();
            device.Open(i);
            ConnectionHandle handle;
            if (device.bSuperSpeed == true)
                handle.media = "USB 3.0";
            else if (device.bHighSpeed == true)
                handle.media = "USB 2.0";
            else
                handle.media = "USB";
            handle.name = device.DeviceName;
            handle.index = i;
            std::wstring ws(device.SerialNumber);
            handle.serial = std::string(ws.begin(),ws.end());
            if (hint.serial.empty() or handle.serial.find(hint.serial) != std::string::npos)
            {
                handles.push_back(handle); //filter on serial
            }
            device.Close();
        }
    }
#else
    // Check if we're in Termux USB FD mode
    const char* termux_fd = std::getenv("TERMUX_USB_FD");
    if (termux_fd) {
        int fd;
        if (sscanf(termux_fd, "%d", &fd) == 1) {
            libusb_device_handle *tempDev_handle(nullptr);
            if (libusb_wrap_sys_device(ctx, (intptr_t)fd, &tempDev_handle) == 0 && tempDev_handle != nullptr) {
                ConnectionHandle handle;
                libusb_device* device = libusb_get_device(tempDev_handle);
                
                // Get device speed
                int speed = libusb_get_device_speed(device);
                if(speed == LIBUSB_SPEED_HIGH)
                    handle.media = "USB 2.0";
                else if(speed == LIBUSB_SPEED_SUPER)
                    handle.media = "USB 3.0";
                else
                    handle.media = "USB";

                // Get device descriptor
                libusb_device_descriptor desc;
                if (libusb_get_device_descriptor(device, &desc) == 0) {
                    // Read device name
                    char data[255];
                    int r = libusb_get_string_descriptor_ascii(tempDev_handle, LIBUSB_CLASS_COMM, (unsigned char*)data, sizeof(data));
                    if (r > 0) handle.name = std::string(data, size_t(r));

                    // Set address as fd:<number>
                    handle.addr = std::string("fd:") + termux_fd;

                    // Get serial number if available
                    if (desc.iSerialNumber > 0) {
                        r = libusb_get_string_descriptor_ascii(tempDev_handle, desc.iSerialNumber, (unsigned char*)data, sizeof(data));
                        if (r > 0) handle.serial = std::string(data, size_t(r));
                    }

                    handles.push_back(handle);
                }
                libusb_close(tempDev_handle);
            }
        }
    } else {
        // Normal device enumeration
        libusb_device **devs;
        int usbDeviceCount = libusb_get_device_list(ctx, &devs);

        if (usbDeviceCount < 0) {
            lime::error("failed to get libusb device list: %s", libusb_strerror(libusb_error(usbDeviceCount)));
            return handles;
        }

        for(int i=0; i<usbDeviceCount; ++i)
        {
            libusb_device_descriptor desc;
            int r = libusb_get_device_descriptor(devs[i], &desc);
            if(r<0)
                lime::error("failed to get device description");
            int pid = desc.idProduct;
            int vid = desc.idVendor;

            if(vid == 1204 && pid == 34323)
            {
                ConnectionHandle handle;
                handle.media = "USB";
                handle.name = "DigiGreen";
                handle.addr = std::to_string(int(pid))+":"+std::to_string(int(vid));
                handles.push_back(handle);
            }
            else if((vid == 1204 && pid == 241) || (vid == 1204 && pid == 243) || (vid == 7504 && pid == 24840))
            {
                libusb_device_handle *tempDev_handle(nullptr);
                if(libusb_open(devs[i], &tempDev_handle) != 0 || tempDev_handle == nullptr)
                    continue;

                ConnectionHandle handle;

                //check operating speed
                int speed = libusb_get_device_speed(devs[i]);
                if(speed == LIBUSB_SPEED_HIGH)
                    handle.media = "USB 2.0";
                else if(speed == LIBUSB_SPEED_SUPER)
                    handle.media = "USB 3.0";
                else
                    handle.media = "USB";

                //read device name
                char data[255];
                r = libusb_get_string_descriptor_ascii(tempDev_handle,  LIBUSB_CLASS_COMM, (unsigned char*)data, sizeof(data));
                if(r > 0) handle.name = std::string(data, size_t(r));

                r = std::sprintf(data, "%.4x:%.4x", int(vid), int(pid));
                if (r > 0) handle.addr = std::string(data, size_t(r));

                if (desc.iSerialNumber > 0)
                {
                    r = libusb_get_string_descriptor_ascii(tempDev_handle,desc.iSerialNumber,(unsigned char*)data, sizeof(data));
                    if(r<0)
                        lime::error("failed to get serial number");
                    else
                        handle.serial = std::string(data, size_t(r));
                }
                libusb_close(tempDev_handle);

                //add handle conditionally, filter by serial number
                if (hint.serial.empty() or handle.serial.find(hint.serial) != std::string::npos)
                {
                    handles.push_back(handle);
                }
            }
        }

        libusb_free_device_list(devs, 1);
    }
#endif
    return handles;
}

IConnection *ConnectionFX3Entry::make(const ConnectionHandle &handle)
{
    return new ConnectionFX3(ctx, handle.addr, handle.serial, handle.index);
}
