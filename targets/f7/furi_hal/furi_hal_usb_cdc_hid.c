// Composite USB device: CDC ACM serial + HID, live at the same time.
//
// Why: on the Flipper, switching to plain HID (usb_hid) tears down the CDC
// serial link, so a BadUSB run kills the CLI/serial bridge the moment it
// starts. This mode keeps ONE CDC ACM function (interfaces 0/1, the usual
// flip_<name> serial port) and adds a HID function (interface 2) beside it, so
// the phone/PC bridge and keyboard injection coexist.
//
// It does not re-implement CDC or HID. The two existing drivers expose small
// hooks (see furi_hal_usb_i.h): this file owns the single config descriptor and
// the single usbd config/control/wakeup/suspend callbacks the stack allows, and
// fans each one out to the CDC and HID machinery for the right interfaces. CDC
// keeps EP1 OUT / EP2 IN / EP3 IN; HID was moved to the free EP4 pair so the two
// never collide.

#include <furi_hal_version.h>
#include <furi_hal_usb_i.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <furi.h>

#include "usb.h"
#include "usb_cdc.h"
#include "usb_hid.h"

// Must match the endpoint assignments in furi_hal_usb_cdc.c / furi_hal_usb_hid.c
#define CDC0_RXD_EP 0x01
#define CDC0_TXD_EP 0x82
#define CDC0_NTF_EP 0x83
#define CDC_NTF_SZ  0x08

#define HID_EP_IN    0x84
#define HID_EP_OUT   0x04
#define HID_EP_SZ    0x10
#define HID_INTERVAL 2

#define HID_INTF_NUM 2

struct CdcIadDescriptor {
    struct usb_iad_descriptor comm_iad;
    struct usb_interface_descriptor comm;
    struct usb_cdc_header_desc cdc_hdr;
    struct usb_cdc_call_mgmt_desc cdc_mgmt;
    struct usb_cdc_acm_desc cdc_acm;
    struct usb_cdc_union_desc cdc_union;
    struct usb_endpoint_descriptor comm_ep;
    struct usb_interface_descriptor data;
    struct usb_endpoint_descriptor data_eprx;
    struct usb_endpoint_descriptor data_eptx;
};

struct HidIntfDescriptor {
    struct usb_interface_descriptor hid;
    struct usb_hid_descriptor hid_desc;
    struct usb_endpoint_descriptor hid_ep_in;
    struct usb_endpoint_descriptor hid_ep_out;
};

struct CdcHidConfigDescriptor {
    struct usb_config_descriptor config;
    struct CdcIadDescriptor cdc;
    struct HidIntfDescriptor hid;
} FURI_PACKED;

static const struct usb_string_descriptor dev_manuf_desc = USB_STRING_DESC("Flipper Devices Inc.");

/* Device descriptor: IAD so the host groups the two CDC interfaces; same
 * VID/PID as plain CDC so the serial port is recognised exactly as usual. */
static const struct usb_device_descriptor cdc_hid_device_desc = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DTYPE_DEVICE,
    .bcdUSB = VERSION_BCD(2, 0, 0),
    .bDeviceClass = USB_CLASS_IAD,
    .bDeviceSubClass = USB_SUBCLASS_IAD,
    .bDeviceProtocol = USB_PROTO_IAD,
    .bMaxPacketSize0 = USB_EP0_SIZE,
    .idVendor = 0x0483,
    .idProduct = 0x5740,
    .bcdDevice = VERSION_BCD(1, 0, 0),
    .iManufacturer = UsbDevManuf,
    .iProduct = UsbDevProduct,
    .iSerialNumber = UsbDevSerial,
    .bNumConfigurations = 1,
};

/* Combined config descriptor. Non-const: the HID report-descriptor length is
 * patched in at init from the HID driver so the two never drift. */
static struct CdcHidConfigDescriptor cdc_hid_cfg_desc = {
    .config =
        {
            .bLength = sizeof(struct usb_config_descriptor),
            .bDescriptorType = USB_DTYPE_CONFIGURATION,
            .wTotalLength = sizeof(struct CdcHidConfigDescriptor),
            .bNumInterfaces = 3,
            .bConfigurationValue = 1,
            .iConfiguration = NO_DESCRIPTOR,
            .bmAttributes = USB_CFG_ATTR_RESERVED | USB_CFG_ATTR_SELFPOWERED,
            .bMaxPower = USB_CFG_POWER_MA(500),
        },
    .cdc =
        {
            .comm_iad =
                {
                    .bLength = sizeof(struct usb_iad_descriptor),
                    .bDescriptorType = USB_DTYPE_INTERFASEASSOC,
                    .bFirstInterface = 0,
                    .bInterfaceCount = 2,
                    .bFunctionClass = USB_CLASS_CDC,
                    .bFunctionSubClass = USB_CDC_SUBCLASS_ACM,
                    .bFunctionProtocol = USB_PROTO_NONE,
                    .iFunction = NO_DESCRIPTOR,
                },
            .comm =
                {
                    .bLength = sizeof(struct usb_interface_descriptor),
                    .bDescriptorType = USB_DTYPE_INTERFACE,
                    .bInterfaceNumber = 0,
                    .bAlternateSetting = 0,
                    .bNumEndpoints = 1,
                    .bInterfaceClass = USB_CLASS_CDC,
                    .bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
                    .bInterfaceProtocol = USB_PROTO_NONE,
                    .iInterface = NO_DESCRIPTOR,
                },
            .cdc_hdr =
                {
                    .bFunctionLength = sizeof(struct usb_cdc_header_desc),
                    .bDescriptorType = USB_DTYPE_CS_INTERFACE,
                    .bDescriptorSubType = USB_DTYPE_CDC_HEADER,
                    .bcdCDC = VERSION_BCD(1, 1, 0),
                },
            .cdc_mgmt =
                {
                    .bFunctionLength = sizeof(struct usb_cdc_call_mgmt_desc),
                    .bDescriptorType = USB_DTYPE_CS_INTERFACE,
                    .bDescriptorSubType = USB_DTYPE_CDC_CALL_MANAGEMENT,
                    .bmCapabilities = 0,
                    .bDataInterface = 1,
                },
            .cdc_acm =
                {
                    .bFunctionLength = sizeof(struct usb_cdc_acm_desc),
                    .bDescriptorType = USB_DTYPE_CS_INTERFACE,
                    .bDescriptorSubType = USB_DTYPE_CDC_ACM,
                    .bmCapabilities = 0,
                },
            .cdc_union =
                {
                    .bFunctionLength = sizeof(struct usb_cdc_union_desc),
                    .bDescriptorType = USB_DTYPE_CS_INTERFACE,
                    .bDescriptorSubType = USB_DTYPE_CDC_UNION,
                    .bMasterInterface0 = 0,
                    .bSlaveInterface0 = 1,
                },
            .comm_ep =
                {
                    .bLength = sizeof(struct usb_endpoint_descriptor),
                    .bDescriptorType = USB_DTYPE_ENDPOINT,
                    .bEndpointAddress = CDC0_NTF_EP,
                    .bmAttributes = USB_EPTYPE_INTERRUPT,
                    .wMaxPacketSize = CDC_NTF_SZ,
                    .bInterval = 0xFF,
                },
            .data =
                {
                    .bLength = sizeof(struct usb_interface_descriptor),
                    .bDescriptorType = USB_DTYPE_INTERFACE,
                    .bInterfaceNumber = 1,
                    .bAlternateSetting = 0,
                    .bNumEndpoints = 2,
                    .bInterfaceClass = USB_CLASS_CDC_DATA,
                    .bInterfaceSubClass = USB_SUBCLASS_NONE,
                    .bInterfaceProtocol = USB_PROTO_NONE,
                    .iInterface = NO_DESCRIPTOR,
                },
            .data_eprx =
                {
                    .bLength = sizeof(struct usb_endpoint_descriptor),
                    .bDescriptorType = USB_DTYPE_ENDPOINT,
                    .bEndpointAddress = CDC0_RXD_EP,
                    .bmAttributes = USB_EPTYPE_BULK,
                    .wMaxPacketSize = CDC_DATA_SZ,
                    .bInterval = 0x01,
                },
            .data_eptx =
                {
                    .bLength = sizeof(struct usb_endpoint_descriptor),
                    .bDescriptorType = USB_DTYPE_ENDPOINT,
                    .bEndpointAddress = CDC0_TXD_EP,
                    .bmAttributes = USB_EPTYPE_BULK,
                    .wMaxPacketSize = CDC_DATA_SZ,
                    .bInterval = 0x01,
                },
        },
    .hid =
        {
            .hid =
                {
                    .bLength = sizeof(struct usb_interface_descriptor),
                    .bDescriptorType = USB_DTYPE_INTERFACE,
                    .bInterfaceNumber = HID_INTF_NUM,
                    .bAlternateSetting = 0,
                    .bNumEndpoints = 2,
                    .bInterfaceClass = USB_CLASS_HID,
                    .bInterfaceSubClass = USB_HID_SUBCLASS_BOOT,
                    .bInterfaceProtocol = USB_HID_PROTO_KEYBOARD,
                    .iInterface = NO_DESCRIPTOR,
                },
            .hid_desc =
                {
                    .bLength = sizeof(struct usb_hid_descriptor),
                    .bDescriptorType = USB_DTYPE_HID,
                    .bcdHID = VERSION_BCD(1, 0, 0),
                    .bCountryCode = USB_HID_COUNTRY_NONE,
                    .bNumDescriptors = 1,
                    .bDescriptorType0 = USB_DTYPE_HID_REPORT,
                    .wDescriptorLength0 = 0, /* patched at init */
                },
            .hid_ep_in =
                {
                    .bLength = sizeof(struct usb_endpoint_descriptor),
                    .bDescriptorType = USB_DTYPE_ENDPOINT,
                    .bEndpointAddress = HID_EP_IN,
                    .bmAttributes = USB_EPTYPE_INTERRUPT,
                    .wMaxPacketSize = HID_EP_SZ,
                    .bInterval = HID_INTERVAL,
                },
            .hid_ep_out =
                {
                    .bLength = sizeof(struct usb_endpoint_descriptor),
                    .bDescriptorType = USB_DTYPE_ENDPOINT,
                    .bEndpointAddress = HID_EP_OUT,
                    .bmAttributes = USB_EPTYPE_INTERRUPT,
                    .wMaxPacketSize = HID_EP_SZ,
                    .bInterval = HID_INTERVAL,
                },
        },
};

static void cdc_hid_init(usbd_device* dev, FuriHalUsbInterface* intf, void* ctx);
static void cdc_hid_deinit(usbd_device* dev);
static void cdc_hid_on_wakeup(usbd_device* dev);
static void cdc_hid_on_suspend(usbd_device* dev);

FuriHalUsbInterface usb_cdc_hid = {
    .init = cdc_hid_init,
    .deinit = cdc_hid_deinit,
    .wakeup = cdc_hid_on_wakeup,
    .suspend = cdc_hid_on_suspend,

    .dev_descr = (struct usb_device_descriptor*)&cdc_hid_device_desc,

    .str_manuf_descr = (void*)&dev_manuf_desc,
    .str_prod_descr = NULL,
    .str_serial_descr = NULL,

    .cfg_descr = (void*)&cdc_hid_cfg_desc,
};

static usbd_respond cdc_hid_ep_config(usbd_device* dev, uint8_t cfg) {
    if(furi_hal_cdc_ep_config(dev, cfg) != usbd_ack) {
        return usbd_fail;
    }
    if(furi_hal_hid_ep_config(dev, cfg) != usbd_ack) {
        return usbd_fail;
    }
    return usbd_ack;
}

static usbd_respond
    cdc_hid_control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback) {
    // Interface-directed requests for the HID interface go to the HID handler.
    // The HID driver checks for its interface as index 0 (it only ever had one
    // interface), so temporarily present the request that way.
    if(((req->bmRequestType & USB_REQ_RECIPIENT) == USB_REQ_INTERFACE) &&
       (req->wIndex == HID_INTF_NUM)) {
        uint16_t saved = req->wIndex;
        req->wIndex = 0;
        usbd_respond res = furi_hal_hid_control(dev, req, callback);
        req->wIndex = saved;
        return res;
    }

    // Everything else (CDC class requests on interface 0) goes to CDC.
    return furi_hal_cdc_control(dev, req, callback);
}

static void cdc_hid_init(usbd_device* dev, FuriHalUsbInterface* intf, void* ctx) {
    UNUSED(ctx);

    // Keep the advertised HID report length in step with the actual descriptor.
    cdc_hid_cfg_desc.hid.hid_desc.wDescriptorLength0 = furi_hal_hid_report_desc_size();

    // CDC owns the product/serial string descriptors (built from the device
    // name); attach it against this composite interface so they land on us.
    furi_hal_cdc_attach_to(dev, intf);
    // HID just needs its static state (usb_dev, semaphore, report ids) wired up;
    // the composite device descriptor above supplies VID/PID, so pass no config.
    furi_hal_hid_attach_to(dev, NULL);

    usbd_reg_config(dev, cdc_hid_ep_config);
    usbd_reg_control(dev, cdc_hid_control);

    usbd_connect(dev, true);
}

static void cdc_hid_deinit(usbd_device* dev) {
    usbd_reg_config(dev, NULL);
    usbd_reg_control(dev, NULL);

    // CDC allocated these onto our interface in furi_hal_cdc_attach_to().
    free(usb_cdc_hid.str_prod_descr);
    free(usb_cdc_hid.str_serial_descr);
    usb_cdc_hid.str_prod_descr = NULL;
    usb_cdc_hid.str_serial_descr = NULL;
}

static void cdc_hid_on_wakeup(usbd_device* dev) {
    furi_hal_cdc_wakeup(dev);
    furi_hal_hid_wakeup(dev);
}

static void cdc_hid_on_suspend(usbd_device* dev) {
    furi_hal_cdc_suspend(dev);
    furi_hal_hid_suspend(dev);
}
