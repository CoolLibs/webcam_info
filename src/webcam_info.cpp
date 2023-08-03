#include <filesystem>
#include <iostream>
#include <string>
#include <variant>
#include <vector>
#include <webcam_info/webcam_info.hpp>

auto webcam_info::to_string(webcam_info::pixel_format format) -> std::string
{
    switch (format)
    {
    case webcam_info::pixel_format::yuyv:
        return "yuyv";

    case webcam_info::pixel_format::mjpeg:
        return "mjpeg";

    default:
        return "unknown";
    }
}

#if defined(_WIN32)

#include <dshow.h>
#include <codecvt>

std::string ConvertWCharToString(const wchar_t* wcharStr)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(wcharStr);
}

HRESULT GetVideoParameters(IBaseFilter* pCaptureFilter, int& width, int& height, webcam_info::pixel_format& pixel_format)
{
    HRESULT        hr         = S_OK;
    IEnumPins*     pEnumPins  = NULL;
    IPin*          pPin       = NULL;
    AM_MEDIA_TYPE* pMediaType = NULL;

    // Trouver la première sortie vidéo du filtre d'entrée
    hr = pCaptureFilter->EnumPins(&pEnumPins);
    if (SUCCEEDED(hr))
    {
        while (pEnumPins->Next(1, &pPin, NULL) == S_OK)
        {
            PIN_DIRECTION pinDirection;
            pPin->QueryDirection(&pinDirection);

            if (pinDirection == PINDIR_OUTPUT)
            {
                // Obtenir l'interface IAMStreamConfig pour manipuler les paramètres de capture
                IAMStreamConfig* pStreamConfig = NULL;
                hr                             = pPin->QueryInterface(IID_PPV_ARGS(&pStreamConfig));
                if (SUCCEEDED(hr))
                {
                    int iCount = 0, iSize = 0;
                    hr = pStreamConfig->GetNumberOfCapabilities(&iCount, &iSize);
                    if (SUCCEEDED(hr))
                    {
                        // Obtenir les capacités de capture (résolutions)
                        VIDEO_STREAM_CONFIG_CAPS caps;
                        for (int i = 0; i < iCount; i++)
                        {
                            AM_MEDIA_TYPE* pmtConfig;
                            hr = pStreamConfig->GetStreamCaps(i, &pmtConfig, (BYTE*)&caps);
                            if (SUCCEEDED(hr))
                            {
                                if (pmtConfig->formattype == FORMAT_VideoInfo)
                                {
                                    VIDEOINFOHEADER* pVih = reinterpret_cast<VIDEOINFOHEADER*>(pmtConfig->pbFormat);
                                    width                 = max(width, pVih->bmiHeader.biWidth);
                                    height                = max(height, pVih->bmiHeader.biHeight);
                                    // break;
                                }
                                if (pmtConfig->subtype == MEDIASUBTYPE_YUY2)
                                    pixel_format = webcam_info::pixel_format::yuyv;
                                else if (pmtConfig->subtype == MEDIASUBTYPE_MJPG)
                                    pixel_format = webcam_info::pixel_format::mjpeg;
                                else
                                    pixel_format = webcam_info::pixel_format::unknown;
                            }
                        }
                    }
                    pStreamConfig->Release();
                }
                pPin->Release();
                break;
            }
            pPin->Release();
        }
        pEnumPins->Release();
    }

    return hr;
}

auto get_devices_info(IEnumMoniker* pEnum) -> std::vector<webcam_info::info>
{
    std::vector<webcam_info::info> list_webcam_info{};
    IMoniker*                      pMoniker = NULL;

    while (pEnum->Next(1, &pMoniker, NULL) == S_OK)
    {
        int                       width{};
        int                       height{};
        webcam_info::pixel_format pixel_format{};

        IPropertyBag* pPropBag;
        HRESULT       hr = pMoniker->BindToStorage(0, 0, IID_PPV_ARGS(&pPropBag));
        if (FAILED(hr))
        {
            pMoniker->Release();
            continue;
        }

        VARIANT var;
        VariantInit(&var);

        // Get description or friendly name.
        hr = pPropBag->Read(L"Description", &var, 0);
        if (FAILED(hr))
        {
            hr = pPropBag->Read(L"FriendlyName", &var, 0);
        }
        if (SUCCEEDED(hr))
        {
            IBaseFilter* pCaptureFilter = NULL;
            hr                          = pMoniker->BindToObject(0, 0, IID_PPV_ARGS(&pCaptureFilter));
            if (SUCCEEDED(hr))
            {
                hr = GetVideoParameters(pCaptureFilter, width, height, pixel_format);
                pCaptureFilter->Release();

                if (SUCCEEDED(hr))
                {
                    list_webcam_info.push_back(webcam_info::info{ConvertWCharToString(var.bstrVal), width, height, pixel_format});
                }

                // printf("%S\n", var.bstrVal);

                VariantClear(&var);
            }

            hr = pPropBag->Write(L"FriendlyName", &var);

            pPropBag->Release();
            pMoniker->Release();
        }
    }
    return list_webcam_info;
}

auto EnumerateDevices(REFGUID category, IEnumMoniker** ppEnum) -> HRESULT
{
    // Create the System Device Enumerator.
    ICreateDevEnum* pDevEnum;
    HRESULT         hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDevEnum));

    if (SUCCEEDED(hr))
    {
        // Create an enumerator for the category.
        hr = pDevEnum->CreateClassEnumerator(category, ppEnum, 0);
        if (hr == S_FALSE)
        {
            hr = VFW_E_NOT_FOUND; // The category is empty. Treat as an error.
        }
        pDevEnum->Release();
    }
    return hr;
}

auto webcam_info::get_all_webcams() -> std::vector<info>
{
    std::vector<info> list_webcam_info{};
    HRESULT           hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr))
    {
        IEnumMoniker* pEnum;

        hr = EnumerateDevices(CLSID_VideoInputDeviceCategory, &pEnum);
        if (SUCCEEDED(hr))
        {
            list_webcam_info = get_devices_info(pEnum);
            pEnum->Release();
        }
        CoUninitialize();
    }
    return list_webcam_info;
}

#endif

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>

auto webcam_info::get_all_webcams() -> std::vector<info>
{
    std::vector<info> list_webcam_info{};

    std::vector<std::string> list_camera_path{};
    std::string              camera_path = "/dev/video0";
    for (const auto& entry : std::filesystem::directory_iterator("/dev"))
    {
        if (entry.path().string().find("video") == std::string::npos)
            continue;

        int video_device = open(entry.path().c_str(), O_RDONLY);
        if (video_device == -1)
            continue;

        v4l2_capability cap{};

        if (ioctl(video_device, VIDIOC_QUERYCAP, &cap) == -1)
        {
            std::cout << "Erreur lors de l'obtention des informations du périphérique";
            continue;
        }

        // Vérification si le périphérique est capable de capturer des flux vidéo
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
        {
            std::cout << "Le périphérique n'est pas capable de capturer des flux vidéo\n";
            continue;
        }

        // Récupération du nom du périphérique
        char deviceName[256];
        strcpy(deviceName, (char*)cap.card);

        int          width{};
        int          height{};
        pixel_format format{};

        v4l2_fmtdesc formatDescription{};
        formatDescription.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        while (ioctl(video_device, VIDIOC_ENUM_FMT, &formatDescription) == 0)
        {
            printf("  - Format : %s\n", formatDescription.description);
            // Récupérer les résolutions associées à chaque format
            v4l2_frmsizeenum frameSize{};
            frameSize.pixel_format = formatDescription.pixelformat;
            while (ioctl(video_device, VIDIOC_ENUM_FRAMESIZES, &frameSize) == 0)
            {
                v4l2_frmivalenum frameInterval{};
                frameInterval.pixel_format = formatDescription.pixelformat;
                frameInterval.width        = frameSize.discrete.width;
                frameInterval.height       = frameSize.discrete.height;

                while (ioctl(video_device, VIDIOC_ENUM_FRAMEINTERVALS, &frameInterval) == 0)
                {
                    if (frameInterval.type == V4L2_FRMIVAL_TYPE_DISCRETE)
                    {
                        float fps = 1.0f * frameInterval.discrete.denominator / frameInterval.discrete.numerator;
                        if (fps > 29. && frameSize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                        {
                            width  = std::max(width, static_cast<int>(frameSize.discrete.width));
                            height = std::max(height, static_cast<int>(frameSize.discrete.height));
                            std::string format_name(reinterpret_cast<char*>(formatDescription.description), 32);

                            if (format_name.find("Motion-JPEG") != std::string::npos)
                                format = pixel_format::mjpeg;

                            else if (format_name.find("YUYV") != std::string::npos)
                                format = pixel_format::yuyv;
                            else
                                format = pixel_format::unknown;
                        }
                    }
                    frameInterval.index++;
                }
                frameSize.index++;
            }

            formatDescription.index++;
        }
        if (width <= 0 || height <= 0)
            continue;
        list_webcam_info.push_back(info{std::string(deviceName), width, height, format});
    }
    return list_webcam_info;
}

#endif

// #if defined(__APPLE__)
#include <CoreMediaIO/CMIOFormats.h>
#include <CoreMediaIO/CMIOHardware.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

auto webcam_info::get_all_webcams() -> std::vector<info>
{
    std::vector<info> list_webcams_infos{};

    kern_return_t kr;
    io_iterator_t iterator;
    kr = IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching(kIOUSBInterfaceClassName), &iterator);

    if (kr != KERN_SUCCESS)
    {
        std::cerr << "Failed to find any USB interfaces." << std::endl;
        return list_webcams_infos;
    }

    io_service_t usbDevice;
    while ((usbDevice = IOIteratorNext(iterator)))
    {
        std::string webcam_name{};
        int         width{};
        int         height{};

        pixel_format format{pixel_format::unknown};

        CFMutableDictionaryRef properties = NULL;
        kr                                = IORegistryEntryCreateCFProperties(usbDevice, &properties, kCFAllocatorDefault, kNilOptions);

        if (kr == KERN_SUCCESS)
        {
            CFStringRef productName = (CFStringRef)CFDictionaryGetValue(properties, CFSTR(kUSBProductString));
            if (productName)
            {
                char productNameBuf[256];
                if (CFStringGetCString(productName, productNameBuf, sizeof(productNameBuf), kCFStringEncodingUTF8))
                {
                    std::cout << "Webcam: " << productNameBuf << std::endl;
                    webcam_name = std::string(productName);
                }
            }

            CFNumberRef vendorID  = (CFNumberRef)CFDictionaryGetValue(properties, CFSTR(kUSBVendorID));
            CFNumberRef productID = (CFNumberRef)CFDictionaryGetValue(properties, CFSTR(kUSBProductID));

            int32_t vendorIDValue, productIDValue;
            if (vendorID && productID && CFNumberGetValue(vendorID, kCFNumberSInt32Type, &vendorIDValue) && CFNumberGetValue(productID, kCFNumberSInt32Type, &productIDValue))
            {
                std::cout << "Vendor ID: 0x" << std::hex << vendorIDValue << std::dec << std::endl;
                std::cout << "Product ID: 0x" << std::hex << productIDValue << std::dec << std::endl;
            }

            CFRelease(properties);
        }

        // Get video stream format descriptions
        io_iterator_t formatIterator;
        kr = IORegistryEntryGetChildIterator(usbDevice, kIOServicePlane, &formatIterator);

        if (kr == KERN_SUCCESS)
        {
            io_service_t videoFormat;
            while ((videoFormat = IOIteratorNext(formatIterator)))
            {
                CFMutableDictionaryRef formatProperties = NULL;
                kr                                      = IORegistryEntryCreateCFProperties(videoFormat, &formatProperties, kCFAllocatorDefault, kNilOptions);

                if (kr == KERN_SUCCESS)
                {
                    CFStringRef formatType = (CFStringRef)CFDictionaryGetValue(formatProperties, CFSTR(kIOStreamMode));

                    if (formatType && CFStringCompare(formatType, CFSTR(kIOStreamModeVideo), 0) == kCFCompareEqualTo)
                    {
                        CFNumberRef formatWidth  = (CFNumberRef)CFDictionaryGetValue(formatProperties, CFSTR(kIOStreamModeFrameWidthKey));
                        CFNumberRef formatHeight = (CFNumberRef)CFDictionaryGetValue(formatProperties, CFSTR(kIOStreamModeFrameHeightKey));

                        if (formatWidth && formatHeight && CFNumberGetValue(formatWidth, kCFNumberSInt32Type, &width) && CFNumberGetValue(formatHeight, kCFNumberSInt32Type, &height))
                        {
                            std::cout << "Resolution: " << width << "x" << height << std::endl;
                        }
                    }

                    CFRelease(formatProperties);
                }

                IOObjectRelease(videoFormat);
            }

            IOObjectRelease(formatIterator);
        }

        IOObjectRelease(usbDevice);

        list_webcams_infos.push_back(info{webcam_name, width, height});
    }

    IOObjectRelease(iterator);

    return list_webcams_infos;
}

// #endif