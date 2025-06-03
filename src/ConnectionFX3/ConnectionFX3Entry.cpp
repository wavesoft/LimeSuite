/**
    @file ConnectionSTREAMEntry.cpp
    @author Lime Microsystems
    @brief Implementation of STREAM board connection.
*/

#include "ConnectionFX3.h"
#include "Logger.h"
#include "threadHelper.h"

using namespace lime;

#ifdef __unix__
void ConnectionFX3Entry::handle_libusb_events()
{
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 250000;
    while(mProcessUSBEvents.load() == true)
    {
        int r = libusb_handle_events_timeout_completed(ctx, &tv, NULL);
        if(r != 0) lime::error("error libusb_handle_events %s", libusb_strerror(libusb_error(r)));
    }
}

// Helper function to validate device and create handle
static bool validateAndCreateHandle(libusb_context* ctx, libusb_device_handle* dev_handle, 
                                  const ConnectionHandle& hint, ConnectionHandle& handle)
{
    libusb_device *device = libusb_get_device(dev_handle);
    libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(device, &desc) < 0) {
        lime::error("Failed to get device descriptor");
        return false;
    }

    // Verify it's a supported device
    bool isSupported = false;
    if (desc.idVendor == 1204 && desc.idProduct == 34323) {
        handle.name = "DigiGreen";
        isSupported = true;
    }
    else if ((desc.idVendor == 1204 && desc.idProduct == 241) || 
             (desc.idVendor == 1204 && desc.idProduct == 243) || 
             (desc.idVendor == 7504 && desc.idProduct == 24840)) {
        isSupported = true;
    }

    if (!isSupported) {
        lime::error("Device is not supported (VID: 0x%04x, PID: 0x%04x)", desc.idVendor, desc.idProduct);
        return false;
    }

    // Get device speed
    int speed = libusb_get_device_speed(device);
    if (speed == LIBUSB_SPEED_HIGH)
        handle.media = "USB 2.0";
    else if (speed == LIBUSB_SPEED_SUPER)
        handle.media = "USB 3.0";
    else
        handle.media = "USB";

    // Get device name
    char data[255];
    int r = libusb_get_string_descriptor_ascii(dev_handle, LIBUSB_CLASS_COMM, (unsigned char*)data, sizeof(data));
    if (r > 0) handle.name = std::string(data, size_t(r));

    // Get serial number if available
    if (desc.iSerialNumber > 0) {
        r = libusb_get_string_descriptor_ascii(dev_handle, desc.iSerialNumber, (unsigned char*)data, sizeof(data));
        if (r >= 0) {
            handle.serial = std::string(data, size_t(r));
        }
    }

    r = std::sprintf(data, "%.4x:%.4x", int(desc.idVendor), int(desc.idProduct));
    if (r > 0) handle.addr = std::string(data, size_t(r));

    // Check if serial matches hint (if provided)
    return hint.serial.empty() || handle.serial.find(hint.serial) != std::string::npos;
}
#endif // __UNIX__

//! make a static-initialized entry in the registry
void __loadConnectionFX3Entry(void) //TODO fixme replace with LoadLibrary/dlopen
{
static ConnectionFX3Entry FX3Entry;
}

ConnectionFX3Entry::ConnectionFX3Entry(const char* connectionName):
    ConnectionRegistryEntry(connectionName)
{
#ifdef __unix__
    int r = libusb_init(&ctx); //initialize the library for the session we just declared
    if(r < 0)
        lime::error("Init Error %i", r); //there was an error
#if LIBUSBX_API_VERSION < 0x01000106
    libusb_set_debug(ctx, 3); //set verbosity level to 3, as suggested in the documentation
#else
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, 3); //set verbosity level to 3, as suggested in the documentation
#endif
    mProcessUSBEvents.store(true);
    mUSBProcessingThread = std::thread(&ConnectionFX3Entry::handle_libusb_events, this);
    SetOSThreadPriority(ThreadPriority::NORMAL, ThreadPolicy::REALTIME, &mUSBProcessingThread);
#endif
}

ConnectionFX3Entry::ConnectionFX3Entry(void):
    ConnectionRegistryEntry("FX3")
{
#ifdef __unix__
    const char* termux_fd = getenv("TERMUX_USB_FD");
    if (termux_fd) {
        // Disable device discovery when using pre-opened FD
        libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    }
    int r = libusb_init(&ctx); //initialize the library for the session we just declared
    if(r < 0)
        lime::error("Init Error %i", r); //there was an error
#if LIBUSBX_API_VERSION < 0x01000106
    libusb_set_debug(ctx, 3); //set verbosity level to 3, as suggested in the documentation
#else
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, 3); //set verbosity level to 3, as suggested in the documentation
#endif
    mProcessUSBEvents.store(true);
    mUSBProcessingThread = std::thread(&ConnectionFX3Entry::handle_libusb_events, this);
    SetOSThreadPriority(ThreadPriority::NORMAL, ThreadPolicy::REALTIME, &mUSBProcessingThread);
#endif
}

ConnectionFX3Entry::~ConnectionFX3Entry(void)
{
#ifdef __unix__
    mProcessUSBEvents.store(false);
    mUSBProcessingThread.join();
    libusb_exit(ctx);
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
    const char* termux_fd = getenv("TERMUX_USB_FD");
    if (termux_fd) {
        // When using pre-opened FD, validate the device
        libusb_device_handle *tempDev_handle(nullptr);
        int fd = std::stoi(termux_fd);
        
        // Try to wrap the system device
        if (libusb_wrap_sys_device(ctx, fd, &tempDev_handle) != 0) {
            lime::error("Failed to wrap system device");
            return handles;
        }

        ConnectionHandle handle;
        handle.fd = fd;
        
        if (validateAndCreateHandle(ctx, tempDev_handle, hint, handle)) {
            handles.push_back(handle);
        }

        libusb_close(tempDev_handle);
        return handles;
    }

    libusb_device **devs; //pointer to pointer of device, used to retrieve a list of devices
    int usbDeviceCount = libusb_get_device_list(ctx, &devs);

    if (usbDeviceCount < 0) {
        lime::error("failed to get libusb device list: %s", libusb_strerror(libusb_error(usbDeviceCount)));
        return handles;
    }

    for(int i=0; i<usbDeviceCount; ++i)
    {
        libusb_device_handle *tempDev_handle(nullptr);
        if(libusb_open(devs[i], &tempDev_handle) != 0 || tempDev_handle == nullptr)
            continue;

        ConnectionHandle handle;
        if (validateAndCreateHandle(ctx, tempDev_handle, hint, handle)) {
            handles.push_back(handle);
        }

        libusb_close(tempDev_handle);
    }

    libusb_free_device_list(devs, 1);
#endif
    return handles;
}

IConnection *ConnectionFX3Entry::make(const ConnectionHandle &handle)
{
    return new ConnectionFX3(ctx, handle);
}
