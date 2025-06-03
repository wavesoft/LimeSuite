/**
    @file Connection_uLimeSDREntry.cpp
    @author Lime Microsystems
    @brief Implementation of uLimeSDR board connection.
*/

#include "ConnectionFT601.h"
#include "Logger.h"
#include "threadHelper.h"
using namespace lime;

#ifdef __unix__
void ConnectionFT601Entry::handle_libusb_events()
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

    // Verify it's an FTDI device (VID 0x0403) and FT601 (PID 0x601F)
    if (desc.idVendor != 0x0403 || desc.idProduct != 0x601F) {
        lime::error("Device is not an FT601 (VID: 0x%04x, PID: 0x%04x)", desc.idVendor, desc.idProduct);
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

    // Get serial number if available
    if (desc.iSerialNumber > 0) {
        char data[255];
        int r = libusb_get_string_descriptor_ascii(dev_handle, desc.iSerialNumber, (unsigned char*)data, sizeof(data));
        if (r >= 0) {
            handle.serial = std::string(data, size_t(r));
        }
    }

    handle.name = "FT601 Device";
    handle.addr = std::to_string(desc.idProduct) + ":" + std::to_string(desc.idVendor);

    // Check if serial matches hint (if provided)
    return hint.serial.empty() || handle.serial.find(hint.serial) != std::string::npos;
}
#endif // __UNIX__

//! make a static-initialized entry in the registry
void __loadConnectionFT601Entry(void) //TODO fixme replace with LoadLibrary/dlopen
{
    static ConnectionFT601Entry FTDIEntry;
}

ConnectionFT601Entry::ConnectionFT601Entry(void):
    ConnectionRegistryEntry("FT601")
{
#ifndef __unix__
    //m_pDriver = new CDriverInterface();
#else
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
    mUSBProcessingThread = std::thread(&ConnectionFT601Entry::handle_libusb_events, this);
    SetOSThreadPriority(ThreadPriority::NORMAL, ThreadPolicy::REALTIME, &mUSBProcessingThread);
#endif
}

ConnectionFT601Entry::~ConnectionFT601Entry(void)
{
#ifndef __unix__
    //delete m_pDriver;
#else
    mProcessUSBEvents.store(false);
    mUSBProcessingThread.join();
    libusb_exit(ctx);
#endif
}

std::vector<ConnectionHandle> ConnectionFT601Entry::enumerate(const ConnectionHandle &hint)
{
    std::vector<ConnectionHandle> handles;

#ifndef __unix__
    FT_STATUS ftStatus=FT_OK;
    static DWORD numDevs = 0;

    ftStatus = FT_CreateDeviceInfoList(&numDevs);

    if (!FT_FAILED(ftStatus) && numDevs > 0)
    {
        DWORD Flags = 0;
        char SerialNumber[16] = { 0 };
        char Description[32] = { 0 };
        for (DWORD i = 0; i < numDevs; i++)
        {
            ftStatus = FT_GetDeviceInfoDetail(i, &Flags, nullptr, nullptr, nullptr, SerialNumber, Description, nullptr);
            if (!FT_FAILED(ftStatus))
            {
                ConnectionHandle handle;
                handle.media = Flags & FT_FLAGS_SUPERSPEED ? "USB 3" : Flags & FT_FLAGS_HISPEED ? "USB 2" : "USB";
                handle.name = Description;
                handle.index = i;
                handle.serial = SerialNumber;
                //add handle conditionally, filter by serial number
                if (hint.serial.empty() || handle.serial.find(hint.serial) != std::string::npos)
                    handles.push_back(handle);
            }
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

IConnection *ConnectionFT601Entry::make(const ConnectionHandle &handle)
{
#ifndef __unix__
    return new ConnectionFT601(mFTHandle, handle);
#else
    return new ConnectionFT601(ctx, handle);
#endif
}
