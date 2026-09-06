#pragma once

#include "usb.h"
#include <furi_hal_usb.h>

#define USB_EP0_SIZE 8

/* String descriptors */
enum UsbDevDescStr {
    UsbDevLang = 0,
    UsbDevManuf = 1,
    UsbDevProduct = 2,
    UsbDevSerial = 3,
};

/* Composite (CDC + HID) hooks.
 *
 * The usb stack allows a single config/control callback per device, so the
 * combined driver in furi_hal_usb_cdc_hid.c cannot let the CDC and HID modules
 * each call usbd_reg_config()/usbd_reg_control() on their own. Instead each
 * module exposes an "attach" that wires up its static state (usb_dev, buffers,
 * string descriptors) against a shared device, plus its endpoint-config and
 * control-request handlers, which the composite driver invokes for the right
 * interfaces. The standalone CDC and HID modes are unaffected. */
void furi_hal_cdc_attach_to(usbd_device* dev, FuriHalUsbInterface* intf);
usbd_respond furi_hal_cdc_ep_config(usbd_device* dev, uint8_t cfg);
usbd_respond furi_hal_cdc_control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback);
void furi_hal_cdc_wakeup(usbd_device* dev);
void furi_hal_cdc_suspend(usbd_device* dev);

void furi_hal_hid_attach_to(usbd_device* dev, void* ctx);
usbd_respond furi_hal_hid_ep_config(usbd_device* dev, uint8_t cfg);
usbd_respond furi_hal_hid_control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback);
void furi_hal_hid_wakeup(usbd_device* dev);
void furi_hal_hid_suspend(usbd_device* dev);
uint16_t furi_hal_hid_report_desc_size(void);
